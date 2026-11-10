#include "mlsgroup.h"

#include <string.h>

#include "mlstree.h"

namespace {

const uint16_t MLS10 = 1;
enum : uint8_t { Sender_Member = 1, Sender_External = 2, Sender_NewMemberProposal = 3, Sender_NewMemberCommit = 4 };
enum : uint8_t { Content_Application = 1, Content_Proposal = 2, Content_Commit = 3 };
const uint16_t EXT_RATCHET_TREE = 2, EXT_EXTERNAL_SENDERS = 5;

RatchetTree g_prov;              // a commit's tree while it's checked (64 KB: not on a stack)
uint8_t g_scratch[16 * 1024];    // TBS structs being signed or MAC'd
uint8_t g_gc[MLS_EXTENSIONS_MAX + 256];
uint8_t g_gi[RATCHET_ARENA + 2048 + GCM_TAG];  // (the tree, and the GroupContext and signature around it)  // a GroupInfo: opened from a Welcome, or written (and sealed in place) for one

// A KeyPackage, read
struct KeyPackageView {
    Bytes init_key, extensions, signature, tbs, all;
    LeafView leaf;
};

bool readKeyPackage(Reader& r, KeyPackageView* kp) {
    const uint8_t* start = r.p;
    if (r.u16() != MLS10 || r.u16() != MLS_SUITE) return false;
    kp->init_key = r.vec();
    if (!readLeafNode(r, &kp->leaf)) return false;
    kp->extensions = r.vec();
    kp->tbs = Bytes{start, (size_t)(r.p - start)};
    kp->signature = r.vec();
    kp->all = Bytes{start, (size_t)(r.p - start)};
    return r.ok;
}

bool verifyKeyPackage(const KeyPackageView& kp) {
    return kp.leaf.source == Leaf_KeyPackage && kp.leaf.credential_type == CREDENTIAL_BASIC &&
           mlsVerify(kp.leaf.signature_key.p, kp.leaf.signature_key.n, "KeyPackageTBS", kp.tbs.p, kp.tbs.n, kp.signature.p, kp.signature.n) &&
           verifyLeafSignature(kp.leaf, Bytes{}, 0);
}

// One Proposal (type and body), skipped over: false if it isn't one
bool readProposal(Reader& r, uint16_t* type, Bytes* body) {
    const uint8_t* start = r.p;
    *type = r.u16();
    const uint8_t* b = r.p;
    switch (*type) {
        case Prop_Add: {
            KeyPackageView kp;
            if (!readKeyPackage(r, &kp)) return false;
            break;
        }
        case Prop_Update: {
            LeafView l;
            if (!readLeafNode(r, &l)) return false;
            break;
        }
        case Prop_Remove:
            r.u32();
            break;
        case Prop_Psk: {
            uint8_t t = r.u8();
            if (t == 1) {
                r.vec();
            } else if (t == 2) {
                r.u8();
                r.vec();
                r.u64();
            } else {
                return false;
            }
            r.vec();
            break;
        }
        case Prop_ReInit:
            r.vec();
            r.u16();
            r.u16();
            r.vec();
            break;
        case Prop_ExternalInit:
            r.vec();
            break;
        case Prop_Gce:
            r.vec();
            break;
        default:
            return false;
    }
    *body = Bytes{b, (size_t)(r.p - b)};
    (void)start;
    return r.ok;
}

bool inSubtree(uint32_t root, uint32_t node) {
    uint32_t span = (1u << mlstree::level(root)) - 1;
    return node + span >= root && node <= root + span;
}

// A ParentNode's bytes: encryption_key<V>, parent_hash<V>, unmerged_leaves<V>
bool writeParentNode(Writer& w, const Bytes& key, const uint8_t* ph, size_t ph_n, const uint32_t* unmerged, size_t un) {
    w.vec(key);
    w.vec(ph, ph_n);
    size_t at = w.begin();
    for (size_t i = 0; i < un; i++) w.u32(unmerged[i]);
    w.end(at);
    return w.ok;
}

}  // namespace

// --- ours ---

MlsResult MlsGroup::fail(MlsResult r, int line) {
    why_line_ = line;
    return r;
}


void MlsGroup::reset() {
    joined_ = false;
    epoch_ = 0;
    group_id_n_ = extensions_n_ = 0;
    tree_.clear();
    for (PathKey& k : path_keys_) k.used = false;
    keys_.wipeAll();
    clearProposals();
    wipe(psks_, sizeof(psks_));
    psks_n_ = 0;
}

bool MlsGroup::setKeyPackage(const Bytes& key_package, const uint8_t init_priv[P256_PRIV], const uint8_t leaf_priv[P256_PRIV],
                             const uint8_t sig_priv[P256_PRIV]) {
    Reader r(key_package);
    KeyPackageView kp;
    if (!readKeyPackage(r, &kp) || !r.done() || kp.leaf.all.n > sizeof(kp_leaf_)) return false;
    mlsRefHash("MLS 1.0 KeyPackage Reference", kp.all.p, kp.all.n, kp_ref_);
    memcpy(kp_leaf_, kp.leaf.all.p, kp.leaf.all.n);
    kp_leaf_n_ = kp.leaf.all.n;
    memcpy(init_priv_, init_priv, P256_PRIV);
    memcpy(leaf_priv_, leaf_priv, P256_PRIV);
    memcpy(sig_priv_, sig_priv, P256_PRIV);
    have_kp_ = true;
    return true;
}

bool MlsGroup::setPathKey(uint32_t node, const uint8_t priv[P256_PRIV]) {
    PathKey* slot = nullptr;
    for (PathKey& k : path_keys_)
        if (k.used && k.node == node) slot = &k;
    for (PathKey& k : path_keys_)
        if (!slot && !k.used) slot = &k;
    if (!slot) return false;
    slot->used = true;
    slot->node = node;
    memcpy(slot->priv, priv, P256_PRIV);
    return p256Public(priv, slot->pub);
}

const MlsGroup::PathKey* MlsGroup::pathKey(uint32_t node) const {
    for (const PathKey& k : path_keys_)
        if (k.used && k.node == node) return &k;
    return nullptr;
}

// Keys for nodes that are blank now, or hold another key, are of no use: wiped
void MlsGroup::dropStaleKeys() {
    for (PathKey& k : path_keys_) {
        if (!k.used) continue;
        ParentView p;
        if (!tree_.parent(k.node, &p) || p.encryption_key.n != P256_PUB || memcmp(p.encryption_key.p, k.pub, P256_PUB) != 0) {
            wipe(&k, sizeof(k));
        }
    }
}

bool MlsGroup::keyFor(uint32_t node, uint8_t priv[P256_PRIV]) const {
    if (node == mlstree::leafNode(my_leaf_)) {
        memcpy(priv, leaf_priv_, P256_PRIV);
        return true;
    }
    const PathKey* k = pathKey(node);
    if (!k) return false;
    memcpy(priv, k->priv, P256_PRIV);
    return true;
}

bool MlsGroup::writeGroupContext(Writer& w, uint64_t epoch, const uint8_t tree_hash[MLS_NH], const uint8_t* confirmed, size_t confirmed_n,
                                 const Bytes& extensions) const {
    w.u16(MLS10);
    w.u16(MLS_SUITE);
    w.vec(group_id_, group_id_n_);
    w.u64(epoch);
    w.vec(tree_hash, MLS_NH);
    w.vec(confirmed, confirmed_n);
    w.vec(extensions);
    return w.ok;
}

Bytes MlsGroup::externalSenderKey(uint32_t index) const {
    Reader exts(extensions_, extensions_n_);
    while (exts.n && exts.ok) {
        uint16_t type = exts.u16();
        Bytes data = exts.vec();
        if (type != EXT_EXTERNAL_SENDERS) continue;
        Reader d(data);
        Reader list = d.sub();
        for (uint32_t i = 0; list.n && list.ok; i++) {
            Bytes key = list.vec();
            uint16_t ct = list.u16();
            if (ct == CREDENTIAL_BASIC) list.vec();
            else return Bytes{};
            if (i == index) return list.ok ? key : Bytes{};
        }
    }
    return Bytes{};
}

