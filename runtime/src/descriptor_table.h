#pragma once

// The descriptor hash table for set 0, binding 2 (docs/d3d12_vulkan.md section 5.1 and docs/design.md):
// Shaders look up bindless resource/sampler indices by FNV-1a hashing the 8-dword descriptor key (T# or padded S#)
// and probing up to 8 slots in this table. Miss or fallback yields index 0.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <utility>
#include <vector>

namespace wavespan {

// FNV-1a constants for 32-bit dwords
inline constexpr std::uint32_t kFnvOffsetBasis = 2166136261u;
inline constexpr std::uint32_t kFnvPrime = 16777619u;
inline constexpr std::uint32_t kDescriptorProbes = 8;
inline constexpr std::uint32_t kSlotOccupiedMarker = 0xA11CA7EDu;

// Key types:
// T# key is 8 dwords.
// S# key is 4 dwords followed by 4x 0xFFFFFFFF.
using DescriptorKey = std::array<std::uint32_t, 8>;

inline std::uint32_t hash_descriptor_key(const std::uint32_t key[8]) {
    std::uint32_t h = kFnvOffsetBasis;
    for (int i = 0; i < 8; ++i) {
        h = (h ^ key[i]) * kFnvPrime;
    }
    return h;
}

inline std::uint32_t hash_descriptor_key(const DescriptorKey& key) {
    return hash_descriptor_key(key.data());
}

inline DescriptorKey make_tsharp_key(const std::uint32_t tsharp[8]) {
    DescriptorKey k{};
    std::memcpy(k.data(), tsharp, sizeof(DescriptorKey));
    return k;
}

inline DescriptorKey make_ssharp_key(const std::uint32_t ssharp[4]) {
    DescriptorKey k{};
    k[0] = ssharp[0];
    k[1] = ssharp[1];
    k[2] = ssharp[2];
    k[3] = ssharp[3];
    k[4] = 0xFFFFFFFFu;
    k[5] = 0xFFFFFFFFu;
    k[6] = 0xFFFFFFFFu;
    k[7] = 0xFFFFFFFFu;
    return k;
}

// Memory layout of each entry in the table: 10 uint32s = 40 bytes.
struct DescriptorEntry {
    std::uint32_t key[8] = {};
    std::uint32_t index = 0;
    std::uint32_t pad = 0; // 0 when empty, kSlotOccupiedMarker when occupied
};
static_assert(sizeof(DescriptorEntry) == 40);

class DescriptorTable {
public:
    enum class InsertResult { added, unchanged, replaced, added_with_relocation };

    explicit DescriptorTable(std::uint32_t initial_capacity = 16) {
        std::uint32_t cap = 16;
        while (cap < initial_capacity) cap <<= 1;
        mask_ = cap - 1;
        entries_.resize(cap);
    }

    // Inserts or updates a key -> index mapping.
    // If the key already exists in one of the 8 probe slots, its index is updated.
    // Try one bounded relocation before growing. Load never exceeds 50%.
    bool insert(const std::uint32_t key[8], std::uint32_t index) {
        insert_or_update(key, index);
        return true;
    }

    // Report the change under the same lock so the GPU caller can synchronize writes to live slots.
    InsertResult insert_or_update(const std::uint32_t key[8], std::uint32_t index) {
        std::unique_lock lock(mutex_);
        return insert_locked(key, index);
    }

    bool insert(const DescriptorKey& key, std::uint32_t index) {
        return insert(key.data(), index);
    }

    // Batch insert/update under a single exclusive lock.
    void insert_batch(std::span<const std::pair<DescriptorKey, std::uint32_t>> items,
                      std::vector<InsertResult>* results = nullptr) {
        if (results) {
            results->clear();
            results->reserve(items.size());
        }
        if (items.empty()) return;
        std::unique_lock lock(mutex_);
        for (const auto& [key, index] : items) {
            const InsertResult res = insert_locked(key.data(), index);
            if (results) results->push_back(res);
        }
    }

    // Queries the index associated with key. Returns 0 if not found.
    std::uint32_t lookup(const std::uint32_t key[8]) const {
        std::shared_lock lock(mutex_);
        const int slot = find_slot_locked(key, hash_descriptor_key(key));
        return slot >= 0 ? entries_[slot].index : 0;
    }

    std::uint32_t lookup(const DescriptorKey& key) const {
        return lookup(key.data());
    }

