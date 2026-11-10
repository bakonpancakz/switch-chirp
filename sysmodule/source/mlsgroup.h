// One MLS group, as DAVE uses it (RFC 9420, ciphersuite 2): we join through
// a Welcome, then follow it commit by commit (handshake messages are always
// PublicMessages in DAVE; proposals come from members or from the voice
// gateway as the group's external sender). What DAVE needs out of it is the
// exporter, for each speaker's media key.
//
// Fixed size: the tree in its arena (and one more for a commit being
// checked), proposals in a pool, our private keys for our own direct path.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mlskeys.h"
#include "ratchet.h"

#define MLS_GROUP_ID_MAX 64
#define MLS_EXTENSIONS_MAX 1024
#define MLS_PROPOSALS 64
#define MLS_PROPOSAL_POOL (32 * 1024)
#define MLS_PATH_KEYS 8  // (a 128-leaf tree is 7 levels above the leaves)

enum MlsWireFormat : uint16_t { Wire_Public = 1, Wire_Private = 2, Wire_Welcome = 3, Wire_GroupInfo = 4, Wire_KeyPackage = 5 };
enum MlsProposalType : uint16_t { Prop_Add = 1, Prop_Update = 2, Prop_Remove = 3, Prop_Psk = 4, Prop_ReInit = 5, Prop_ExternalInit = 6, Prop_Gce = 7 };
enum MlsResult : uint8_t {
    Mls_Ok = 0,
    Mls_Malformed,      // not what it says it is
    Mls_WrongGroup,     // another group, or another epoch
    Mls_BadSignature,   // a signature or MAC didn't check out
    Mls_BadTree,        // the tree isn't valid (parent hashes, leaf signatures, tree hash)
    Mls_NotForUs,       // a Welcome without our key package in it
    Mls_Unsupported,    // something DAVE never uses (PSKs, private messages, x509...)
    Mls_UnknownProposal,// a commit names a proposal we don't have
    Mls_Removed,        // the commit took us out of the group
    Mls_TooBig,         // a fixed table is full
};

class MlsGroup {
public:
    typedef void (*Random)(void* out, size_t n);

    // A fresh key package for identity (DAVE's: the 8-byte big-endian user id), with a new
    // signature key: written as an MLSMessage into w, and kept as ours
    bool makeKeyPackage(const uint8_t* identity, size_t identity_n, Random rnd, Writer& w);
    // Our key package's keys and bytes (the KeyPackage struct, not wrapped in an MLSMessage), as
    // a test gives them; until a Welcome, that's all there is
    bool setKeyPackage(const Bytes& key_package, const uint8_t init_priv[P256_PRIV], const uint8_t leaf_priv[P256_PRIV],
                       const uint8_t sig_priv[P256_PRIV]);
    // A new group with only us in it (after makeKeyPackage), at epoch 0; extensions: the group
    // context's (DAVE's carries the voice gateway as external sender)
    bool create(const Bytes& group_id, const Bytes& extensions, Random rnd);
    // A commit of every proposal we have, with a fresh path (MLSMessage into commit_out), and a
    // Welcome for those it adds (MLSMessage into welcome_out; *welcome says whether there is one).
    // It's kept pending: it takes effect when it comes back through handle(), as DAVE's gateway
    // announces it (or not at all, if another member's commit is announced first).
    // (commit_out and welcome_out may be one Writer: the commit is finished before the Welcome is begun)
    MlsResult commit(Random rnd, Writer& commit_out, Writer& welcome_out, bool* welcome);
    size_t pendingProposals() const { return proposals_n_; }
    // An external pre-shared key the group may use (DAVE never does; the MLS test vectors do)
    bool addExternalPsk(const Bytes& id, const Bytes& psk);
    // Join from a Welcome (the struct, not wrapped); ratchet_tree: the tree as it comes
    // (ratchet_tree<V> with its length), when the GroupInfo doesn't carry it
    MlsResult join(const Bytes& welcome, const Bytes& ratchet_tree = Bytes{});
    // A handshake message (MLSMessage): a proposal is kept for a commit to come, a commit moves the epoch
    // (used: when messages come one after another, set to the bytes this one took; null: it must be all of msg)
    MlsResult handle(const Bytes& mls_message, size_t* used = nullptr);