bool MlsGroup::addExternalPsk(const Bytes& id, const Bytes& psk) {
    if (psks_n_ == 4 || id.n > sizeof(psks_[0].id) || psk.n > sizeof(psks_[0].psk)) return false;
    Psk& p = psks_[psks_n_++];
    memcpy(p.id, id.p, id.n);
    memcpy(p.psk, psk.p, psk.n);
    p.id_n = id.n;
    p.psk_n = psk.n;
    return true;
}

namespace {

// A PreSharedKeyID {psktype, external: psk_id<V> | resumption: usage, group_id<V>, epoch; psk_nonce<V>}
bool readPskId(Reader& r, uint8_t* type, Bytes* psk_id) {
    *type = r.u8();
    if (*type == 1) {
        *psk_id = r.vec();
    } else if (*type == 2) {
        r.u8();
        r.vec();
        r.u64();
    } else {
        return false;
    }
    r.vec();
    return r.ok;
}

}  // namespace

bool MlsGroup::pskSecret(const Bytes* ids, size_t n, uint8_t out[MLS_NH]) const {
    static const uint8_t zeros[MLS_NH] = {0};
    memset(out, 0, MLS_NH);
    for (size_t i = 0; i < n; i++) {
        Reader r(ids[i]);
        uint8_t type;
        Bytes id;
        if (!readPskId(r, &type, &id) || type != 1) {  // (only external ones: a resumption PSK lands here)
            why_line_ = __LINE__;
            return false;
        }
        const Psk* found = nullptr;
        for (size_t k = 0; k < psks_n_; k++)
            if (psks_[k].id_n == id.n && memcmp(psks_[k].id, id.p, id.n) == 0) found = &psks_[k];
        if (!found) {
            why_line_ = __LINE__;
            return false;
        }
        // psk_input = ExpandWithLabel(Extract(0, psk), "derived psk", PSKLabel {id, index, count}, Nh)
        uint8_t extracted[MLS_NH], input[MLS_NH], label[256];
        mlsExtract(zeros, MLS_NH, found->psk, found->psk_n, extracted);
        Writer w(label, sizeof(label));
        w.raw(ids[i].p, ids[i].n);
        w.u16((uint16_t)i);
        w.u16((uint16_t)n);
        if (!w.ok || !mlsExpand(extracted, "derived psk", label, w.n, input, MLS_NH)) return false;
        // psk_secret_[i] = Extract(psk_input_[i-1], psk_secret_[i-1])
        mlsExtract(input, MLS_NH, out, MLS_NH, out);
        wipe(extracted, sizeof(extracted));
        wipe(input, sizeof(input));
    }
    return true;
}

void MlsGroup::revokeProposal(const Bytes& ref) {
    for (size_t i = 0; i < proposals_n_; i++) {
        if (ref.n != MLS_NH || memcmp(proposals_[i].ref, ref.p, MLS_NH) != 0) continue;
        // (its bytes stay in the pool until the next commit clears it: only the entry goes)
        proposals_[i] = proposals_[--proposals_n_];
        return;
    }
}

void MlsGroup::clearProposals() {
    proposals_n_ = 0;
    pool_n_ = 0;
}

// --- joining ---

MlsResult MlsGroup::join(const Bytes& welcome, const Bytes& ratchet_tree) {
    if (!have_kp_) return fail(Mls_NotForUs, __LINE__);
    Reader r(welcome);
    if (r.u16() != MLS_SUITE) return fail(Mls_Unsupported, __LINE__);
    Reader secrets = r.sub();
    Bytes encrypted_group_info = r.vec();
    if (!r.done()) return fail(Mls_Malformed, __LINE__);

    // 1. our EncryptedGroupSecrets, opened with our init key
    Bytes kem, ct;
    bool found = false;
    while (secrets.n && secrets.ok && !found) {
        Bytes ref = secrets.vec();
        Bytes k = secrets.vec(), c = secrets.vec();
        if (ref.n == MLS_NH && memcmp(ref.p, kp_ref_, MLS_NH) == 0) kem = k, ct = c, found = true;
    }
    if (!secrets.ok) return fail(Mls_Malformed, __LINE__);
    if (!found) return fail(Mls_NotForUs, __LINE__);
    if (ct.n < GCM_TAG || ct.n - GCM_TAG > sizeof(g_scratch)) return fail(Mls_Malformed, __LINE__);
    uint8_t* gs = g_scratch;
    if (!mlsDecrypt(init_priv_, "Welcome", encrypted_group_info.p, encrypted_group_info.n, kem.p, kem.n, ct.p, ct.n, gs)) return fail(Mls_BadSignature, __LINE__);
    Reader g(gs, ct.n - GCM_TAG);
    Bytes joiner = g.vec();
    uint8_t has_path = g.u8();
    Bytes path_secret;
    if (has_path == 1) path_secret = g.vec();
    Bytes psks = g.vec();
    if (!g.done() || joiner.n != MLS_NH || has_path > 1 || (has_path && path_secret.n != MLS_NH)) return fail(Mls_Malformed, __LINE__);
    // pre-shared keys (DAVE uses none; only external ones are known here)
    uint8_t psk_secret[MLS_NH] = {0};
    if (psks.n) {
        Bytes ids[4];
        size_t ids_n = 0;
        Reader pr(psks);
        while (pr.n && pr.ok) {
            if (ids_n == 4) return fail(Mls_TooBig, __LINE__);
            const uint8_t* start = pr.p;
            uint8_t type;
            Bytes id;
            if (!readPskId(pr, &type, &id)) return fail(Mls_Malformed, __LINE__);
            ids[ids_n++] = Bytes{start, (size_t)(pr.p - start)};
        }
        if (!pskSecret(ids, ids_n, psk_secret)) return fail(Mls_Unsupported, __LINE__);
    }
    uint8_t joiner_secret[MLS_NH], path[MLS_NH];
    memcpy(joiner_secret, joiner.p, MLS_NH);
    if (has_path) memcpy(path, path_secret.p, MLS_NH);
    wipe(gs, ct.n);

    // 2. the GroupInfo, opened with the welcome key
    uint8_t welcome_secret[MLS_NH], key[MLS_NK], nonce[MLS_NN];
    mlsWelcomeSecret(joiner_secret, psk_secret, welcome_secret);
    mlsExpand(welcome_secret, "key", nullptr, 0, key, MLS_NK);
    mlsExpand(welcome_secret, "nonce", nullptr, 0, nonce, MLS_NN);
    uint8_t* gi = g_gi;
    if (encrypted_group_info.n < GCM_TAG || encrypted_group_info.n - GCM_TAG > sizeof(g_gi)) return fail(Mls_TooBig, __LINE__);
    size_t gi_n = encrypted_group_info.n - GCM_TAG;
    Gcm aead;
    aead.init(key, MLS_NK);
    bool opened = aead.open(nonce, nullptr, 0, encrypted_group_info.p, gi_n, encrypted_group_info.p + gi_n, GCM_TAG, gi);
    aead.wipeKey();
    wipe(key, sizeof(key));
    wipe(welcome_secret, sizeof(welcome_secret));
    if (!opened) return fail(Mls_BadSignature, __LINE__);

    // GroupInfo {GroupContext, extensions<V>, confirmation_tag<V>, signer, signature<V>}
    Reader i(gi, gi_n);
    const uint8_t* gc_start = i.p;
    if (i.u16() != MLS10 || i.u16() != MLS_SUITE) return fail(Mls_Unsupported, __LINE__);
    Bytes group_id = i.vec();
    uint64_t epoch = i.u64();
    Bytes tree_hash = i.vec(), confirmed = i.vec(), gc_ext = i.vec();
    Bytes gc_bytes{gc_start, (size_t)(i.p - gc_start)};
    Bytes gi_ext = i.vec(), tag = i.vec();
    uint32_t signer = i.u32();
    Bytes tbs{gi, (size_t)(i.p - gi)};
    Bytes signature = i.vec();
    if (!i.done() || group_id.n > MLS_GROUP_ID_MAX || gc_ext.n > MLS_EXTENSIONS_MAX || tree_hash.n != MLS_NH || confirmed.n != MLS_NH)
        return fail(Mls_Malformed, __LINE__);

    // 3. the tree: the GroupInfo's ratchet_tree extension, or the one given
    Bytes tree_bytes = ratchet_tree;
    Reader exts(gi_ext);
    while (exts.n && exts.ok) {
        uint16_t type = exts.u16();
        Bytes data = exts.vec();
        if (type == EXT_RATCHET_TREE) tree_bytes = data;
    }
    Reader tr(tree_bytes);
    Bytes contents = tr.vec();
    if (!exts.ok || !tr.done() || !tree_.decode(contents)) return fail(Mls_BadTree, __LINE__);
    LeafView signer_leaf;
    if (!tree_.leaf(signer, &signer_leaf) ||
        !mlsVerify(signer_leaf.signature_key.p, signer_leaf.signature_key.n, "GroupInfoTBS", tbs.p, tbs.n, signature.p, signature.n))
        return fail(Mls_BadSignature, __LINE__);
    uint8_t root[MLS_NH];
    if (!tree_.rootHash(root) || memcmp(root, tree_hash.p, MLS_NH) != 0 || !tree_.verifyParentHashes()) return fail(Mls_BadTree, __LINE__);
    bool mine = false;
    for (uint32_t li = 0; li < tree_.leaves(); li++) {
        LeafView l;
        if (!tree_.leaf(li, &l)) continue;
        if (l.credential_type != CREDENTIAL_BASIC || !verifyLeafSignature(l, group_id, li)) return fail(Mls_BadTree, __LINE__);
        if (l.all.n == kp_leaf_n_ && memcmp(l.all.p, kp_leaf_, kp_leaf_n_) == 0) my_leaf_ = li, mine = true;
    }
    if (!mine) return fail(Mls_NotForUs, __LINE__);

    // the group as the GroupInfo says
    memcpy(group_id_, group_id.p, group_id.n);
    group_id_n_ = group_id.n;
    epoch_ = epoch;
    memcpy(extensions_, gc_ext.p, gc_ext.n);
    extensions_n_ = gc_ext.n;
    memcpy(tree_hash_, tree_hash.p, MLS_NH);
    memcpy(confirmed_, confirmed.p, MLS_NH);
    confirmed_n_ = MLS_NH;
    for (PathKey& k : path_keys_) k.used = false;

    // 4. the path secret, for the lowest common ancestor of us and the signer, and on up
    if (has_path) {
        uint32_t me = mlstree::leafNode(my_leaf_), them = mlstree::leafNode(signer), node = me;
        uint32_t rootNode = mlstree::root(tree_.leaves());
        while (!inSubtree(node, them)) node = mlstree::parent(node);
        bool first = true;
        for (;;) {
            if (tree_.type(node) == Node_Parent) {
                if (!first) mlsDeriveSecret(path, "path", path);
                first = false;
                uint8_t node_secret[MLS_NH], priv[P256_PRIV], pub[P256_PUB];
                ParentView pv;
                mlsDeriveSecret(path, "node", node_secret);
                bool ok = hpkeDeriveKeyPair(node_secret, MLS_NH, priv, pub) && tree_.parent(node, &pv) && pv.encryption_key.n == P256_PUB &&
                          memcmp(pv.encryption_key.p, pub, P256_PUB) == 0 && setPathKey(node, priv);
                wipe(node_secret, sizeof(node_secret));
                wipe(priv, sizeof(priv));
                if (!ok) return fail(Mls_BadTree, __LINE__);
            }
            if (node == rootNode) break;
            node = mlstree::parent(node);
        }
        wipe(path, sizeof(path));
    }

    // 5. the key schedule, and the confirmation tag that proves we got it right
    bool scheduled = mlsKeyScheduleFromJoiner(joiner_secret, psk_secret, gc_bytes, &keys_);
    wipe(psk_secret, sizeof(psk_secret));
    if (!scheduled) return fail(Mls_Malformed, __LINE__);
    wipe(joiner_secret, sizeof(joiner_secret));
    uint8_t want[MLS_NH];
    hmacSha256(keys_.confirmation_key, MLS_NH, confirmed_, confirmed_n_, want);
    if (tag.n != MLS_NH || memcmp(want, tag.p, MLS_NH) != 0) {
        reset();
        return fail(Mls_BadSignature, __LINE__);
    }
    // interim = Hash(confirmed || InterimTranscriptHashInput{confirmation_tag<V>})
    Sha256 h;
    h.init();
    h.update(confirmed_, confirmed_n_);
    uint8_t len = MLS_NH;
    h.update(&len, 1);
    h.update(tag.p, MLS_NH);
    h.final(interim_);
    clearProposals();
    joined_ = true;
    return Mls_Ok;
}