    // Batch lookup under a single shared reader lock.
    void lookup_batch(std::span<const DescriptorKey> keys, std::uint32_t* out_indices) const {
        if (keys.empty() || !out_indices) return;
        std::shared_lock lock(mutex_);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const int slot = find_slot_locked(keys[i].data(), hash_descriptor_key(keys[i].data()));
            out_indices[i] = slot >= 0 ? entries_[slot].index : 0;
        }
    }

    bool contains(const std::uint32_t key[8]) const {
        std::shared_lock lock(mutex_);
        return find_slot_locked(key, hash_descriptor_key(key)) >= 0;
    }

    bool contains(const DescriptorKey& key) const {
        return contains(key.data());
    }

    // Erases a key from the table. Returns true if the key was found and erased, false otherwise.
    // Only the key's own slot changes (it becomes empty): no other entry moves. Every lookup, here and in the
    // shaders (rdna2spirv.py desc_probe), compares all kDescriptorProbes slots and never stops at an empty one, so
    // a hole breaks no probe chain and inserts reuse it. Keeping the other entries in place is what lets the GPU
    // copy of the table take an erase while shaders still probe it (BindlessSet::flush_needs_idle).
    bool erase(const std::uint32_t key[8]) {
        std::unique_lock lock(mutex_);
        return erase_locked(key);
    }

    bool erase(const DescriptorKey& key) {
        return erase(key.data());
    }

    // Batch erase under a single exclusive lock. Returns the number of keys erased.
    std::size_t erase_batch(std::span<const DescriptorKey> keys) {
        if (keys.empty()) return 0;
        std::unique_lock lock(mutex_);
        std::size_t erased = 0;
        for (const auto& key : keys) {
            if (erase_locked(key.data())) ++erased;
        }
        return erased;
    }

    // Pre-grows the table to at least `min_capacity` slots (rounded up to a power of 2).
    void reserve(std::uint32_t min_capacity) {
        std::unique_lock lock(mutex_);
        if (min_capacity <= entries_.size()) return;
        std::uint32_t target = static_cast<std::uint32_t>(entries_.size());
        while (target < min_capacity && target <= (std::uint32_t(1) << 30)) target <<= 1;
        if (entries_.capacity() < target) entries_.reserve(target);
        while (entries_.size() < target) grow_locked();
    }

    void clear() {
        std::unique_lock lock(mutex_);
        std::fill(entries_.begin(), entries_.end(), DescriptorEntry{});
        occupied_count_ = 0;
        all_changed_ = true;
        changed_slots_.clear();
        changed_slots_sorted_ = true;
    }

    std::size_t size() const {
        std::shared_lock lock(mutex_);
        return occupied_count_;
    }

    std::size_t capacity() const {
        std::shared_lock lock(mutex_);
        return entries_.size();
    }

    std::uint32_t mask() const {
        std::shared_lock lock(mutex_);
        return mask_;
    }

    std::size_t size_dwords() const {
        std::shared_lock lock(mutex_);
        return 1 + entries_.size() * 10;
    }

    std::size_t size_bytes() const {
        return size_dwords() * sizeof(std::uint32_t);
    }

    // Exports the table buffer format consumed by SPIR-V set 0, binding 2.
    // Word 0 is mask; followed by entries[mask + 1] of 10 dwords: key[8], index, pad.
    std::vector<std::uint32_t> build_gpu_buffer(bool clear_pad = false) const {
        std::shared_lock lock(mutex_);
        std::vector<std::uint32_t> out(1 + entries_.size() * 10, 0);
        out[0] = mask_;
        for (std::size_t s = 0; s < entries_.size(); ++s) {
            const std::size_t base = 1 + s * 10;
            std::memcpy(&out[base], entries_[s].key, sizeof(entries_[s].key));
            out[base + 8] = entries_[s].index;
            out[base + 9] = clear_pad ? 0 : entries_[s].pad;
        }
        return out;
    }

    // What a shader's copy of the table needs re-uploaded since the last take_changes().
    struct Changes {
        bool all = false;                  // the mask (table grew or was cleared): every slot and the header
        std::vector<std::uint32_t> slots;  // slots whose 10 dwords changed, sorted and deduplicated
    };

    struct SlotRange {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };

