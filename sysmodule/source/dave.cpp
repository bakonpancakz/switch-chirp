#include "dave.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {


void be64(uint64_t v, uint8_t out[8]) {
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(v >> (56 - 8 * i));
}

}  // namespace

DaveRatchet* DaveSession::Epoch::forUser(uint64_t user) {
    for (size_t i = 0; i < ratchets_n; i++)
        if (ratchets[i].user == user) return &ratchets[i];
    // (a speaker's first frame this epoch: their ratchet, made from the exporter; the oldest goes when full)
    size_t slot = ratchets_n < DAVE_RATCHETS ? ratchets_n++ : (size_t)(user % DAVE_RATCHETS);
    uint8_t base[DAVE_KEY];
    if (!daveBaseSecret(exporter, user, base)) return nullptr;
    ratchets[slot].init(user, base);
    wipe(base, sizeof(base));
    return &ratchets[slot];
}

void DaveSession::say(const char* fmt, ...) {
    if (!out_) return;
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    out_->log(line);
}

// (off: DAVE works. On, every message to and from the gateway goes into the log in hex, for
// test/interop/fromlog.mjs; dozens of lines a join)
constexpr bool DUMP = false;

void DaveSession::dump(const char* what, const uint8_t* p, size_t n) {
    if (!DUMP) return;
    static const char* digits = "0123456789abcdef";
    say("DAVE hex %s: %u bytes", what, (unsigned)n);
    for (size_t at = 0; at < n; at += 64) {
        char line[160];
        size_t k = 0;
        k += snprintf(line, sizeof(line), "DAVE hex %s+%u ", what, (unsigned)at);
        for (size_t i = at; i < n && i < at + 64 && k + 3 < sizeof(line); i++) {
            line[k++] = digits[p[i] >> 4];
            line[k++] = digits[p[i] & 15];
        }
        line[k] = 0;
        out_->log(line);
    }
}

void DaveSession::start(uint64_t me, uint64_t channel, DaveOut* out, MlsGroup::Random rnd) {
    me_ = me, channel_ = channel, out_ = out, rnd_ = rnd;
    version_ = 0, pending_version_ = -1;
    have_pending_ = false;
    external_n_ = 0;
    group_.reset();
    current_.clear(), previous_.clear(), next_.clear();
    passthrough_until_ = UINT64_MAX;  // (until DAVE is on, frames are as they're sent)
    users_n_ = 0;
    failures_ = 0;
    silence_ = plain_ = opened_ = 0;
}

void DaveSession::sendKeyPackage() {
    uint8_t id[8];
    be64(me_, id);  // (the credential: the user id, big-endian)
    static uint8_t kp[2048];
    Writer w(kp, sizeof(kp));
    if (!group_.makeKeyPackage(id, 8, rnd_, w)) {
        say("DAVE: couldn't make a key package");
        return;
    }
    // (the KeyPackage itself, without the MLSMessage around it: as libdave and davey send it, and
    // the only way the gateway takes it, though DAVE's paper writes MLSMessage there)
    out_->binary(DaveOp_KeyPackage, kp + 4, w.n - 4);
    dump("op26", kp + 4, w.n - 4);
    say("DAVE: key package sent (%u bytes)", (unsigned)w.n);
}

// A group of our own, with only us in it, once we know the gateway (as external sender) and the
// protocol: the group is real once the gateway announces its first commit
void DaveSession::createIfReady() {
    if (version_ <= 0 || !external_n_ || group_.joined()) return;
    static uint8_t ext[600];
    Writer e(ext, sizeof(ext));
    e.u16(5);  // external_senders
    size_t data = e.begin();
    e.vec(external_, external_n_);  // (a list of one)
    e.end(data);
    uint8_t gid[8];
    be64(channel_, gid);
    if (!e.ok || !group_.create(Bytes{gid, 8}, e.bytes(), rnd_)) say("DAVE: couldn't create the group");
}