// --- handshake messages ---

MlsResult MlsGroup::handle(const Bytes& msg, size_t* used) {
    if (!joined_) return fail(Mls_WrongGroup, __LINE__);
    Reader r(msg);
    if (r.u16() != MLS10) return fail(Mls_Malformed, __LINE__);
    uint16_t wire = r.u16();
    if (wire != Wire_Public) return fail(Mls_Unsupported, __LINE__);  // (DAVE's handshakes are all public)

    // FramedContent
    const uint8_t* fc = r.p;
    Bytes group_id = r.vec();
    uint64_t epoch = r.u64();
    uint8_t sender_type = r.u8();
    uint32_t sender = 0;
    if (sender_type == Sender_Member || sender_type == Sender_External) sender = r.u32();
    else return fail(Mls_Unsupported, __LINE__);
    r.vec();  // authenticated_data
    uint8_t content_type = r.u8();
    const uint8_t* body = r.p;
    uint16_t ptype = 0;
    Bytes pbody;
    if (content_type == Content_Proposal) {
        if (!readProposal(r, &ptype, &pbody)) return fail(Mls_Malformed, __LINE__);
    } else if (content_type == Content_Commit) {
        r.sub();       // proposals<V>
        if (r.u8() == 1) {  // optional<UpdatePath>
            LeafView l;
            if (!readLeafNode(r, &l)) return fail(Mls_Malformed, __LINE__);
            r.vec();
        }
    } else {
        return fail(Mls_Unsupported, __LINE__);
    }
    Bytes content{fc, (size_t)(r.p - fc)};
    Bytes content_body{body, (size_t)(r.p - body)};
    // FramedContentAuthData, then the membership tag (members only)
    const uint8_t* auth = r.p;
    Bytes signature = r.vec();
    Bytes confirmation_tag;
    if (content_type == Content_Commit) confirmation_tag = r.vec();
    Bytes auth_data{auth, (size_t)(r.p - auth)};
    Bytes membership_tag;
    if (sender_type == Sender_Member) membership_tag = r.vec();
    if (!r.ok || (!used && r.n)) return fail(Mls_Malformed, __LINE__);
    if (used) *used = msg.n - r.n;
    if (group_id.n != group_id_n_ || memcmp(group_id.p, group_id_, group_id_n_) != 0 || epoch != epoch_) return fail(Mls_WrongGroup, __LINE__);

    // FramedContentTBS {version, wire_format, content, the group context (members)}
    Writer tbs(g_scratch, sizeof(g_scratch));
    tbs.u16(MLS10);
    tbs.u16(Wire_Public);
    tbs.raw(content.p, content.n);
    if (sender_type == Sender_Member) writeGroupContext(tbs, epoch_, tree_hash_, confirmed_, confirmed_n_, Bytes{extensions_, extensions_n_});
    if (!tbs.ok) return fail(Mls_TooBig, __LINE__);
    size_t tbs_n = tbs.n;

    Bytes sig_key;
    LeafView leaf;
    if (sender_type == Sender_Member) {
        if (!tree_.leaf(sender, &leaf)) return fail(Mls_BadSignature, __LINE__);
        sig_key = leaf.signature_key;
        // membership_tag = MAC(membership_key, AuthenticatedContentTBM {FramedContentTBS, FramedContentAuthData})
        HmacSha256 m;
        uint8_t want[MLS_NH];
        m.init(keys_.membership_key, MLS_NH);
        m.update(g_scratch, tbs_n);
        m.update(auth_data.p, auth_data.n);
        m.final(want);
        if (membership_tag.n != MLS_NH || memcmp(want, membership_tag.p, MLS_NH) != 0) return fail(Mls_BadSignature, __LINE__);
    } else {
        sig_key = externalSenderKey(sender);
        if (!sig_key.n || content_type != Content_Proposal) return fail(Mls_BadSignature, __LINE__);
        if (ptype != Prop_Add && ptype != Prop_Remove) return fail(Mls_Unsupported, __LINE__);  // (all DAVE's gateway may propose)
    }
    if (!mlsVerify(sig_key.p, sig_key.n, "FramedContentTBS", g_scratch, tbs_n, signature.p, signature.n)) return fail(Mls_BadSignature, __LINE__);

    if (content_type == Content_Proposal) {
        // ProposalRef = RefHash("MLS 1.0 Proposal Reference", AuthenticatedContent {wire_format, content, auth})
        if (proposals_n_ == MLS_PROPOSALS || pool_n_ + content_body.n > MLS_PROPOSAL_POOL) return fail(Mls_TooBig, __LINE__);
        Writer ac(g_scratch, sizeof(g_scratch));
        ac.u16(Wire_Public);
        ac.raw(content.p, content.n);
        ac.raw(auth_data.p, auth_data.n);
        if (!ac.ok) return fail(Mls_TooBig, __LINE__);
        Proposal& p = proposals_[proposals_n_++];
        mlsRefHash("MLS 1.0 Proposal Reference", g_scratch, ac.n, p.ref);
        p.type = ptype;
        p.sender_type = sender_type;
        p.sender = sender;
        p.off = (uint32_t)pool_n_;
        p.len = (uint32_t)content_body.n;
        memcpy(pool_ + pool_n_, content_body.p, content_body.n);
        pool_n_ += content_body.n;
        return Mls_Ok;
    }
    if (sender_type != Sender_Member) return fail(Mls_Unsupported, __LINE__);
    return handleCommit(content, content_body, sender, signature, confirmation_tag);
}