    Changes take_changes() {
        std::unique_lock lock(mutex_);
        Changes changes;
        changes.all = all_changed_;
        if (!all_changed_ && !changed_slots_.empty()) {
            if (changed_slots_.size() > 1 && !changed_slots_sorted_) {
                std::sort(changed_slots_.begin(), changed_slots_.end());
                changed_slots_.erase(std::unique(changed_slots_.begin(), changed_slots_.end()),
                                     changed_slots_.end());
            }
            changes.slots = std::move(changed_slots_);
        }
        changed_slots_.clear();
        changed_slots_sorted_ = true;
        all_changed_ = false;
        return changes;
    }

    // One slot's 10 dwords (key[8], index, pad) to `dest`; false when the slot is out of range.
    bool read_slot(std::uint32_t slot, std::uint32_t* dest) const { return read_slots(slot, 1, dest); }

    // `count` consecutive slots' 10 dwords each to `dest`, under one lock; false when any is out of range.
    bool read_slots(std::uint32_t first, std::uint32_t count, std::uint32_t* dest) const {
        const SlotRange range{first, count};
        return read_slot_ranges(std::span<const SlotRange>(&range, 1), dest);
    }

    // Reads multiple contiguous slot ranges under one shared lock into `dest` (10 dwords per slot).
    bool read_slot_ranges(std::span<const SlotRange> ranges, std::uint32_t* dest) const {
        std::shared_lock lock(mutex_);
        for (const auto& r : ranges) {
            if (r.count > 0 && !dest) return false;
            if (r.first > entries_.size() || r.count > entries_.size() - r.first) return false;
        }
        std::uint32_t* out = dest;
        for (const auto& r : ranges) {
            for (std::uint32_t i = 0; i < r.count; ++i) {
                const DescriptorEntry& entry = entries_[r.first + i];
                std::memcpy(out, entry.key, sizeof(entry.key));
                out[8] = entry.index;
                out[9] = entry.pad;
                out += 10;
            }
        }
        return true;
    }

    // Serializes directly into `dest` (must hold at least size_bytes()) without allocating a temporary vector.
    void write_to_buffer(void* dest, bool clear_pad = false) const {
        if (!dest) return;
        std::shared_lock lock(mutex_);
        auto* out = static_cast<std::uint8_t*>(dest);
        std::memcpy(out, &mask_, sizeof(mask_));
        out += sizeof(mask_);
        for (const auto& entry : entries_) {
            std::uint32_t slot[10];
            std::memcpy(slot, entry.key, sizeof(entry.key));
            slot[8] = entry.index;
            slot[9] = clear_pad ? 0 : entry.pad;
            std::memcpy(out, slot, sizeof(slot));
            out += sizeof(slot);
        }
    }

private:
    void mark(std::uint32_t slot) {
        if (all_changed_) return;
        if (!changed_slots_.empty()) {
            const std::uint32_t prev = changed_slots_.back();
            if (slot == prev) return;
            if (slot < prev) changed_slots_sorted_ = false;
        }
        changed_slots_.push_back(slot);
    }

    int find_slot_locked(const std::uint32_t key[8], std::uint32_t hash) const {
        for (std::uint32_t k = 0; k < kDescriptorProbes; ++k) {
            const std::uint32_t slot = (hash + k) & mask_;
            const auto& entry = entries_[slot];
            if (entry.pad == kSlotOccupiedMarker && std::memcmp(entry.key, key, sizeof(entry.key)) == 0) {
                return static_cast<int>(slot);
            }
        }
        return -1;
    }

    // All eight slots for the incoming key are occupied. Try moving one resident
    // to an empty slot beyond or before that window, within its own eight probes.
    // `moved_to` receives the hole the resident moved into, so the caller can mark that slot changed too.
    static int make_probe_slot(std::vector<DescriptorEntry>& entries, std::uint32_t hash,
                               std::uint32_t* moved_to = nullptr) {
        const auto mask = static_cast<std::uint32_t>(entries.size() - 1);
        for (auto offset = kDescriptorProbes; offset < 2 * kDescriptorProbes - 1; ++offset) {
            const auto hole = (hash + offset) & mask;
            if (entries[hole].pad == kSlotOccupiedMarker) continue;
            for (auto k = kDescriptorProbes; k > 0; --k) {
                const auto slot = (hash + k - 1) & mask;
                const auto home = hash_descriptor_key(entries[slot].key);
                if (((hole - home) & mask) < kDescriptorProbes) {
                    entries[hole] = entries[slot];
                    if (moved_to) *moved_to = hole;
                    return static_cast<int>(slot); // caller immediately overwrites the resident
                }
            }
            break;
        }
        for (std::uint32_t offset = 1; offset < kDescriptorProbes; ++offset) {
            const auto hole = (hash - offset) & mask;
            if (entries[hole].pad == kSlotOccupiedMarker) continue;
            for (std::uint32_t k = 0; k < kDescriptorProbes; ++k) {
                const auto slot = (hash + k) & mask;
                const auto home = hash_descriptor_key(entries[slot].key);
                if (((hole - home) & mask) < kDescriptorProbes) {
                    entries[hole] = entries[slot];
                    if (moved_to) *moved_to = hole;
                    return static_cast<int>(slot);
                }
            }
        }
        return -1;
    }

