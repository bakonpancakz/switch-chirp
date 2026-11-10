#include "ratchet.h"

#include <string.h>

#include "mlstree.h"

namespace {

bool readCredential(Reader& r, uint16_t* type, Bytes* identity) {
    *type = r.u16();
    if (*type == CREDENTIAL_BASIC) {
        *identity = r.vec();
    } else if (*type == 2) {  // x509: certificates<V>, skipped (DAVE only allows basic)
        r.vec();
        *identity = Bytes{};
    } else {
        return false;
    }
    return r.ok;
}

}  // namespace

bool readLeafNode(Reader& r, LeafView* out) {
    const uint8_t* start = r.p;
    out->encryption_key = r.vec();
    out->signature_key = r.vec();
    if (!readCredential(r, &out->credential_type, &out->identity)) return false;
    const uint8_t* cap = r.p;
    for (int i = 0; i < 5; i++) r.vec();  // versions, cipher_suites, extensions, proposals, credentials
    out->capabilities = Bytes{cap, (size_t)(r.p - cap)};
    out->source = r.u8();
    out->not_before = out->not_after = 0;
    out->parent_hash = Bytes{};
    if (out->source == Leaf_KeyPackage) {
        out->not_before = r.u64();
        out->not_after = r.u64();
    } else if (out->source == Leaf_Commit) {
        out->parent_hash = r.vec();
    } else if (out->source != Leaf_Update) {
        return false;
    }
    out->extensions = r.vec();
    out->tbs_fields = Bytes{start, (size_t)(r.p - start)};
    out->signature = r.vec();
    out->all = Bytes{start, (size_t)(r.p - start)};
    return r.ok;
}

bool readParentNode(Reader& r, ParentView* out) {
    const uint8_t* start = r.p;
    out->encryption_key = r.vec();
    out->parent_hash = r.vec();
    out->unmerged = r.vec();
    out->all = Bytes{start, (size_t)(r.p - start)};
    return r.ok && out->unmerged.n % 4 == 0;
}

bool verifyLeafSignature(const LeafView& leaf, const Bytes& group_id, uint32_t leaf_index) {
    // LeafNodeTBS: the fields, then (for update and commit leaves) group_id<V> and leaf_index
    uint8_t buf[4096];
    Writer w(buf, sizeof(buf));
    w.raw(leaf.tbs_fields.p, leaf.tbs_fields.n);
    if (leaf.source == Leaf_Update || leaf.source == Leaf_Commit) {
        w.vec(group_id);
        w.u32(leaf_index);
    }
    return w.ok && mlsVerify(leaf.signature_key.p, leaf.signature_key.n, "LeafNodeTBS", buf, w.n, leaf.signature.p, leaf.signature.n);
}

// --- the tree ---

void RatchetTree::clear() {
    leaves_ = 0;
    used_ = 0;
    memset(types_, 0, sizeof(types_));
}

Bytes RatchetTree::raw(uint32_t node) const {
    if (node >= nodes() || types_[node] == Node_Blank) return Bytes{};
    return Bytes{arena_ + off_[node], len_[node]};
}

bool RatchetTree::leaf(uint32_t leaf_index, LeafView* out) const {
    uint32_t n = mlstree::leafNode(leaf_index);
    if (type(n) != Node_Leaf) return false;
    Reader r(raw(n));
    return readLeafNode(r, out);
}

bool RatchetTree::parent(uint32_t node, ParentView* out) const {
    if (type(node) != Node_Parent) return false;
    Reader r(raw(node));
    return readParentNode(r, out);
}

bool RatchetTree::compact() {
    // live nodes slid down to the arena's start, in the order they're in it (so a move never
    // lands on one not moved yet): no second arena needed
    static uint16_t order[RATCHET_NODES];
    size_t n = 0;
    for (uint32_t i = 0; i < nodes(); i++)
        if (types_[i] != Node_Blank) order[n++] = (uint16_t)i;
    for (size_t a = 1; a < n; a++) {
        uint16_t v = order[a];
        size_t b = a;
        while (b > 0 && off_[order[b - 1]] > off_[v]) order[b] = order[b - 1], b--;
        order[b] = v;
    }
    size_t at = 0;
    for (size_t k = 0; k < n; k++) {
        uint16_t i = order[k];
        memmove(arena_ + at, arena_ + off_[i], len_[i]);
        off_[i] = (uint16_t)at;
        at += len_[i];
    }
    used_ = at;
    return true;
}

bool RatchetTree::set(uint32_t node, NodeType t, const Bytes& bytes) {
    if (node >= nodes()) return false;
    types_[node] = Node_Blank;
    if (t == Node_Blank) return true;
    if (bytes.n > 0xffff) return false;
    if (used_ + bytes.n > RATCHET_ARENA) {
        compact();
        if (used_ + bytes.n > RATCHET_ARENA) return false;
    }
    memcpy(arena_ + used_, bytes.p, bytes.n);
    off_[node] = (uint16_t)used_;
    len_[node] = (uint16_t)bytes.n;
    used_ += bytes.n;
    types_[node] = t;
    return true;
}