MlsResult MlsGroup::applyProposal(RatchetTree& t, const Bytes& proposal, uint32_t sender, uint8_t* new_leaves, Bytes* gce) {
    Reader r(proposal);
    uint16_t type = r.u16();
    if (type == Prop_Remove) {
        uint32_t removed = r.u32();
        if (!r.done() || removed >= t.leaves() || t.type(mlstree::leafNode(removed)) != Node_Leaf) return fail(Mls_Malformed, __LINE__);
        uint32_t node = mlstree::leafNode(removed), root = mlstree::root(t.leaves());
        t.blank(node);
        while (node != root) {
            node = mlstree::parent(node);
            t.blank(node);
        }
        t.truncate();
        return Mls_Ok;
    }
    if (type == Prop_Update) {
        LeafView l;
        if (!readLeafNode(r, &l) || !r.done() || l.source != Leaf_Update) return fail(Mls_Malformed, __LINE__);
        if (!verifyLeafSignature(l, Bytes{group_id_, group_id_n_}, sender)) return fail(Mls_BadSignature, __LINE__);
        uint32_t node = mlstree::leafNode(sender), root = mlstree::root(t.leaves());
        if (!t.set(node, Node_Leaf, l.all)) return fail(Mls_TooBig, __LINE__);
        while (node != root) {
            node = mlstree::parent(node);
            t.blank(node);
        }
        return Mls_Ok;
    }
    if (type == Prop_Add) {
        KeyPackageView kp;
        if (!readKeyPackage(r, &kp) || !r.done()) return fail(Mls_Malformed, __LINE__);
        if (!verifyKeyPackage(kp)) return fail(Mls_BadSignature, __LINE__);
        // the leftmost blank leaf, the tree doubled if there's none
        uint32_t li = 0;
        while (li < t.leaves() && t.type(mlstree::leafNode(li)) != Node_Blank) li++;
        if (!t.grow(li) || li >= RATCHET_LEAVES) return fail(Mls_TooBig, __LINE__);
        uint32_t node = mlstree::leafNode(li), root = mlstree::root(t.leaves());
        if (!t.set(node, Node_Leaf, kp.leaf.all)) return fail(Mls_TooBig, __LINE__);
        new_leaves[li / 8] |= (uint8_t)(1 << (li % 8));
        // it's unmerged at every non-blank parent above it
        while (node != root) {
            node = mlstree::parent(node);
            ParentView p;
            if (!t.parent(node, &p)) continue;
            uint32_t un[RATCHET_LEAVES];
            size_t n = 0;
            for (size_t k = 0; k < p.unmergedCount() && n < RATCHET_LEAVES - 1; k++) un[n++] = p.unmergedAt(k);
            un[n++] = li;
            for (size_t a = n - 1; a > 0 && un[a] < un[a - 1]; a--) {  // (kept in order)
                uint32_t x = un[a];
                un[a] = un[a - 1], un[a - 1] = x;
            }
            static uint8_t buf[RATCHET_LEAVES * 4 + 256];
            Writer w(buf, sizeof(buf));
            if (!writeParentNode(w, p.encryption_key, p.parent_hash.p, p.parent_hash.n, un, n) || !t.set(node, Node_Parent, w.bytes()))
                return fail(Mls_TooBig, __LINE__);
        }
        return Mls_Ok;
    }
    if (type == Prop_Gce) {
        Bytes e = r.vec();
        if (!r.done()) return fail(Mls_Malformed, __LINE__);
        *gce = e;
        return Mls_Ok;
    }
    return fail(Mls_Unsupported, __LINE__);  // (PSKs, ReInit, ExternalInit: not in DAVE)
}