    InsertResult insert_locked(const std::uint32_t key[8], std::uint32_t index) {
        while (true) {
            const std::uint32_t h = hash_descriptor_key(key);
            int empty_slot = -1;

            for (std::uint32_t k = 0; k < kDescriptorProbes; ++k) {
                const std::uint32_t slot = (h + k) & mask_;
                auto& entry = entries_[slot];
                if (entry.pad == kSlotOccupiedMarker) {
                    if (std::memcmp(entry.key, key, sizeof(entry.key)) == 0) {
                        if (entry.index == index) return InsertResult::unchanged;
                        entry.index = index;
                        mark(slot);
                        return InsertResult::replaced;
                    }
                } else if (empty_slot < 0) {
                    empty_slot = static_cast<int>(slot);
                }
            }

            if ((occupied_count_ + 1) * 2 > entries_.size()) {
                grow_locked();
                continue;
            }

            const bool relocate = empty_slot < 0;
            std::uint32_t moved_to = 0;
            if (relocate) empty_slot = make_probe_slot(entries_, h, &moved_to);
            if (empty_slot >= 0) {
                auto& entry = entries_[empty_slot];
                std::memcpy(entry.key, key, sizeof(entry.key));
                entry.index = index;
                entry.pad = kSlotOccupiedMarker;
                ++occupied_count_;
                if (relocate) mark(moved_to);
                mark(std::uint32_t(empty_slot));
                return relocate ? InsertResult::added_with_relocation : InsertResult::added;
            }

            grow_locked();
        }
    }

    bool erase_locked(const std::uint32_t key[8]) {
        const int found_slot = find_slot_locked(key, hash_descriptor_key(key));
        if (found_slot < 0) return false;

        // No backward shift: see erase().
        entries_[found_slot] = DescriptorEntry{};
        mark(static_cast<std::uint32_t>(found_slot));
        --occupied_count_;
        return true;
    }

    void grow_locked() {
        const std::uint32_t old_cap = static_cast<std::uint32_t>(entries_.size());
        const std::uint32_t new_cap = old_cap * 2;
        const std::uint32_t old_mask = mask_;
        const std::uint32_t new_mask = new_cap - 1;
        // Keep spare capacity ahead of growth to avoid repeated reallocations under load.
        if (entries_.capacity() < new_cap) entries_.reserve(std::size_t(new_cap) * 2);
        entries_.resize(new_cap);

        // Every occupied slot `s` is within kDescriptorProbes of `h` under `old_mask`, so
        // `(h + k) & new_mask` is either `s` or `s + old_cap`: in-place, collision-free, O(N).
        for (std::uint32_t s = 0; s < old_cap; ++s) {
            if (entries_[s].pad != kSlotOccupiedMarker) continue;
            const std::uint32_t h = hash_descriptor_key(entries_[s].key);
            const std::uint32_t k = (s - h) & old_mask;
            const std::uint32_t new_slot = (h + k) & new_mask;
            if (new_slot != s) {
                entries_[new_slot] = entries_[s];
                entries_[s] = DescriptorEntry{};
            }
        }

        mask_ = new_mask;
        all_changed_ = true;
        changed_slots_.clear();
        changed_slots_sorted_ = true;
    }

    mutable std::shared_mutex mutex_;
    std::uint32_t mask_ = 15;
    std::vector<DescriptorEntry> entries_;
    std::size_t occupied_count_ = 0;
    bool all_changed_ = false;                  // clear() or a grow: the mask and every slot changed
    bool changed_slots_sorted_ = true;          // true when changed_slots_ is strictly increasing
    std::vector<std::uint32_t> changed_slots_;  // slots inserted, replaced or moved since take_changes()
};

} // namespace wavespan