void DaveSession::resetGroup() {
    group_.reset();
    next_.clear();
}

void DaveSession::invalid(uint32_t transition) {
    char msg[96];
    snprintf(msg, sizeof(msg), "{\"op\":%d,\"d\":{\"transition_id\":%u}}", DaveOp_InvalidCommitWelcome, transition);
    out_->json(msg);
    resetGroup();
    sendKeyPackage();
    createIfReady();
}

void DaveSession::transitionReady(uint32_t transition, uint64_t now_ms) {
    if (transition == 0) {
        execute(now_ms);  // (0: at once, nothing to wait for)
        return;
    }
    pending_transition_ = transition;
    have_pending_ = true;
    char msg[96];
    snprintf(msg, sizeof(msg), "{\"op\":%d,\"d\":{\"transition_id\":%u}}", DaveOp_TransitionReady, transition);
    out_->json(msg);
    say("DAVE: ready for transition %u", transition);
}

void DaveSession::execute(uint64_t now_ms) {
    int was = version_;
    if (pending_version_ >= 0) version_ = pending_version_;
    pending_version_ = -1;
    have_pending_ = false;
    if (next_.used) {
        if (current_.used) {
            previous_ = current_;
            previous_.expires_ms = now_ms + DAVE_KEEP_OLD_MS;
        }
        current_ = next_;
        current_.expires_ms = 0;
        next_.clear();
    }
    // (into DAVE: unencrypted frames still open for a while; out of it: for good)
    if (version_ == 0) passthrough_until_ = UINT64_MAX;
    else if (was == 0) passthrough_until_ = now_ms + DAVE_KEEP_OLD_MS;
    say("DAVE: now protocol %d, epoch %llu", version_, (unsigned long long)group_.epoch());
}

void DaveSession::onProtocol(int version, uint64_t now_ms) {
    version_ = version;
    say("DAVE: protocol %d at the start", version);
    if (version > 0) {
        passthrough_until_ = now_ms + DAVE_KEEP_OLD_MS;
        sendKeyPackage();
        createIfReady();
    }
}

void DaveSession::onPrepareTransition(int version, uint32_t transition, uint64_t now_ms) {
    say("DAVE: transition %u to protocol %d coming", transition, version);
    pending_version_ = version;
    if (version == 0) passthrough_until_ = UINT64_MAX;  // (a downgrade: frames may come plain from now)
    transitionReady(transition, now_ms);
}

void DaveSession::onExecuteTransition(uint32_t transition, uint64_t now_ms) {
    say("DAVE: transition %u executed", transition);
    if (have_pending_ && transition != pending_transition_) say("DAVE: executing transition %u (waiting for %u)", transition, pending_transition_);
    execute(now_ms);
}

void DaveSession::onPrepareEpoch(int version, uint64_t epoch, uint64_t now_ms) {
    say("DAVE: epoch %llu coming (protocol %d)", (unsigned long long)epoch, version);
    if (epoch != 1) return;  // (a new group: start over)
    version_ = version;
    resetGroup();
    sendKeyPackage();
    createIfReady();
    (void)now_ms;
}

void DaveSession::onClientsConnect(const uint64_t* users, size_t n) {
    for (size_t i = 0; i < n; i++) {
        bool known = false;
        for (size_t k = 0; k < users_n_; k++) known = known || users_[k] == users[i];
        if (!known && users_n_ < DAVE_USERS) users_[users_n_++] = users[i];
    }
}

void DaveSession::onClientDisconnect(uint64_t user) {
    for (size_t k = 0; k < users_n_; k++)
        if (users_[k] == user) users_[k] = users_[--users_n_];
}

