#pragma once
#include <cstdint>
#include <vector>

namespace rx::core {

template <typename Tag>
class Handle {
public:
    Handle() = default;
    Handle(uint32_t index, uint32_t generation) : index_(index), generation_(generation) {}

    uint32_t index() const { return index_; }
    uint32_t generation() const { return generation_; }
    bool isValid() const { return generation_ != 0; }

    bool operator==(const Handle& other) const {
        return index_ == other.index_ && generation_ == other.generation_;
    }

private:
    uint32_t index_ = 0;
    uint32_t generation_ = 0;
};

template <typename Tag, typename T>
class HandlePool {
public:
    Handle<Tag> acquire(T value) {
        if (!freeList_.empty()) {
            uint32_t idx = freeList_.back();
            freeList_.pop_back();
            slots_[idx].value = std::move(value);
            slots_[idx].generation += 1;
            slots_[idx].alive = true;
            return Handle<Tag>(idx, slots_[idx].generation);
        }
        slots_.push_back(Slot{std::move(value), /*generation=*/1, /*alive=*/true});
        return Handle<Tag>(static_cast<uint32_t>(slots_.size() - 1), 1);
    }

    void release(Handle<Tag> handle) {
        if (!isLive(handle)) {
            return;
        }
        slots_[handle.index()].alive = false;
        freeList_.push_back(handle.index());
    }

    T* get(Handle<Tag> handle) {
        if (!isLive(handle)) {
            return nullptr;
        }
        return &slots_[handle.index()].value;
    }

private:
    struct Slot {
        T value;
        uint32_t generation = 0;
        bool alive = false;
    };

    bool isLive(Handle<Tag> handle) const {
        return handle.index() < slots_.size() &&
               slots_[handle.index()].alive &&
               slots_[handle.index()].generation == handle.generation();
    }

    std::vector<Slot> slots_;
    std::vector<uint32_t> freeList_;
};

}  // namespace rx::core
