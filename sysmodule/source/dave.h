// A DAVE session (Discord's protocol.md): the voice gateway's opcodes 21-31
// driving our MLS group, and each speaker's frames decrypted with the keys of
// the epoch they were sent in. Binary opcodes come as [opcode][body] (the
// voice websocket's sequence number already taken off); what we send goes
// out through a DaveOut. The previous epoch's keys are kept 10 seconds after
// a transition, as libdave does, so frames sent around it still open.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "daveframe.h"
#include "mlsgroup.h"

#define DAVE_RATCHETS 24           // speakers' ratchets kept per epoch (made when they first speak)
#define DAVE_KEEP_OLD_MS 10000
#define DAVE_USERS 128

enum DaveOp : uint8_t {
    DaveOp_PrepareTransition = 21,
    DaveOp_ExecuteTransition = 22,
    DaveOp_TransitionReady = 23,
    DaveOp_PrepareEpoch = 24,
    DaveOp_ExternalSender = 25,
    DaveOp_KeyPackage = 26,
    DaveOp_Proposals = 27,
    DaveOp_CommitWelcome = 28,
    DaveOp_AnnounceCommit = 29,
    DaveOp_Welcome = 30,
    DaveOp_InvalidCommitWelcome = 31,
};

class DaveOut {
public:
    virtual ~DaveOut() {}
    virtual void binary(uint8_t op, const uint8_t* body, size_t n) = 0;  // [op][body]
    virtual void json(const char* text) = 0;
    virtual void log(const char* line) = 0;
    // Room for our commit and its Welcome (RATCHET_ARENA + 16 KB at least). It may be the buffer the
    // message being handled sits in: a commit is only written once the proposals have been read in
    virtual uint8_t* scratch(size_t* cap) = 0;
};

class DaveSession {
public:
    void start(uint64_t me, uint64_t channel, DaveOut* out, MlsGroup::Random rnd);
    // Session Description's dave_protocol_version (0: transport encryption only)
    void onProtocol(int version, uint64_t now_ms);
    void onBinary(uint8_t op, const uint8_t* body, size_t n, uint64_t now_ms);
    void onPrepareTransition(int version, uint32_t transition, uint64_t now_ms);
    void onExecuteTransition(uint32_t transition, uint64_t now_ms);
    void onPrepareEpoch(int version, uint64_t epoch, uint64_t now_ms);
    void onClientsConnect(const uint64_t* users, size_t n);
    void onClientDisconnect(uint64_t user);

    // A speaker's frame: decrypted into out (n bytes at least); silence and passthrough copied as they are
    bool decrypt(uint64_t user, const uint8_t* frame, size_t n, uint8_t* out, size_t* out_n, uint64_t now_ms);
    // Our own frame, to send: with our key for the current epoch (cap: n + DAVE_SUPPLEMENTAL_MAX).
    // Silence, and calls without DAVE, go as they are; false while a DAVE call has no group yet
    bool encrypt(const uint8_t* frame, size_t n, uint8_t* out, size_t cap, size_t* out_n);
    // Our own H.264 frame, in place (cap: its buffer's size): DAVE's video rules (daveframe.h). The same
    // nonce counter as the sound: one key, so a nonce is never used twice across them
    bool encryptVideo(uint8_t* frame, size_t n, size_t cap, size_t* out_n);
    int version() const { return version_; }
    bool ready() const { return current_.used; }
    const MlsGroup& group() const { return group_; }
    uint32_t failures() const { return failures_; }
    // Frames since the last time this was asked: Discord's silence, plain (no DAVE on them), opened with a key
    void takeFrameStats(uint32_t* silence, uint32_t* plain, uint32_t* opened) {
        *silence = silence_, *plain = plain_, *opened = opened_;
        silence_ = plain_ = opened_ = 0;
    }

private:
    struct Epoch {
        bool used;
        uint64_t expires_ms;  // (0: the current one; it doesn't)
        uint8_t exporter[MLS_NH];
        DaveRatchet ratchets[DAVE_RATCHETS];
        size_t ratchets_n;
        void clear() { wipe(this, sizeof(*this)); }
        DaveRatchet* forUser(uint64_t user);
    };

    void sendKeyPackage();
    void createIfReady();
    void resetGroup();
    void invalid(uint32_t transition);
    void transitionReady(uint32_t transition, uint64_t now_ms);
    void execute(uint64_t now_ms);
    void say(const char* fmt, ...);
    void dump(const char* what, const uint8_t* p, size_t n);  // (into the log, in hex: for comparing with Discord's)

    DaveOut* out_ = nullptr;
    MlsGroup::Random rnd_ = nullptr;
    uint64_t me_ = 0, channel_ = 0;
    int version_ = 0, pending_version_ = -1;
    uint32_t pending_transition_ = 0;
    bool have_pending_ = false;
    uint8_t external_[512];  // the gateway's ExternalSender, as it sent it
    size_t external_n_ = 0;
    MlsGroup group_;
    Epoch current_, previous_, next_;  // next_: an epoch made, waiting for its transition
    uint64_t passthrough_until_ = 0;
    uint64_t users_[DAVE_USERS];  // who's connected (proposals to add anyone else are refused)
    size_t users_n_ = 0;
    uint32_t failures_ = 0, silence_ = 0, plain_ = 0, opened_ = 0;
    uint32_t counter_ = 0;  // our frames' nonce (only ever goes up: a key never sees one twice)
};