    bool joined() const { return joined_; }
    uint64_t epoch() const { return epoch_; }
    // Who the last commit() added (DAVE: the user ids in their credentials; 0 for one that isn't 8 bytes)
    size_t added(const uint64_t** ids) const { *ids = added_; return added_n_; }
    uint32_t myLeaf() const { return my_leaf_; }
    const RatchetTree& tree() const { return tree_; }
    const uint8_t* epochAuthenticator() const { return keys_.epoch_authenticator; }
    bool exporter(const void* label, size_t label_n, const void* ctx, size_t ctx_n, uint8_t* out, size_t len) const {
        return joined_ && mlsExport(keys_.exporter, label, label_n, ctx, ctx_n, out, len);
    }
    const uint8_t* exporterSecret() const { return keys_.exporter; }
    // A cached proposal taken back (DAVE's gateway revokes them by reference)
    void revokeProposal(const Bytes& ref);
    // The external sender our group context names, first of them (to compare with the gateway's)
    Bytes externalSender() const { return externalSenderKey(0); }
    Bytes groupId() const { return Bytes{group_id_, group_id_n_}; }
    void reset();
    // Where the last failure was found: a line in mlsgroup.cpp (for the log)
    int whyLine() const { return why_line_; }

private:
    MlsResult fail(MlsResult r, int line);
    mutable int why_line_ = 0;
    struct PathKey {
        uint32_t node;
        bool used;
        uint8_t priv[P256_PRIV];
        uint8_t pub[P256_PUB];
    };
    struct Proposal {
        uint8_t ref[MLS_NH];
        uint16_t type;
        uint8_t sender_type;
        uint32_t sender;  // leaf index (member) or external sender index
        uint32_t off, len;  // the Proposal struct, in the pool
    };

    bool writeGroupContext(Writer& w, uint64_t epoch, const uint8_t tree_hash[MLS_NH], const uint8_t* confirmed, size_t confirmed_n,
                           const Bytes& extensions) const;
    bool setPathKey(uint32_t node, const uint8_t priv[P256_PRIV]);
    const PathKey* pathKey(uint32_t node) const;
    void dropStaleKeys();
    bool keyFor(uint32_t node, uint8_t priv[P256_PRIV]) const;  // our private key for a node (our leaf, or on our path)
    Bytes externalSenderKey(uint32_t index) const;
    MlsResult handleCommit(const Bytes& content, const Bytes& content_tail, uint32_t sender, const Bytes& signature,
                           const Bytes& confirmation_tag);
    MlsResult applyProposal(RatchetTree& t, const Bytes& proposal, uint32_t sender, uint8_t* new_leaves, Bytes* gce);
    // psk_secret from PreSharedKeyIDs (RFC 9420 8.4), each `ids` entry pointing at one in the message
    bool pskSecret(const Bytes* ids, size_t n, uint8_t out[MLS_NH]) const;

    struct Psk {
        uint8_t id[64], psk[64];
        size_t id_n, psk_n;
    };
    Psk psks_[4];
    size_t psks_n_ = 0;

    // our commit, until it's announced: what its path came from
    bool pending_ = false;
    uint8_t pending_ref_[MLS_NH];      // (a hash of the commit's FramedContent, to know it when it's back)
    uint8_t pending_leaf_priv_[P256_PRIV];
    uint8_t pending_path_secret_[MLS_NH];
    void clearProposals();

    // ours
    bool have_kp_ = false;
    uint8_t kp_ref_[MLS_NH];
    uint8_t init_priv_[P256_PRIV], leaf_priv_[P256_PRIV], sig_priv_[P256_PRIV];
    uint8_t kp_leaf_[2048];
    size_t kp_leaf_n_ = 0;

    // the group
    bool joined_ = false;
    uint8_t group_id_[MLS_GROUP_ID_MAX];
    size_t group_id_n_ = 0;
    uint64_t epoch_ = 0;
    uint8_t extensions_[MLS_EXTENSIONS_MAX];
    size_t extensions_n_ = 0;
    uint8_t confirmed_[MLS_NH], interim_[MLS_NH];
    size_t confirmed_n_ = MLS_NH;  // (0 at a new group's epoch 0: the transcript hasn't begun)
    uint8_t tree_hash_[MLS_NH];
    uint32_t my_leaf_ = 0;
    uint64_t added_[8] = {0};
    size_t added_n_ = 0;
    RatchetTree tree_;
    PathKey path_keys_[MLS_PATH_KEYS];
    EpochKeys keys_;

    Proposal proposals_[MLS_PROPOSALS];
    size_t proposals_n_ = 0;
    uint8_t pool_[MLS_PROPOSAL_POOL];
    size_t pool_n_ = 0;
};
