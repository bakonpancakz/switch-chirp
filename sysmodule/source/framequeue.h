// The frames waiting to be sent: copied out of grc:d's buffer into a ring, so the recorder is read again
// at once (grc keeps no frame nobody's reading, and a keyframe takes longer than a frame's time to get
// out over Wi-Fi; the small frames after it give the sender time to catch up).
//
// One writer (capture: at the head), one reader (the stream's connection: at the tail). Each frame sits
// whole in the ring, where there's room after the newest (or back at its start). A frame may also be
// queued from memory of its own (one too big for the ring): no room is found until it's gone.
// Nothing here knows about the Switch: test/unit_test.cpp tests it on a PC.
#pragma once
#include <stddef.h>
#include <stdint.h>

class FrameQueue {
public:
    static constexpr uint32_t SLOTS = 32;  // (frames at most: half a second at 60)

    struct Frame {
        uint8_t* p;
        size_t n, cap;  // its bytes, and the room it has (it may grow in place, up to cap)
        uint64_t tick;  // when it was recorded (the system's ticks)
        bool gap;       // frames before it were dropped: the ones after refer to pictures never sent
    };

    void init(uint8_t* ring, size_t size);

    // --- the writer ---
    // Where a frame of `need` bytes would go now (null: no room, or a frame of its own memory is queued)
    uint8_t* room(size_t need) const;
    // Queued: p from room(), or memory of its own (kept until the reader's done with it)
    void push(uint8_t* p, size_t n, size_t cap, uint64_t tick, bool gap);
    bool empty() const;

    // --- the reader ---
    // The oldest frame, held until done() (one at a time); false: none
    bool take(Frame* f);
    void done();
    bool holding() const { return holding_; }
    // Everything queued thrown away (the reader's, or the writer's while the reader isn't holding one)
    void drop();

private:
    uint32_t head() const { return __atomic_load_n(&head_, __ATOMIC_ACQUIRE); }
    uint32_t tail() const { return __atomic_load_n(&tail_, __ATOMIC_ACQUIRE); }
    bool inRing(const uint8_t* p) const { return p >= ring_ && p < ring_ + size_; }

    uint8_t* ring_ = nullptr;
    size_t size_ = 0;
    Frame q_[SLOTS];
    uint32_t head_ = 0, tail_ = 0;  // (atomic: the writer moves head, the reader tail)
    bool holding_ = false;
};