MlsResult MlsGroup::handleCommit(const Bytes& content, const Bytes& commit, uint32_t sender, const Bytes& signature,
                                 const Bytes& confirmation_tag) {
    Reader r(commit);
    Reader list = r.sub();
    uint8_t has_path = r.u8();
    LeafView path_leaf;
    Reader path_nodes(nullptr, 0);
    if (has_path == 1) {
        if (!readLeafNode(r, &path_leaf)) return fail(Mls_Malformed, __LINE__);
        path_nodes = r.sub();
    }
    if (!r.done() || has_path > 1) return fail(Mls_Malformed, __LINE__);

    // the proposals, by value or by reference, gathered by kind (applied in RFC 9420 12.3's order)
    struct Item {
        Bytes p;
        uint16_t type;
        uint32_t sender;
    };
    static Item items[MLS_PROPOSALS * 2];
    size_t n = 0;
    while (list.n && list.ok) {
        if (n == MLS_PROPOSALS * 2) return fail(Mls_TooBig, __LINE__);
        uint8_t kind = list.u8();
        if (kind == 1) {
            const uint8_t* start = list.p;
            uint16_t type;
            Bytes body;
            if (!readProposal(list, &type, &body)) return fail(Mls_Malformed, __LINE__);
            items[n++] = Item{Bytes{start, (size_t)(list.p - start)}, type, sender};
        } else if (kind == 2) {
            Bytes ref = list.vec();
            const Proposal* found = nullptr;
            for (size_t i = 0; i < proposals_n_; i++)
                if (ref.n == MLS_NH && memcmp(proposals_[i].ref, ref.p, MLS_NH) == 0) found = &proposals_[i];
            if (!found) return fail(Mls_UnknownProposal, __LINE__);
            items[n++] = Item{Bytes{pool_ + found->off, found->len}, found->type,
                              found->sender_type == Sender_Member ? found->sender : 0xffffffffu};
        } else {
            return fail(Mls_Malformed, __LINE__);
        }
    }
    if (!list.ok) return fail(Mls_Malformed, __LINE__);
    bool path_required = n == 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t t = items[i].type;
        if (t == Prop_Update || t == Prop_Remove || t == Prop_ExternalInit || t == Prop_Gce) path_required = true;
        if (t == Prop_Update && items[i].sender == sender) return fail(Mls_Malformed, __LINE__);  // (a committer can't update itself by proposal)
    }
    if (path_required && !has_path) return fail(Mls_Malformed, __LINE__);

    g_prov = tree_;
    // the keys a path brings us, kept aside until the commit is known good
    static PathKey fresh[MLS_PATH_KEYS + 1];
    for (PathKey& f : fresh) f.used = false;
    uint8_t new_leaves[RATCHET_LEAVES / 8] = {0};
    Bytes gce{extensions_, extensions_n_};
    static const uint16_t ORDER[] = {Prop_Gce, Prop_Update, Prop_Remove, Prop_Add, Prop_Psk, Prop_ReInit, Prop_ExternalInit};
    Bytes psk_ids[4];
    size_t psk_n = 0;
    for (uint16_t kind : ORDER) {
        for (size_t i = 0; i < n; i++) {
            if (items[i].type != kind) continue;
            if (kind == Prop_Psk) {  // (gathered for the key schedule, in the commit's order)
                if (psk_n == 4) return fail(Mls_TooBig, __LINE__);
                psk_ids[psk_n++] = Bytes{items[i].p.p + 2, items[i].p.n - 2};
                continue;
            }
            MlsResult res = applyProposal(g_prov, items[i].p, items[i].sender, new_leaves, &gce);
            if (res != Mls_Ok) return res;
        }
    }
    if (g_prov.type(mlstree::leafNode(my_leaf_)) != Node_Leaf) return fail(Mls_Removed, __LINE__);
    if (g_prov.type(mlstree::leafNode(sender)) != Node_Leaf) return fail(Mls_Malformed, __LINE__);

    uint8_t commit_secret[MLS_NH] = {0};
    uint8_t tree_hash[MLS_NH];
    if (has_path) {
        if (path_leaf.source != Leaf_Commit || !verifyLeafSignature(path_leaf, Bytes{group_id_, group_id_n_}, sender)) return fail(Mls_BadSignature, __LINE__);
        // the sender's filtered direct path, and each step's copath child
        uint32_t fpath[MLS_PATH_KEYS + 1], copath[MLS_PATH_KEYS + 1];
        size_t fn = 0;
        uint32_t node = mlstree::leafNode(sender), root = mlstree::root(g_prov.leaves());
        static uint32_t res[RATCHET_NODES];
        while (node != root) {
            uint32_t p = mlstree::parent(node), s = mlstree::sibling(node);
            if (g_prov.resolution(s, res, RATCHET_NODES) > 0) {
                if (fn == MLS_PATH_KEYS + 1) return fail(Mls_TooBig, __LINE__);
                fpath[fn] = p, copath[fn] = s, fn++;
            }
            node = p;
        }
        // the UpdatePathNodes, one per filtered node
        struct PathNode {
            Bytes key;
            Reader cts;
        };
        static PathNode pn[MLS_PATH_KEYS + 1];
        size_t got = 0;
        while (path_nodes.n && path_nodes.ok) {
            if (got == fn) return fail(Mls_Malformed, __LINE__);
            pn[got].key = path_nodes.vec();
            pn[got].cts = path_nodes.sub();
            if (pn[got].key.n != P256_PUB) return fail(Mls_Malformed, __LINE__);
            got++;
        }
        if (!path_nodes.ok || got != fn) return fail(Mls_Malformed, __LINE__);

        // merge it: the direct path blanked, the filtered nodes set (root down, each with the parent hash of the one above)
        node = mlstree::leafNode(sender);
        while (node != root) {
            node = mlstree::parent(node);
            g_prov.blank(node);
        }
        uint8_t above[MLS_NH];
        size_t above_n = 0;
        static uint8_t buf[512];
        for (size_t i = fn; i-- > 0;) {
            Writer w(buf, sizeof(buf));
            if (!writeParentNode(w, pn[i].key, above, above_n, nullptr, 0) || !g_prov.set(fpath[i], Node_Parent, w.bytes())) return fail(Mls_TooBig, __LINE__);
            if (!g_prov.parentHash(fpath[i], copath[i], above)) return fail(Mls_Malformed, __LINE__);
            above_n = MLS_NH;
        }
        // (the leaf's parent_hash: the first filtered node's, or empty when there's none)
        if (path_leaf.parent_hash.n != above_n || (above_n && memcmp(path_leaf.parent_hash.p, above, MLS_NH) != 0)) return fail(Mls_BadTree, __LINE__);
        if (!g_prov.set(mlstree::leafNode(sender), Node_Leaf, path_leaf.all)) return fail(Mls_TooBig, __LINE__);

        // the provisional group context: next epoch, this tree, the old confirmed transcript hash
        if (!g_prov.rootHash(tree_hash)) return fail(Mls_Malformed, __LINE__);
        Writer gc(g_gc, sizeof(g_gc));
        if (!writeGroupContext(gc, epoch_ + 1, tree_hash, confirmed_, confirmed_n_, gce)) return fail(Mls_TooBig, __LINE__);

        if (sender != my_leaf_) {
            // our path secret: from the step whose copath child holds us, opened with a key we have
            uint32_t me = mlstree::leafNode(my_leaf_);
            size_t step = fn;
            for (size_t i = 0; i < fn && step == fn; i++)
                if (inSubtree(copath[i], me)) step = i;
            if (step == fn) return fail(Mls_BadTree, __LINE__);
            int rn = g_prov.resolution(copath[step], res, RATCHET_NODES);
            int k = -1, idx = 0;
            uint8_t priv[P256_PRIV];
            for (int j = 0; j < rn; j++) {
                uint32_t li = res[j] / 2;
                if (res[j] % 2 == 0 && ((new_leaves[li / 8] >> (li % 8)) & 1)) continue;  // (new members get theirs in the Welcome)
                if (k < 0 && res[j] == me) {
                    keyFor(me, priv);
                    k = idx;
                } else if (k < 0) {
                    // (a parent's key of ours, if the node still holds that key)
                    const PathKey* pk = pathKey(res[j]);
                    ParentView pv;
                    if (pk && g_prov.parent(res[j], &pv) && pv.encryption_key.n == P256_PUB &&
                        memcmp(pv.encryption_key.p, pk->pub, P256_PUB) == 0) {
                        memcpy(priv, pk->priv, P256_PRIV);
                        k = idx;
                    }
                }
                idx++;
            }
            if (k < 0) return fail(Mls_BadTree, __LINE__);
            Bytes kem, ct;
            for (int j = 0; j <= k; j++) {
                kem = pn[step].cts.vec();
                ct = pn[step].cts.vec();
            }
            if (!pn[step].cts.ok || ct.n != MLS_NH + GCM_TAG) return fail(Mls_Malformed, __LINE__);
            uint8_t ps[MLS_NH];
            if (!mlsDecrypt(priv, "UpdatePathNode", g_gc, gc.n, kem.p, kem.n, ct.p, ct.n, ps)) return fail(Mls_BadSignature, __LINE__);
            wipe(priv, sizeof(priv));
            // then on up the filtered path: each node's key from its path secret, checked against what was sent
            for (size_t i = step; i < fn; i++) {
                if (i > step) mlsDeriveSecret(ps, "path", ps);
                uint8_t ns[MLS_NH], pub[P256_PUB];
                mlsDeriveSecret(ps, "node", ns);
                bool ok = hpkeDeriveKeyPair(ns, MLS_NH, fresh[i].priv, pub) && memcmp(pub, pn[i].key.p, P256_PUB) == 0;
                wipe(ns, sizeof(ns));
                if (!ok) return fail(Mls_BadTree, __LINE__);
                fresh[i].node = fpath[i];
                fresh[i].used = true;
            }
            mlsDeriveSecret(ps, "path", commit_secret);
            wipe(ps, sizeof(ps));
        } else {
            // our own commit, back: its path from the secret we kept when we made it
            uint8_t ref[MLS_NH];
            sha256(content.p, content.n, ref);
            if (!pending_ || memcmp(ref, pending_ref_, MLS_NH) != 0) return fail(Mls_Malformed, __LINE__);
            uint8_t ps[MLS_NH];
            memcpy(ps, pending_path_secret_, MLS_NH);
            for (size_t i = 0; i < fn; i++) {
                if (i > 0) mlsDeriveSecret(ps, "path", ps);
                uint8_t ns[MLS_NH], pub[P256_PUB];
                mlsDeriveSecret(ps, "node", ns);
                bool ok = hpkeDeriveKeyPair(ns, MLS_NH, fresh[i].priv, pub) && memcmp(pub, pn[i].key.p, P256_PUB) == 0;
                wipe(ns, sizeof(ns));
                if (!ok) return fail(Mls_BadTree, __LINE__);
                fresh[i].node = fpath[i];
                fresh[i].used = true;
            }
            // (with no filtered path, the one secret we kept is the last: only we and those we welcome need it)
            mlsDeriveSecret(ps, "path", commit_secret);
            wipe(ps, sizeof(ps));
        }
    } else {
        if (!g_prov.rootHash(tree_hash)) return fail(Mls_Malformed, __LINE__);
    }

    // the transcript: confirmed = Hash(interim || {wire_format, content, signature<V>})
    uint8_t confirmed[MLS_NH];
    {
        Sha256 h;
        h.init();
        h.update(interim_, MLS_NH);
        uint8_t wf[2] = {0, Wire_Public};
        h.update(wf, 2);
        h.update(content.p, content.n);
        uint8_t lenb[4];
        Writer lw(lenb, sizeof(lenb));
        lw.varint((uint32_t)signature.n);
        h.update(lenb, lw.n);
        h.update(signature.p, signature.n);
        h.final(confirmed);
    }
    Writer gc(g_gc, sizeof(g_gc));
    if (!writeGroupContext(gc, epoch_ + 1, tree_hash, confirmed, MLS_NH, gce)) return fail(Mls_TooBig, __LINE__);
    EpochKeys next;
    uint8_t psk_secret[MLS_NH] = {0};
    if (psk_n && !pskSecret(psk_ids, psk_n, psk_secret)) return Mls_Unsupported;  // (why_line_ says which)
    bool scheduled = mlsKeySchedule(keys_.init, commit_secret, psk_secret, gc.bytes(), &next);
    wipe(psk_secret, sizeof(psk_secret));
    if (!scheduled) return fail(Mls_Malformed, __LINE__);
    wipe(commit_secret, sizeof(commit_secret));
    uint8_t want[MLS_NH];
    hmacSha256(next.confirmation_key, MLS_NH, confirmed, MLS_NH, want);
    if (confirmation_tag.n != MLS_NH || memcmp(want, confirmation_tag.p, MLS_NH) != 0) {
        next.wipeAll();
        return fail(Mls_BadSignature, __LINE__);
    }

    // it's good: the new epoch
    if (gce.p != extensions_) {
        if (gce.n > MLS_EXTENSIONS_MAX) return fail(Mls_TooBig, __LINE__);
        memmove(extensions_, gce.p, gce.n);
    }
    extensions_n_ = gce.n;
    tree_ = g_prov;
    epoch_++;
    memcpy(tree_hash_, tree_hash, MLS_NH);
    memcpy(confirmed_, confirmed, MLS_NH);
    confirmed_n_ = MLS_NH;
    {
        Sha256 h;
        h.init();
        h.update(confirmed_, confirmed_n_);
        uint8_t len = MLS_NH;
        h.update(&len, 1);
        h.update(confirmation_tag.p, MLS_NH);
        h.final(interim_);
    }
    keys_.wipeAll();
    keys_ = next;
    next.wipeAll();
    if (has_path && sender == my_leaf_) memcpy(leaf_priv_, pending_leaf_priv_, P256_PRIV);  // (our new leaf's key)
    // (whoever's commit it was, ours isn't waiting any more: it's in, or it lost to this one)
    pending_ = false;
    wipe(pending_leaf_priv_, sizeof(pending_leaf_priv_));
    wipe(pending_path_secret_, sizeof(pending_path_secret_));
    dropStaleKeys();  // (the old keys for the nodes the path just replaced)
    for (PathKey& f : fresh) {
        if (f.used) setPathKey(f.node, f.priv);
        wipe(&f, sizeof(f));
    }
    clearProposals();
    return Mls_Ok;
}