bool RatchetTree::grow(uint32_t leaf_index) {
    while (leaf_index >= leaves_) {
        uint32_t next = leaves_ ? leaves_ * 2 : 1;
        if (next > RATCHET_LEAVES) return false;
        for (uint32_t i = nodes(); i < 2 * next - 1; i++) types_[i] = Node_Blank;
        leaves_ = next;
    }
    return true;
}

void RatchetTree::truncate() {
    while (leaves_ > 1) {
        bool rightBlank = true;
        for (uint32_t i = leaves_; i < 2 * leaves_ - 1 && rightBlank; i++) rightBlank = types_[i] == Node_Blank;
        // (the right half's nodes are leaves_ .. 2 leaves_ - 2; node leaves_ - 1 is the root)
        if (!rightBlank) break;
        leaves_ /= 2;
    }
}

bool RatchetTree::decode(const Bytes& b) {
    clear();
    Reader r(b);
    uint32_t count = 0;
    static uint8_t typesIn[RATCHET_NODES];
    static Bytes bodies[RATCHET_NODES];
    while (r.n > 0 && r.ok) {
        if (count == RATCHET_NODES) return false;
        uint8_t present = r.u8();
        if (present == 0) {
            typesIn[count] = Node_Blank;
        } else if (present == 1) {
            uint8_t t = r.u8();
            const uint8_t* start = r.p;
            if (t == Node_Leaf) {
                LeafView l;
                if (!readLeafNode(r, &l)) return false;
            } else if (t == Node_Parent) {
                ParentView p;
                if (!readParentNode(r, &p)) return false;
            } else {
                return false;
            }
            typesIn[count] = t;
            bodies[count] = Bytes{start, (size_t)(r.p - start)};
        } else {
            return false;
        }
        // (leaves at even places, parents at odd ones)
        if (typesIn[count] != Node_Blank && (typesIn[count] == Node_Leaf) != (count % 2 == 0)) return false;
        count++;
    }
    if (!r.ok || count == 0 || typesIn[count - 1] == Node_Blank) return false;  // (the last node can't be blank: it'd be trimmed)
    // the tree is the smallest full one that holds them
    uint32_t leafCount = (count + 1) / 2;
    if (!grow(leafCount - 1)) return false;
    for (uint32_t i = 0; i < count; i++)
        if (typesIn[i] != Node_Blank && !set(i, (NodeType)typesIn[i], bodies[i])) return false;
    return true;
}

bool RatchetTree::encode(Writer& w) const {
    // (blank nodes at the end are left out)
    uint32_t last = nodes();
    while (last > 0 && types_[last - 1] == Node_Blank) last--;
    for (uint32_t i = 0; i < last; i++) {
        if (types_[i] == Node_Blank) {
            w.u8(0);
        } else {
            w.u8(1);
            w.u8(types_[i]);
            Bytes b = raw(i);
            w.raw(b.p, b.n);
        }
    }
    return w.ok;
}

int RatchetTree::resolution(uint32_t node, uint32_t* out, int cap) const {
    if (node >= nodes()) return 0;
    if (types_[node] == Node_Leaf) {
        if (cap < 1) return -1;
        out[0] = node;
        return 1;
    }
    if (types_[node] == Node_Parent) {
        ParentView p;
        if (!parent(node, &p) || cap < 1 + (int)p.unmergedCount()) return -1;
        out[0] = node;
        for (size_t i = 0; i < p.unmergedCount(); i++) out[1 + i] = mlstree::leafNode(p.unmergedAt(i));
        return 1 + (int)p.unmergedCount();
    }
    if (mlstree::level(node) == 0) return 0;  // (a blank leaf)
    int a = resolution(mlstree::left(node), out, cap);
    if (a < 0) return -1;
    int b = resolution(mlstree::right(node), out + a, cap - a);
    return b < 0 ? -1 : a + b;
}

namespace {

bool isExcluded(const uint8_t* excluded, uint32_t leaf) { return excluded && (excluded[leaf / 8] >> (leaf % 8)) & 1; }

}  // namespace

