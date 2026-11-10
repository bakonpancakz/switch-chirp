#include "framequeue.h"

void FrameQueue::init(uint8_t* ring, size_t size) {
    ring_ = ring, size_ = size;
    head_ = tail_ = 0;
    holding_ = false;
}

uint8_t* FrameQueue::room(size_t need) const {
    uint32_t h = head_, t = tail();
    if (h - t >= SLOTS) return nullptr;
    if (h == t) return need <= size_ ? ring_ : nullptr;
    const Frame& first = q_[t % SLOTS];
    const Frame& last = q_[(h - 1) % SLOTS];
    if (!inRing(first.p) || !inRing(last.p)) return nullptr;  // (one of its own memory: it goes first)
    size_t f = (size_t)(first.p - ring_), e = (size_t)(last.p + last.cap - ring_);
    if (e > f) {  // (the queued ones run from f to e: room after them, else at the start before f)
        if (size_ - e >= need) return ring_ + e;
        return f >= need ? ring_ : nullptr;
    }
    return f - e >= need ? ring_ + e : nullptr;  // (they wrapped: room between the end and the start)
}

void FrameQueue::push(uint8_t* p, size_t n, size_t cap, uint64_t tick, bool gap) {
    q_[head_ % SLOTS] = Frame{p, n, cap, tick, gap};
    __atomic_store_n(&head_, head_ + 1, __ATOMIC_RELEASE);
}

bool FrameQueue::empty() const { return head() == tail(); }

bool FrameQueue::take(Frame* f) {
    if (holding_ || head() == tail_) return false;
    *f = q_[tail_ % SLOTS];
    holding_ = true;
    return true;
}

void FrameQueue::done() {
    if (!holding_) return;
    holding_ = false;
    __atomic_store_n(&tail_, tail_ + 1, __ATOMIC_RELEASE);
}

void FrameQueue::drop() {
    if (holding_) return;
    __atomic_store_n(&tail_, head(), __ATOMIC_RELEASE);
}
