// MLS's ratchet tree (RFC 9420 sections 7 and 12.4.3): the group's members
// at the leaves, the keys shared by each part of the tree at the parents.
// Each node is kept as its serialized bytes (a LeafNode or ParentNode) in
// one fixed arena and read when it's needed, so the tree is a fixed size
// whatever the members' credentials and extensions are like.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mlscrypto.h"
#include "mlswire.h"

#define RATCHET_LEAVES 128  // (Discord's voice channels hold 99 at most)
#define RATCHET_NODES (2 * RATCHET_LEAVES - 1)
#define RATCHET_ARENA (40 * 1024)  // (99 members: ~25 KB of leaves, ~11 KB of parents)

enum NodeType : uint8_t { Node_Blank = 0, Node_Leaf = 1, Node_Parent = 2 };
enum LeafSource : uint8_t { Leaf_KeyPackage = 1, Leaf_Update = 2, Leaf_Commit = 3 };
#define CREDENTIAL_BASIC 1

// A LeafNode, read: its fields point into the bytes it was read from
struct LeafView {
    Bytes encryption_key, signature_key;
    uint16_t credential_type;
    Bytes identity;      // (a basic credential's; DAVE's is the 8-byte big-endian user id)
    Bytes capabilities;  // (raw)
    uint8_t source;      // LeafSource
    uint64_t not_before, not_after;
    Bytes parent_hash;   // (Leaf_Commit)
    Bytes extensions;    // (raw)
    Bytes tbs_fields;    // everything before the signature, as serialized
    Bytes signature;
    Bytes all;           // the whole LeafNode
};

struct ParentView {
    Bytes encryption_key, parent_hash;
    Bytes unmerged;  // the uint32 leaf indices, raw (4 bytes each)
    Bytes all;
    size_t unmergedCount() const { return unmerged.n / 4; }
    uint32_t unmergedAt(size_t i) const {
        const uint8_t* p = unmerged.p + i * 4;
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
};

bool readLeafNode(Reader& r, LeafView* out);
bool readParentNode(Reader& r, ParentView* out);
// The leaf's signature over LeafNodeTBS (group_id and leaf index matter for update/commit leaves)
bool verifyLeafSignature(const LeafView& leaf, const Bytes& group_id, uint32_t leaf_index);

class RatchetTree {
public:
    void clear();
    // ratchet_tree as sent (optional<Node> ratchet_tree<V>'s contents); false if it isn't one
    bool decode(const Bytes& b);
    bool encode(Writer& w) const;  // (the vector's contents, as decode takes them)

    uint32_t leaves() const { return leaves_; }
    uint32_t nodes() const { return leaves_ ? 2 * leaves_ - 1 : 0; }
    NodeType type(uint32_t node) const { return node < nodes() ? (NodeType)types_[node] : Node_Blank; }
    Bytes raw(uint32_t node) const;
    bool leaf(uint32_t leaf_index, LeafView* out) const;
    bool parent(uint32_t node, ParentView* out) const;

    // Set a node (bytes: a serialized LeafNode or ParentNode), or blank it; false when the arena is full
    bool set(uint32_t node, NodeType t, const Bytes& bytes);
    void blank(uint32_t node) { set(node, Node_Blank, Bytes{}); }
    // Room for leaf_index (doubling the tree as needed), for adds
    bool grow(uint32_t leaf_index);
    // Halve the tree while its right half is all blank (after removes)
    void truncate();

    // The resolution of a node (RFC 9420 4.1.1), into out (cap entries); its size, or -1 if it didn't fit
    int resolution(uint32_t node, uint32_t* out, int cap) const;
    // The tree hash of a node (the root's: of the whole tree)
    bool treeHash(uint32_t node, uint8_t out[MLS_NH]) const;
    bool rootHash(uint8_t out[MLS_NH]) const;

    // The parent hash of parent node p with copath child s (RFC 9420 7.9): over p's key, p's own
    // parent_hash and s's "original" tree hash (p's unmerged leaves taken out of it)
    bool parentHash(uint32_t p, uint32_t s, uint8_t out[MLS_NH]) const;
    // The parent_hash a node carries (a parent's, or a commit-sourced leaf's); empty if none
    Bytes carriedParentHash(uint32_t node) const;
    // Every non-blank parent chained back to a leaf by parent hashes (7.9.2, top down)
    bool verifyParentHashes() const;

private:
    bool compact();
    // excluded: a bitmap of leaves to count as blank (and out of unmerged lists), or null
    bool hashNode(uint32_t node, uint8_t out[MLS_NH], const uint8_t* excluded = nullptr) const;

    uint32_t leaves_ = 0;
    uint8_t types_[RATCHET_NODES];
    uint16_t off_[RATCHET_NODES], len_[RATCHET_NODES];
    uint8_t arena_[RATCHET_ARENA];
    size_t used_ = 0;
};