// --- making our own: key packages, a new group, commits and Welcomes ---

namespace {

// A key pair from 32 random bytes (HPKE's DeriveKeyPair, as MLS makes its keys)
bool freshKeyPair(MlsGroup::Random rnd, uint8_t priv[P256_PRIV], uint8_t pub[P256_PUB]) {
    uint8_t ikm[32];
    rnd(ikm, sizeof(ikm));
    bool ok = hpkeDeriveKeyPair(ikm, sizeof(ikm), priv, pub);
    wipe(ikm, sizeof(ikm));
    return ok;
}

// A ParentNode's or LeafNode's encryption key, for encrypting to it
bool nodeKey(const RatchetTree& t, uint32_t node, Bytes* key) {
    if (t.type(node) == Node_Leaf) {
        LeafView l;
        if (!t.leaf(node / 2, &l)) return false;
        *key = l.encryption_key;
    } else {
        ParentView p;
        if (!t.parent(node, &p)) return false;
        *key = p.encryption_key;
    }
    return key->n == P256_PUB;
}

}  // namespace

bool MlsGroup::makeKeyPackage(const uint8_t* identity, size_t identity_n, Random rnd, Writer& w) {
    uint8_t sig_priv[P256_PRIV], sig_pub[P256_PUB], init_priv[P256_PRIV], init_pub[P256_PUB], leaf_priv[P256_PRIV], leaf_pub[P256_PUB];
    do rnd(sig_priv, sizeof(sig_priv));
    while (!p256ValidPrivate(sig_priv));
    if (!p256Public(sig_priv, sig_pub) || !freshKeyPair(rnd, init_priv, init_pub) || !freshKeyPair(rnd, leaf_priv, leaf_pub)) return false;

    // the LeafNode: our keys, a basic credential, what we support, forever valid (as DAVE asks)
    static uint8_t leaf[1024];
    Writer l(leaf, sizeof(leaf));
    l.vec(leaf_pub, P256_PUB);
    l.vec(sig_pub, P256_PUB);
    l.u16(CREDENTIAL_BASIC);
    l.vec(identity, identity_n);
    const uint8_t versions[] = {0, 1}, suites[] = {0, MLS_SUITE}, credentials[] = {0, CREDENTIAL_BASIC};
    l.vec(versions, 2);
    l.vec(suites, 2);
    l.vec(nullptr, 0);  // extensions (the default ones needn't be listed)
    l.vec(nullptr, 0);  // proposals (likewise)
    l.vec(credentials, 2);
    l.u8(Leaf_KeyPackage);
    l.u64(0);
    l.u64(UINT64_MAX);
    l.vec(nullptr, 0);  // the leaf's extensions
    uint8_t sig[P256_SIG_MAX];
    size_t sig_n = 0;
    if (!l.ok || !mlsSign(sig_priv, "LeafNodeTBS", leaf, l.n, sig, &sig_n)) return false;
    l.vec(sig, sig_n);

    // the KeyPackage around it, signed, then wrapped
    static uint8_t kp[1536];
    Writer k(kp, sizeof(kp));
    k.u16(MLS10);
    k.u16(MLS_SUITE);
    k.vec(init_pub, P256_PUB);
    k.raw(leaf, l.n);
    k.vec(nullptr, 0);
    if (!k.ok || !mlsSign(sig_priv, "KeyPackageTBS", kp, k.n, sig, &sig_n)) return false;
    k.vec(sig, sig_n);
    w.u16(MLS10);
    w.u16(Wire_KeyPackage);
    w.raw(kp, k.n);
    bool ok = k.ok && w.ok && setKeyPackage(k.bytes(), init_priv, leaf_priv, sig_priv);
    wipe(sig_priv, sizeof(sig_priv));
    wipe(init_priv, sizeof(init_priv));
    wipe(leaf_priv, sizeof(leaf_priv));
    return ok;
}

bool MlsGroup::create(const Bytes& group_id, const Bytes& extensions, Random rnd) {
    if (!have_kp_ || group_id.n > MLS_GROUP_ID_MAX || extensions.n > MLS_EXTENSIONS_MAX) return false;
    uint8_t keep[sizeof(psks_)];
    size_t keep_n = psks_n_;
    memcpy(keep, psks_, sizeof(psks_));
    reset();
    memcpy(psks_, keep, sizeof(psks_));
    psks_n_ = keep_n;
    tree_.grow(0);
    if (!tree_.set(0, Node_Leaf, Bytes{kp_leaf_, kp_leaf_n_})) return false;
    my_leaf_ = 0;
    memcpy(group_id_, group_id.p, group_id.n);
    group_id_n_ = group_id.n;
    epoch_ = 0;
    memcpy(extensions_, extensions.p, extensions.n);
    extensions_n_ = extensions.n;
    confirmed_n_ = 0;  // (the transcript starts empty)
    tree_.rootHash(tree_hash_);
    // epoch 0's secrets from a random init_secret and no commit
    uint8_t init[MLS_NH], zero[MLS_NH] = {0};
    rnd(init, sizeof(init));
    Writer gc(g_gc, sizeof(g_gc));
    bool ok = writeGroupContext(gc, 0, tree_hash_, confirmed_, 0, Bytes{extensions_, extensions_n_}) &&
              mlsKeySchedule(init, zero, nullptr, gc.bytes(), &keys_);
    wipe(init, sizeof(init));
    if (!ok) return false;
    uint8_t tag[MLS_NH];
    hmacSha256(keys_.confirmation_key, MLS_NH, confirmed_, 0, tag);
    Sha256 h;
    h.init();
    uint8_t len = MLS_NH;
    h.update(&len, 1);
    h.update(tag, MLS_NH);
    h.final(interim_);
    joined_ = true;
    return true;
}