void DaveSession::onBinary(uint8_t op, const uint8_t* body, size_t n, uint64_t now_ms) {
    say("DAVE: binary opcode %u came (%u bytes)%s", op, (unsigned)n, group_.joined() ? "" : ", not in a group yet");
    char what[8];
    snprintf(what, sizeof(what), "op%u", op);
    dump(what, body, n);
    Reader r(body, n);
    if (op == DaveOp_ExternalSender) {
        if (n > sizeof(external_)) return say("DAVE: the external sender is too big");
        memcpy(external_, body, n);
        external_n_ = n;
        createIfReady();
        return;
    }
    if (op == DaveOp_Proposals) {
        if (!group_.joined()) return;  // (no group yet: nothing to apply them to)
        uint8_t kind = r.u8();
        Reader list = r.sub();
        if (kind == 0 && group_.epoch() == 0 && list.n) {
            // (a group only we are in so far, under the id we guessed: a Go Live stream's group isn't its
            // call's channel. The proposals say the real one in the clear (PublicMessage: version,
            // wire format, then the group id): started again as that)
            Reader m(list.p, list.n);
            m.u16(), m.u16();
            Bytes gid = m.vec();
            uint8_t mine[8];
            be64(channel_, mine);
            if (m.ok && gid.n == 8 && !(gid == Bytes{mine, 8})) {
                uint64_t id = 0;
                for (int i = 0; i < 8; i++) id = id << 8 | gid.p[i];
                say("DAVE: the group is %llu, not %llu: made again as it", (unsigned long long)id, (unsigned long long)channel_);
                channel_ = id;
                resetGroup();
                createIfReady();
            }
        }
        if (kind == 0) {
            while (list.n && list.ok) {
                size_t used = 0;
                MlsResult res = group_.handle(Bytes{list.p, list.n}, &used);
                if (res != Mls_Ok) say("DAVE: a proposal didn't take (%d, mlsgroup.cpp:%d)", res, group_.whyLine());
                if (!used) break;
                list.take(used);
            }
        } else if (kind == 1) {
            while (list.n && list.ok) group_.revokeProposal(list.vec());
        }
        if (!group_.pendingProposals()) return;
        // every member commits what's cached (DAVE v1): the gateway takes the first commit it gets
        // (one writer for both: commit() finishes the commit before it starts the Welcome, so the
        // Welcome lands right after it, as op 28 wants them)
        // (our commit and, right after it, its Welcome: op 28 is the two back to back. The Welcome is a
        // GroupInfo with the whole tree in it, and a secret for each new member)
        size_t cap = 0;
        uint8_t* out = out_->scratch(&cap);
        Writer c(out, cap);
        bool welcome = false;
        MlsResult res = group_.commit(rnd_, c, c, &welcome);
        if (res != Mls_Ok) return say("DAVE: couldn't commit (%d, mlsgroup.cpp:%d)", res, group_.whyLine());
        if (!c.ok) return say("DAVE: the commit is too big to send");
        out_->binary(DaveOp_CommitWelcome, out, c.n);
        dump("op28", out, c.n);
        say("DAVE: commit sent (%u bytes%s), epoch %llu, our user %llu", (unsigned)c.n, welcome ? ", with a Welcome" : "",
            (unsigned long long)group_.epoch(), (unsigned long long)me_);
        const uint64_t* ids;
        size_t added = group_.added(&ids);
        for (size_t i = 0; i < added; i++) say("DAVE: the commit adds user %llu", (unsigned long long)ids[i]);
        return;
    }
    if (op == DaveOp_AnnounceCommit || op == DaveOp_Welcome) {
        uint32_t transition = r.u16();
        Bytes rest{r.p, r.n};
        if (!r.ok) return;
        MlsResult res;
        if (op == DaveOp_AnnounceCommit) {
            if (!group_.joined()) return;  // (we're not in it: a Welcome will come)
            res = group_.handle(rest);
        } else {
            res = group_.join(rest);
            // (the group must be this call's, run by the gateway we know)
            uint8_t gid[8];
            be64(channel_, gid);
            Bytes ext{external_, external_n_};
            Reader er(ext);
            Bytes key = er.vec();
            // (a Go Live stream's group isn't its call's channel, and Discord doesn't say it up front: a
            // Welcome from the gateway we know names it, and it's taken)
            Bytes wid = group_.groupId();
            if (res == Mls_Ok && group_.externalSender() == key && wid.n == 8 && !(wid == Bytes{gid, 8})) {
                uint64_t id = 0;
                for (int i = 0; i < 8; i++) id = id << 8 | wid.p[i];
                say("DAVE: welcomed into group %llu (not %llu, the one guessed)", (unsigned long long)id, (unsigned long long)channel_);
                channel_ = id;
                be64(channel_, gid);
            }
            if (res == Mls_Ok && (!(group_.groupId() == Bytes{gid, 8}) || !(group_.externalSender() == key))) {
                say("DAVE: a Welcome to another group, or from another gateway");
                res = Mls_BadSignature;
            }
        }
        if (res == Mls_NotForUs) return;
        // (our own group was only ours so far, and this commit is another group's: we'll be welcomed into it)
        if (op == DaveOp_AnnounceCommit && res == Mls_WrongGroup && group_.epoch() == 0) return;
        if (res != Mls_Ok) {
            say("DAVE: %s refused (%d, mlsgroup.cpp:%d)", op == DaveOp_Welcome ? "Welcome" : "commit", res, group_.whyLine());
            invalid(transition);
            return;
        }
        // the new epoch's keys, made now, used once the transition is executed
        next_.clear();
        next_.used = true;
        memcpy(next_.exporter, group_.exporterSecret(), MLS_NH);
        unsigned members = 0;
        for (uint32_t i = 0; i < group_.tree().leaves(); i++) members += group_.tree().type(2 * i) == Node_Leaf;
        say("DAVE: epoch %llu, %u in the group", (unsigned long long)group_.epoch(), members);
        transitionReady(transition, now_ms);
        return;
    }
    say("DAVE: binary opcode %u ignored", op);
}