bool RatchetTree::hashNode(uint32_t node, uint8_t out[MLS_NH], const uint8_t* excluded) const {
    // TreeHashInput: node_type, then a leaf's {leaf_index, optional<LeafNode>}
    // or a parent's {optional<ParentNode>, left_hash<V>, right_hash<V>}
    Sha256 h;
    h.init();
    uint8_t hdr[8];
    if (mlstree::level(node) == 0) {
        uint32_t li = node / 2;
        bool present = types_[node] == Node_Leaf && !isExcluded(excluded, li);
        hdr[0] = Node_Leaf;
        hdr[1] = (uint8_t)(li >> 24), hdr[2] = (uint8_t)(li >> 16), hdr[3] = (uint8_t)(li >> 8), hdr[4] = (uint8_t)li;
        hdr[5] = present ? 1 : 0;
        h.update(hdr, 6);
        if (present) {
            Bytes b = raw(node);
            h.update(b.p, b.n);
        }
    } else {
        uint8_t l[MLS_NH], r[MLS_NH];
        if (!hashNode(mlstree::left(node), l, excluded) || !hashNode(mlstree::right(node), r, excluded)) return false;
        hdr[0] = Node_Parent;
        hdr[1] = types_[node] == Node_Parent ? 1 : 0;
        h.update(hdr, 2);
        if (types_[node] == Node_Parent) {
            ParentView p;
            if (!parent(node, &p)) return false;
            if (!excluded) {
                h.update(p.all.p, p.all.n);
            } else {
                // (the node as it was before the excluded leaves joined: out of its unmerged list)
                uint8_t buf[RATCHET_LEAVES * 4 + 16];
                Writer w(buf, sizeof(buf));
                size_t at = w.begin();
                for (size_t i = 0; i < p.unmergedCount(); i++)
                    if (!isExcluded(excluded, p.unmergedAt(i))) w.u32(p.unmergedAt(i));
                w.end(at);
                // (encryption_key<V> and parent_hash<V> as they are, then the filtered list)
                Reader rr(p.all);
                rr.vec();
                rr.vec();
                h.update(p.all.p, (size_t)(rr.p - p.all.p));
                h.update(buf, w.n);
            }
        }
        uint8_t len = MLS_NH;  // (32 fits a 1-byte length)
        h.update(&len, 1);
        h.update(l, MLS_NH);
        h.update(&len, 1);
        h.update(r, MLS_NH);
    }
    h.final(out);
    return true;
}

bool RatchetTree::treeHash(uint32_t node, uint8_t out[MLS_NH]) const { return node < nodes() && hashNode(node, out); }

bool RatchetTree::rootHash(uint8_t out[MLS_NH]) const { return leaves_ && hashNode(mlstree::root(leaves_), out); }

bool RatchetTree::parentHash(uint32_t p, uint32_t s, uint8_t out[MLS_NH]) const {
    ParentView pv;
    if (!parent(p, &pv)) return false;
    uint8_t excluded[RATCHET_LEAVES / 8] = {0};
    for (size_t i = 0; i < pv.unmergedCount(); i++) {
        uint32_t li = pv.unmergedAt(i);
        if (li < RATCHET_LEAVES) excluded[li / 8] |= (uint8_t)(1 << (li % 8));
    }
    uint8_t sibling[MLS_NH];
    if (!hashNode(s, sibling, excluded)) return false;
    // ParentHashInput {encryption_key<V>, parent_hash<V>, original_sibling_tree_hash<V>}
    Sha256 h;
    h.init();
    uint8_t buf[8];
    Writer w(buf, sizeof(buf));
    w.varint((uint32_t)pv.encryption_key.n);
    h.update(buf, w.n);
    h.update(pv.encryption_key.p, pv.encryption_key.n);
    Writer w2(buf, sizeof(buf));
    w2.varint((uint32_t)pv.parent_hash.n);
    h.update(buf, w2.n);
    h.update(pv.parent_hash.p, pv.parent_hash.n);
    uint8_t len = MLS_NH;
    h.update(&len, 1);
    h.update(sibling, MLS_NH);
    h.final(out);
    return true;
}

Bytes RatchetTree::carriedParentHash(uint32_t node) const {
    if (type(node) == Node_Parent) {
        ParentView p;
        return parent(node, &p) ? p.parent_hash : Bytes{};
    }
    if (type(node) == Node_Leaf) {
        LeafView l;
        return leaf(node / 2, &l) && l.source == Leaf_Commit ? l.parent_hash : Bytes{};
    }
    return Bytes{};
}

bool RatchetTree::verifyParentHashes() const {
    for (uint32_t p = 0; p < nodes(); p++) {
        if (type(p) != Node_Parent) continue;
        ParentView pv;
        if (!parent(p, &pv)) return false;
        int found = 0;
        for (int side = 0; side < 2; side++) {
            uint32_t c = side == 0 ? mlstree::left(p) : mlstree::right(p);
            uint32_t s = side == 0 ? mlstree::right(p) : mlstree::left(p);
            uint8_t ph[MLS_NH];
            if (!parentHash(p, s, ph)) return false;
            uint32_t res[RATCHET_NODES];
            int n = resolution(c, res, RATCHET_NODES);
            if (n < 0) return false;
            // P's unmerged leaves under C (as leaf nodes)
            uint32_t lo = c - ((1u << mlstree::level(c)) - 1), hi = c + ((1u << mlstree::level(c)) - 1);
            for (int k = 0; k < n; k++) {
                uint32_t d = res[k];
                Bytes carried = carriedParentHash(d);
                if (carried.n != MLS_NH || memcmp(carried.p, ph, MLS_NH) != 0) continue;
                // the resolution of C without D must be exactly P's unmerged leaves under C
                int expect = 0;
                bool all = true;
                for (size_t u = 0; u < pv.unmergedCount(); u++) {
                    uint32_t node = mlstree::leafNode(pv.unmergedAt(u));
                    if (node < lo || node > hi) continue;
                    expect++;
                    bool in = false;
                    for (int j = 0; j < n; j++) in = in || (res[j] == node && j != k);
                    all = all && in;
                }
                if (all && expect == n - 1) found++;
            }
        }
        if (found != 1) return false;
    }
    return true;
}