MlsResult MlsGroup::commit(Random rnd, Writer& out, Writer& welcome_out, bool* welcome) {
    *welcome = false;
    if (!joined_) return fail(Mls_WrongGroup, __LINE__);

    // every proposal we have, applied in RFC 9420 12.3's order (as handleCommit does)
    g_prov = tree_;
    uint8_t new_leaves[RATCHET_LEAVES / 8] = {0};
    Bytes gce{extensions_, extensions_n_};
    static const uint16_t ORDER[] = {Prop_Gce, Prop_Update, Prop_Remove, Prop_Add};
    for (size_t i = 0; i < proposals_n_; i++) {
        uint16_t t = proposals_[i].type;
        if (t != Prop_Gce && t != Prop_Update && t != Prop_Remove && t != Prop_Add) return fail(Mls_Unsupported, __LINE__);
    }
    for (uint16_t kind : ORDER) {
        for (size_t i = 0; i < proposals_n_; i++) {
            const Proposal& p = proposals_[i];
            if (p.type != kind) continue;
            MlsResult res = applyProposal(g_prov, Bytes{pool_ + p.off, p.len}, p.sender_type == Sender_Member ? p.sender : 0xffffffffu,
                                          new_leaves, &gce);
            if (res != Mls_Ok) return res;
        }
    }
    if (g_prov.type(mlstree::leafNode(my_leaf_)) != Node_Leaf) return fail(Mls_Removed, __LINE__);
    added_n_ = 0;
    for (uint32_t li = 0; li < g_prov.leaves() && added_n_ < 8; li++) {
        if (!((new_leaves[li / 8] >> (li % 8)) & 1)) continue;
        LeafView lv;
        uint64_t id = 0;
        if (g_prov.leaf(li, &lv) && lv.identity.n == 8)
            for (int b = 0; b < 8; b++) id = id << 8 | lv.identity.p[b];
        added_[added_n_++] = id;
    }

    // A path only when it's needed (RFC 9420 12.4: not for a commit of adds alone), as mlspp
    // commits for libdave (force_path false): the commits Discord's gateway is used to
    bool path = proposals_n_ == 0;
    for (size_t i = 0; i < proposals_n_; i++) path = path || proposals_[i].type != Prop_Add;

    // a fresh path: our new leaf key, and a secret for each node of our filtered direct path
    uint32_t fpath[MLS_PATH_KEYS + 1], copath[MLS_PATH_KEYS + 1];
    size_t fn = 0;
    uint32_t root = mlstree::root(g_prov.leaves()), node = mlstree::leafNode(my_leaf_);
    static uint32_t res[RATCHET_NODES];
    while (path && node != root) {
        uint32_t p = mlstree::parent(node), s = mlstree::sibling(node);
        if (g_prov.resolution(s, res, RATCHET_NODES) > 0) {
            if (fn == MLS_PATH_KEYS + 1) return fail(Mls_TooBig, __LINE__);
            fpath[fn] = p, copath[fn] = s, fn++;
        }
        node = p;
    }
    uint8_t leaf_priv[P256_PRIV], leaf_pub[P256_PUB];
    if (!freshKeyPair(rnd, leaf_priv, leaf_pub)) return fail(Mls_Malformed, __LINE__);
    static uint8_t secrets[MLS_PATH_KEYS + 1][MLS_NH], pubs[MLS_PATH_KEYS + 1][P256_PUB];
    uint8_t first[MLS_NH];
    rnd(first, sizeof(first));
    for (size_t i = 0; i < fn; i++) {
        if (i == 0) memcpy(secrets[0], first, MLS_NH);
        else mlsDeriveSecret(secrets[i - 1], "path", secrets[i]);
        uint8_t ns[MLS_NH], priv[P256_PRIV];
        mlsDeriveSecret(secrets[i], "node", ns);
        bool ok = hpkeDeriveKeyPair(ns, MLS_NH, priv, pubs[i]);
        wipe(ns, sizeof(ns));
        wipe(priv, sizeof(priv));  // (made again from the secret when the commit comes back)
        if (!ok) return fail(Mls_Malformed, __LINE__);
    }

    // merged into the provisional tree as a receiver would: root down, with parent hashes
    node = mlstree::leafNode(my_leaf_);
    while (path && node != root) {
        node = mlstree::parent(node);
        g_prov.blank(node);
    }
    uint8_t above[MLS_NH];
    size_t above_n = 0;
    static uint8_t buf[512];
    for (size_t i = fn; i-- > 0;) {
        Writer w(buf, sizeof(buf));
        if (!writeParentNode(w, Bytes{pubs[i], P256_PUB}, above, above_n, nullptr, 0) || !g_prov.set(fpath[i], Node_Parent, w.bytes()))
            return fail(Mls_TooBig, __LINE__);
        if (!g_prov.parentHash(fpath[i], copath[i], above)) return fail(Mls_Malformed, __LINE__);
        above_n = MLS_NH;
    }
    // our new leaf: as it was, with the new key, source commit and the parent hash, signed again
    static uint8_t leaf[2048];
    Writer l(leaf, sizeof(leaf));
    uint8_t sig[P256_SIG_MAX];
    size_t sig_n = 0;
    if (path) {
        LeafView mine;
        if (!tree_.leaf(my_leaf_, &mine)) return fail(Mls_Malformed, __LINE__);
        l.vec(leaf_pub, P256_PUB);
        l.vec(mine.signature_key);
        l.u16(mine.credential_type);
        l.vec(mine.identity);
        l.raw(mine.capabilities.p, mine.capabilities.n);
        l.u8(Leaf_Commit);
        l.vec(above, above_n);
        l.vec(mine.extensions);
        size_t fields = l.n;
        l.vec(group_id_, group_id_n_);
        l.u32(my_leaf_);
        if (!l.ok || !mlsSign(sig_priv_, "LeafNodeTBS", leaf, l.n, sig, &sig_n)) return fail(Mls_TooBig, __LINE__);
        l.n = fields;  // (group_id and leaf_index were only for the signature)
        l.vec(sig, sig_n);
        if (!l.ok || !g_prov.set(mlstree::leafNode(my_leaf_), Node_Leaf, l.bytes())) return fail(Mls_TooBig, __LINE__);
    }

    uint8_t tree_hash[MLS_NH];
    if (!g_prov.rootHash(tree_hash)) return fail(Mls_Malformed, __LINE__);
    Writer gc(g_gc, sizeof(g_gc));
    if (!writeGroupContext(gc, epoch_ + 1, tree_hash, confirmed_, confirmed_n_, gce)) return fail(Mls_TooBig, __LINE__);
    size_t prov_gc_n = gc.n;

    // the FramedContent: the Commit (every proposal by reference, and the path, encrypted to each copath resolution)
    // (written straight into the PublicMessage, after its version and wire format)
    out.u16(MLS10);
    out.u16(Wire_Public);
    if (!out.ok) return fail(Mls_TooBig, __LINE__);
    uint8_t* content = out.buf + out.n;
    Writer c(content, out.cap - out.n);
    c.vec(group_id_, group_id_n_);
    c.u64(epoch_);
    c.u8(Sender_Member);
    c.u32(my_leaf_);
    c.vec(nullptr, 0);
    c.u8(Content_Commit);
    size_t at = c.begin();
    for (size_t i = 0; i < proposals_n_; i++) {
        c.u8(2);
        c.vec(proposals_[i].ref, MLS_NH);
    }
    c.end(at);
    c.u8(path ? 1 : 0);
    size_t nodes_at = 0;
    if (path) {
        c.raw(leaf, l.n);
        nodes_at = c.begin();
    }
    for (size_t i = 0; i < fn; i++) {
        c.vec(pubs[i], P256_PUB);
        size_t cts = c.begin();
        int rn = g_prov.resolution(copath[i], res, RATCHET_NODES);
        for (int j = 0; j < rn; j++) {
            uint32_t li = res[j] / 2;
            if (res[j] % 2 == 0 && ((new_leaves[li / 8] >> (li % 8)) & 1)) continue;  // (new members: in the Welcome)
            Bytes key;
            if (!nodeKey(g_prov, res[j], &key)) return fail(Mls_BadTree, __LINE__);
            uint8_t ikm[32], kem[HPKE_ENC], ct[MLS_NH + GCM_TAG];
            rnd(ikm, sizeof(ikm));
            bool ok = mlsEncrypt(key.p, "UpdatePathNode", g_gc, prov_gc_n, secrets[i], MLS_NH, ikm, kem, ct);
            wipe(ikm, sizeof(ikm));
            if (!ok) return fail(Mls_Malformed, __LINE__);
            c.vec(kem, sizeof(kem));
            c.vec(ct, sizeof(ct));
        }
        c.end(cts);
    }
    if (path) c.end(nodes_at);
    if (!c.ok) return fail(Mls_TooBig, __LINE__);
    uint8_t commit_secret[MLS_NH] = {0};  // (no path: all zero, RFC 9420 8)
    if (path && fn) mlsDeriveSecret(secrets[fn - 1], "path", commit_secret);
    else if (path) mlsDeriveSecret(first, "path", commit_secret);

    // signed over FramedContentTBS (with this epoch's group context)
    Writer tbs(g_scratch, sizeof(g_scratch));
    tbs.u16(MLS10);
    tbs.u16(Wire_Public);
    tbs.raw(content, c.n);
    writeGroupContext(tbs, epoch_, tree_hash_, confirmed_, confirmed_n_, Bytes{extensions_, extensions_n_});
    if (!tbs.ok || !mlsSign(sig_priv_, "FramedContentTBS", g_scratch, tbs.n, sig, &sig_n)) return fail(Mls_TooBig, __LINE__);

    // the next epoch: its transcript hash, group context, secrets and confirmation tag
    uint8_t confirmed[MLS_NH];
    {
        Sha256 h;
        h.init();
        h.update(interim_, MLS_NH);
        uint8_t wf[2] = {0, Wire_Public};
        h.update(wf, 2);
        h.update(content, c.n);
        uint8_t lenb[4];
        Writer lw(lenb, sizeof(lenb));
        lw.varint((uint32_t)sig_n);
        h.update(lenb, lw.n);
        h.update(sig, sig_n);
        h.final(confirmed);
    }
    Writer ngc(g_gc, sizeof(g_gc));
    if (!writeGroupContext(ngc, epoch_ + 1, tree_hash, confirmed, MLS_NH, gce)) return fail(Mls_TooBig, __LINE__);
    static EpochKeys next;
    if (!mlsKeySchedule(keys_.init, commit_secret, nullptr, ngc.bytes(), &next)) return fail(Mls_Malformed, __LINE__);
    wipe(commit_secret, sizeof(commit_secret));
    uint8_t tag[MLS_NH];
    hmacSha256(next.confirmation_key, MLS_NH, confirmed, MLS_NH, tag);

    // the PublicMessage: content, auth {signature, confirmation_tag}, and the membership tag (this epoch's key)
    size_t auth_at = tbs.n;
    tbs.vec(sig, sig_n);
    tbs.vec(tag, MLS_NH);
    if (!tbs.ok) return fail(Mls_TooBig, __LINE__);
    uint8_t membership[MLS_NH];
    hmacSha256(keys_.membership_key, MLS_NH, g_scratch, tbs.n, membership);
    out.n += c.n;
    out.raw(g_scratch + auth_at, tbs.n - auth_at);
    out.vec(membership, MLS_NH);
    if (!out.ok) return fail(Mls_TooBig, __LINE__);

    // kept until the commit comes back
    sha256(content, c.n, pending_ref_);
    memcpy(pending_leaf_priv_, leaf_priv, P256_PRIV);
    memcpy(pending_path_secret_, first, MLS_NH);
    pending_ = true;
    wipe(leaf_priv, sizeof(leaf_priv));

    // the Welcome, for those it adds: the GroupInfo (with the tree in it), and each one's secrets
    bool adds = false;
    for (size_t i = 0; i < sizeof(new_leaves); i++) adds = adds || new_leaves[i];
    if (adds) {
        uint8_t* gi = g_gi;
        Writer g(gi, sizeof(g_gi) - GCM_TAG);
        g.raw(g_gc, ngc.n);
        size_t exts = g.begin();
        g.u16(EXT_RATCHET_TREE);
        size_t data = g.begin();
        size_t tree_at = g.begin();
        g_prov.encode(g);
        g.end(tree_at);
        g.end(data);
        g.end(exts);
        g.vec(tag, MLS_NH);
        g.u32(my_leaf_);
        if (!g.ok || !mlsSign(sig_priv_, "GroupInfoTBS", gi, g.n, sig, &sig_n)) return fail(Mls_TooBig, __LINE__);
        g.vec(sig, sig_n);
        if (!g.ok) return fail(Mls_TooBig, __LINE__);
        uint8_t key[MLS_NK], nonce[MLS_NN];
        mlsExpand(next.welcome, "key", nullptr, 0, key, MLS_NK);
        mlsExpand(next.welcome, "nonce", nullptr, 0, nonce, MLS_NN);
        uint8_t* egi = gi;  // (sealed in place: the tag goes after it)
        Gcm aead;
        aead.init(key, MLS_NK);
        aead.seal(nonce, nullptr, 0, gi, g.n, egi, egi + g.n, GCM_TAG);
        aead.wipeKey();
        wipe(key, sizeof(key));
        size_t egi_n = g.n + GCM_TAG;

        welcome_out.u16(MLS_SUITE);
        size_t list = welcome_out.begin();
        for (size_t i = 0; i < proposals_n_; i++) {
            if (proposals_[i].type != Prop_Add) continue;
            Reader pr(pool_ + proposals_[i].off, proposals_[i].len);
            pr.u16();
            KeyPackageView kp;
            if (!readKeyPackage(pr, &kp)) return fail(Mls_Malformed, __LINE__);
            // where it went in the tree, and the path secret of the first node above both it and us
            uint32_t li = 0;
            LeafView lv;
            while (li < g_prov.leaves() && !(g_prov.leaf(li, &lv) && lv.all == kp.leaf.all)) li++;
            if (li == g_prov.leaves()) return fail(Mls_Malformed, __LINE__);
            size_t step = fn;
            for (size_t s = 0; s < fn && step == fn; s++)
                if (inSubtree(copath[s], mlstree::leafNode(li))) step = s;
            // GroupSecrets {joiner_secret<V>, optional<PathSecret>, psks<V>}
            uint8_t gs[128];
            Writer w(gs, sizeof(gs));
            w.vec(next.joiner, MLS_NH);
            if (step < fn) {
                w.u8(1);
                w.vec(secrets[step], MLS_NH);
            } else {
                w.u8(0);
            }
            w.vec(nullptr, 0);
            uint8_t ref[MLS_NH], ikm[32], kem[HPKE_ENC], ct[sizeof(gs) + GCM_TAG];
            mlsRefHash("MLS 1.0 KeyPackage Reference", kp.all.p, kp.all.n, ref);
            rnd(ikm, sizeof(ikm));
            bool ok = w.ok && kp.init_key.n == P256_PUB && mlsEncrypt(kp.init_key.p, "Welcome", egi, egi_n, gs, w.n, ikm, kem, ct);
            wipe(ikm, sizeof(ikm));
            wipe(gs, sizeof(gs));
            if (!ok) return fail(Mls_Malformed, __LINE__);
            welcome_out.vec(ref, MLS_NH);
            welcome_out.vec(kem, sizeof(kem));
            welcome_out.vec(ct, w.n + GCM_TAG);
        }
        welcome_out.end(list);
        welcome_out.vec(egi, egi_n);
        if (!welcome_out.ok) return fail(Mls_TooBig, __LINE__);
        *welcome = true;
    }
    for (size_t i = 0; i < fn; i++) wipe(secrets[i], MLS_NH);
    wipe(first, sizeof(first));
    next.wipeAll();
    return Mls_Ok;
}