bool DaveSession::decrypt(uint64_t user, const uint8_t* frame, size_t n, uint8_t* out, size_t* out_n, uint64_t now_ms) {
    bool marked = n >= 3 && frame[n - 1] == 0xFA && frame[n - 2] == 0xFA;
    if (daveIsSilence(frame, n) || version_ == 0 || (!marked && now_ms < passthrough_until_)) {
        if (daveIsSilence(frame, n)) silence_++;
        else plain_++;
        memcpy(out, frame, n);
        *out_n = n;
        return true;
    }
    Epoch* epochs[2] = {&current_, &previous_};
    for (Epoch* e : epochs) {
        if (!e->used || (e->expires_ms && now_ms >= e->expires_ms)) continue;
        DaveRatchet* r = e->forUser(user);
        if (r && daveDecrypt(*r, frame, n, out, out_n)) {
            opened_++;
            return true;
        }
    }
    failures_++;
    return false;
}

bool DaveSession::encrypt(const uint8_t* frame, size_t n, uint8_t* out, size_t cap, size_t* out_n) {
    if (daveIsSilence(frame, n) || version_ == 0) {
        if (cap < n) return false;
        memcpy(out, frame, n);
        *out_n = n;
        return true;
    }
    if (!current_.used) return false;  // (no key yet: nothing anyone could open)
    DaveRatchet* r = current_.forUser(me_);
    return r && daveEncrypt(*r, &counter_, frame, n, out, cap, out_n);
}

bool DaveSession::encryptVideo(uint8_t* frame, size_t n, size_t cap, size_t* out_n) {
    if (version_ == 0) {  // (a call without DAVE: as it is)
        *out_n = n;
        return true;
    }
    if (!current_.used) return false;
    DaveRatchet* r = current_.forUser(me_);
    return r && daveEncryptH264(*r, &counter_, frame, n, cap, out_n);
}
