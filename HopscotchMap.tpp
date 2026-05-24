#pragma once

/**
 * @file HopscotchMap.tpp
 * @brief Template definitions for HopscotchMap.
 *
 * This file is included by HopscotchMap.hpp because HopscotchMap is a class
 * template. Users should normally include only HopscotchMap.hpp.
 */

#include <utility>

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
HopscotchMap<Key, Value, Hasher, Neighborhood>::HopscotchMap(std::size_t capacity_hint)
{
    // The user provides a capacity hint, not an exact capacity.
    // We normalize it so that all circular arithmetic can use a bitmask.
    capacity_hint = normalize_capacity(capacity_hint);
    allocate_empty(capacity_hint);
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
std::size_t HopscotchMap<Key, Value, Hasher, Neighborhood>::size() const noexcept {
    return size_;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
std::size_t HopscotchMap<Key, Value, Hasher, Neighborhood>::capacity() const noexcept {
    return capacity_;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
bool HopscotchMap<Key, Value, Hasher, Neighborhood>::empty() const noexcept {
    return size_ == 0;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
double HopscotchMap<Key, Value, Hasher, Neighborhood>::load_factor() const noexcept {
    if (capacity_ == 0) {
        return 0.0;
    }

    return static_cast<double>(size_) / static_cast<double>(capacity_);
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::max_load_factor(double lf) {
    if (lf <= 0.0 || lf >= 1.0) {
        throw std::invalid_argument("max_load_factor must be in range (0, 1).");
    }

    max_load_factor_ = lf;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
double HopscotchMap<Key, Value, Hasher, Neighborhood>::max_load_factor() const noexcept {
    return max_load_factor_;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
Value* HopscotchMap<Key, Value, Hasher, Neighborhood>::find(const Key& key) {
    const auto pos = find_position(key);
    if (!pos) {
        return nullptr;
    }

    // If find_position returns a slot, the key/value optional invariant says
    // values_[*pos] is engaged.
    return &(*values_[*pos]);
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
const Value* HopscotchMap<Key, Value, Hasher, Neighborhood>::find(const Key& key) const {
    const auto pos = find_position(key);
    if (!pos) {
        return nullptr;
    }

    return &(*values_[*pos]);
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
bool HopscotchMap<Key, Value, Hasher, Neighborhood>::contains(const Key& key) const {
    return find_position(key).has_value();
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
bool HopscotchMap<Key, Value, Hasher, Neighborhood>::insert_or_assign(
    const Key& key,
    const Value& value
) {
    const std::size_t hash = hash_key(key);

    // If the key already exists, we replace only the mapped value.
    // reset() + emplace() avoids requiring Value to be copy-assignable.
    if (const auto pos = find_position_with_hash(key, hash)) {
        values_[*pos].reset();
        values_[*pos].emplace(value);
        return false;
    }

    // Keep load factor below the configured threshold before trying insertion.
    ensure_capacity_for_one_more();

    // A Hopscotch insertion can fail even if the table is not full: a free slot
    // may exist but not be movable into the new key's neighborhood. The simple
    // dynamic strategy is to grow and retry.
    for (int attempts = 0; attempts < max_insert_resize_attempts_; ++attempts) {
        if (insert_no_resize(key, value, hash)) {
            ++size_;
            return true;
        }

        resize(capacity_ * 2);
    }

    // This protects against pathological hashers, for example a hasher that
    // maps every distinct key to the same hash/home bucket. Since one home
    // bucket has only Neighborhood bitmap bits, resizing cannot fix unlimited
    // collisions with an identical home.
    throw std::runtime_error(
        "Hopscotch insertion failed after repeated resizing; "
        "hash function may be too clustered."
    );
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
bool HopscotchMap<Key, Value, Hasher, Neighborhood>::erase(const Key& key) {
    const std::size_t hash = hash_key(key);
    const std::size_t home = home_index(hash);

    HopWord hop = hop_info_[home];

    while (hop != 0) {
        const std::size_t offset = std::countr_zero(hop);
        const std::size_t pos = wrap(home + offset);

        if (is_occupied(pos) && hashes_[pos] == hash && *keys_[pos] == key) {
            // The entry belongs to home and lives at offset. Removing it only
            // requires clearing that ownership bit and resetting the payload.
            // No tombstone is needed because future lookups follow hop bits.
            clear_hop_bit(home, offset);
            clear_slot(pos);
            --size_;
            return true;
        }

        // Clear the lowest set bit and continue to the next candidate offset.
        hop &= hop - 1;
    }

    return false;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::clear() {
    for (std::size_t i = 0; i < capacity_; ++i) {
        hop_info_[i] = 0;
        clear_slot(i);
    }

    size_ = 0;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::reserve(std::size_t requested_size) {
    // Reserve in terms of entries, not buckets. We choose enough buckets so that
    // requested_size entries should remain below max_load_factor_.
    std::size_t needed_capacity = static_cast<std::size_t>(
        static_cast<double>(requested_size) / max_load_factor_
    ) + 1;

    needed_capacity = normalize_capacity(needed_capacity);

    if (needed_capacity > capacity_) {
        resize(needed_capacity);
    }
}

// -----------------------------------------------------------------------------
// Capacity / indexing helpers
// -----------------------------------------------------------------------------

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
std::size_t HopscotchMap<Key, Value, Hasher, Neighborhood>::normalize_capacity(std::size_t n) {
    // Neighborhood distinct offsets must fit into the table. If capacity were
    // smaller than Neighborhood, the bitmap offsets would no longer represent
    // distinct physical slots.
    if (n < Neighborhood) {
        n = Neighborhood;
    }

    // std::bit_ceil gives the next power of two. Power-of-two capacity allows:
    //
    //     wrap(i) = i & (capacity - 1)
    //
    // instead of modulo division.
    return std::bit_ceil(n);
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::allocate_empty(std::size_t new_capacity) {
    capacity_ = normalize_capacity(new_capacity);
    mask_ = capacity_ - 1;
    size_ = 0;

    // Metadata columns.
    hop_info_.assign(capacity_, 0);
    hashes_.assign(capacity_, 0);

    // Payload columns. resize(capacity_) creates capacity optional placeholders,
    // but does not create live Key/Value objects inside empty optionals.
    keys_.clear();
    values_.clear();
    keys_.resize(capacity_);
    values_.resize(capacity_);
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
std::size_t HopscotchMap<Key, Value, Hasher, Neighborhood>::wrap(std::size_t index) const noexcept {
    return index & mask_;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
std::size_t HopscotchMap<Key, Value, Hasher, Neighborhood>::distance(
    std::size_t from,
    std::size_t to
) const noexcept {
    // Circular forward distance. With power-of-two capacity, unsigned wraparound
    // plus mask gives the modulo distance.
    //
    // Example with capacity 16:
    //
    //     distance(13, 2) = (2 - 13) & 15 = 5
    //
    // representing 13 -> 14 -> 15 -> 0 -> 1 -> 2.
    return (to - from) & mask_;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
std::size_t HopscotchMap<Key, Value, Hasher, Neighborhood>::home_index(std::size_t hash) const noexcept {
    return hash & mask_;
}

// -----------------------------------------------------------------------------
// Hashing
// -----------------------------------------------------------------------------

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
std::uint64_t HopscotchMap<Key, Value, Hasher, Neighborhood>::mix64(std::uint64_t x) noexcept {
    // Finalizer-style mixing. Since power-of-two indexing uses low bits,
    // we mix the user hash before masking. This helps when std::hash<Key>
    // has weak low bits, especially for integer-like keys.
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
std::size_t HopscotchMap<Key, Value, Hasher, Neighborhood>::hash_key(const Key& key) const {
    const std::uint64_t raw = static_cast<std::uint64_t>(hasher_(key));
    return static_cast<std::size_t>(mix64(raw));
}

// -----------------------------------------------------------------------------
// Hop bitmap helpers
// -----------------------------------------------------------------------------

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::set_hop_bit(
    std::size_t home,
    std::size_t offset
) {
    hop_info_[home] |= HopWord{1} << offset;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::clear_hop_bit(
    std::size_t home,
    std::size_t offset
) {
    hop_info_[home] &= ~(HopWord{1} << offset);
}

// -----------------------------------------------------------------------------
// Occupancy / slot helpers
// -----------------------------------------------------------------------------

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
bool HopscotchMap<Key, Value, Hasher, Neighborhood>::is_occupied(std::size_t pos) const {
    return keys_[pos].has_value();
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::clear_slot(std::size_t pos) {
    keys_[pos].reset();
    values_[pos].reset();
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::place_slot(
    std::size_t pos,
    const Key& key,
    const Value& value,
    std::size_t hash
) {
    // Precondition: pos is empty.
    //
    // The stored hash is used as a fast rejection filter during lookup:
    // compare hash first, then compare key only if hashes match.
    hashes_[pos] = hash;
    keys_[pos].emplace(key);
    values_[pos].emplace(value);
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::move_payload_slot(
    std::size_t from,
    std::size_t to
) {
    // Move the physical entry from one slot to another.
    //
    // Before:
    //
    //     from: [hash, key, value]
    //     to:   empty
    //
    // After:
    //
    //     from: empty
    //     to:   [hash, key, value]
    //
    // Important: hop_info_ is not moved here. The caller updates the relevant
    // home bucket's bitmap after this physical move.

    hashes_[to] = hashes_[from];

    keys_[to].reset();
    values_[to].reset();

    keys_[to].emplace(std::move(*keys_[from]));
    values_[to].emplace(std::move(*values_[from]));

    clear_slot(from);
}

// -----------------------------------------------------------------------------
// Lookup helpers
// -----------------------------------------------------------------------------

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
std::optional<std::size_t> HopscotchMap<Key, Value, Hasher, Neighborhood>::find_position(
    const Key& key
) const {
    const std::size_t hash = hash_key(key);
    return find_position_with_hash(key, hash);
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
std::optional<std::size_t> HopscotchMap<Key, Value, Hasher, Neighborhood>::find_position_with_hash(
    const Key& key,
    std::size_t hash
) const {
    const std::size_t home = home_index(hash);
    HopWord hop = hop_info_[home];

    // Example:
    //
    //     hop = 0b00101001
    //
    // Iteration order using countr_zero:
    //
    //     offset 0, then 3, then 5
    //
    // We only inspect candidate slots owned by this home bucket.
    while (hop != 0) {
        const std::size_t offset = std::countr_zero(hop);
        const std::size_t pos = wrap(home + offset);

        if (is_occupied(pos) && hashes_[pos] == hash && *keys_[pos] == key) {
            return pos;
        }

        // Clear the lowest set bit and continue to the next candidate.
        hop &= hop - 1;
    }

    return std::nullopt;
}

// -----------------------------------------------------------------------------
// Insertion / resize helpers
// -----------------------------------------------------------------------------

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::ensure_capacity_for_one_more() {
    if (static_cast<double>(size_ + 1) >
        static_cast<double>(capacity_) * max_load_factor_) {
        resize(capacity_ * 2);
    }
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
std::optional<std::size_t> HopscotchMap<Key, Value, Hasher, Neighborhood>::find_free_slot(
    std::size_t home
) const {
    // Find the first empty slot in circular order starting from home.
    //
    // The free slot might be outside the desired neighborhood. In that case,
    // insert_no_resize() tries to pull it backwards by relocating other entries.
    for (std::size_t d = 0; d < capacity_; ++d) {
        const std::size_t pos = wrap(home + d);

        if (!is_occupied(pos)) {
            return pos;
        }
    }

    return std::nullopt;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
bool HopscotchMap<Key, Value, Hasher, Neighborhood>::insert_no_resize(
    const Key& key,
    const Value& value,
    std::size_t hash
) {
    const std::size_t home = home_index(hash);

    auto free_opt = find_free_slot(home);
    if (!free_opt) {
        return false;
    }

    std::size_t free_pos = *free_opt;

    // If the first free slot is too far away, repeatedly move some existing
    // entry into free_pos. Each successful move makes that moved entry's old
    // position the new free position.
    //
    // Goal:
    //
    //     distance(home, free_pos) < Neighborhood
    //
    // so the new key can legally be placed at free_pos.
    while (distance(home, free_pos) >= Neighborhood) {
        if (!move_free_slot_closer(free_pos)) {
            return false;
        }
    }

    const std::size_t offset = distance(home, free_pos);

    place_slot(free_pos, key, value, hash);
    set_hop_bit(home, offset);

    return true;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
bool HopscotchMap<Key, Value, Hasher, Neighborhood>::move_free_slot_closer(std::size_t& free_pos) {
    // This is the core Hopscotch relocation step.
    //
    // We have a free slot F that is too far from the original insertion home.
    // We search possible home buckets before F:
    //
    //     F - (Neighborhood - 1), ..., F - 2, F - 1
    //
    // For a candidate home C, F is at offset d from C:
    //
    //     C + d == F    (circularly)
    //
    // If C currently owns an entry at some old_offset < d, then that entry can
    // be moved forward to F and still remain inside C's neighborhood.
    //
    // Before:
    //
    //     C                  old_pos                   F
    //     |                     |                       |
    //     v                     v                       v
    //     +----+----+----+----+-------+----+----+----+------+
    //     | C  |    |    |    | entry |    |    |    | free |
    //     +----+----+----+----+-------+----+----+----+------+
    //                          ^                       ^
    //                    old_offset                    d
    //
    // After:
    //
    //     C                  old_pos                   F
    //     |                     |                       |
    //     v                     v                       v
    //     +----+----+----+----+------+----+----+----+-------+
    //     | C  |    |    |    | free |    |    |    | entry |
    //     +----+----+----+----+------+----+----+----+-------+
    //
    // Now old_pos becomes the new free slot, closer to the original insertion
    // target. Repeating this can pull the free slot into range.

    for (std::size_t d = Neighborhood - 1; d > 0; --d) {
        const std::size_t candidate_home = wrap(free_pos - d);
        const HopWord hop = hop_info_[candidate_home];

        // Entries at offsets smaller than d can move to free_pos because
        // free_pos is exactly offset d from candidate_home and d < Neighborhood.
        //
        // Mask example with d = 5:
        //
        //     ((1 << d) - 1) = 0b00011111
        //
        // This keeps candidate offsets 0..4 and discards offsets >= 5.
        const HopWord movable_mask = hop & ((HopWord{1} << d) - 1);

        if (movable_mask == 0) {
            continue;
        }

        const std::size_t old_offset = std::countr_zero(movable_mask);
        const std::size_t old_pos = wrap(candidate_home + old_offset);

        move_payload_slot(old_pos, free_pos);

        // The moved entry is still owned by candidate_home, but its offset has
        // changed from old_offset to d.
        clear_hop_bit(candidate_home, old_offset);
        set_hop_bit(candidate_home, d);

        // The slot we just emptied is now the free slot. This is how the free
        // slot moves backward toward the original insertion home.
        free_pos = old_pos;
        return true;
    }

    return false;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::resize(std::size_t requested_capacity) {
    requested_capacity = normalize_capacity(requested_capacity);

    // Move old storage out. Old hop bits are intentionally discarded because
    // home buckets change when capacity changes.
    auto old_hashes = std::move(hashes_);
    auto old_keys = std::move(keys_);
    auto old_values = std::move(values_);

    const std::size_t old_capacity = capacity_;
    std::size_t new_capacity = requested_capacity;

    while (true) {
        allocate_empty(new_capacity);

        bool success = true;

        for (std::size_t i = 0; i < old_capacity; ++i) {
            if (!old_keys[i].has_value()) {
                continue;
            }

            // Reinsert with the stored mixed hash. This avoids recomputing the
            // user hash function during resize. The home bucket is recomputed
            // from the same mixed hash using the new mask.
            if (!insert_no_resize(*old_keys[i], *old_values[i], old_hashes[i])) {
                success = false;
                break;
            }

            ++size_;
        }

        if (success) {
            return;
        }

        // Very unlikely with normal hashes, but possible in principle. If the
        // new table still cannot satisfy all neighborhood invariants, grow again.
        new_capacity *= 2;
    }
}
