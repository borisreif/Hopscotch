#pragma once

#include <utility>

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
HopscotchMap<Key, Value, Hasher, Neighborhood>::HopscotchMap(std::size_t capacity_hint)
{
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
    auto pos = find_position(key);
    if (!pos) {
        return nullptr;
    }

    return &(*values_[*pos]);
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
const Value* HopscotchMap<Key, Value, Hasher, Neighborhood>::find(const Key& key) const {
    auto pos = find_position(key);
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

    if (auto pos = find_position_with_hash(key, hash)) {
        // Replace via reset + emplace so Value does not need to be copy-assignable.
        values_[*pos].reset();
        values_[*pos].emplace(value);
        return false;
    }

    ensure_capacity_for_one_more();

    for (int attempts = 0; attempts < max_insert_resize_attempts_; ++attempts) {
        if (insert_no_resize(key, value, hash)) {
            ++size_;
            return true;
        }

        resize(capacity_ * 2);
    }

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
            clear_hop_bit(home, offset);
            clear_slot(pos);
            --size_;
            return true;
        }

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
    if (n < Neighborhood) {
        n = Neighborhood;
    }

    return std::bit_ceil(n);
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::allocate_empty(std::size_t new_capacity) {
    capacity_ = normalize_capacity(new_capacity);
    mask_ = capacity_ - 1;
    size_ = 0;

    hop_info_.assign(capacity_, 0);
    hashes_.assign(capacity_, 0);

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
    hashes_[pos] = hash;
    keys_[pos].emplace(key);
    values_[pos].emplace(value);
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::move_payload_slot(
    std::size_t from,
    std::size_t to
) {
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

    while (hop != 0) {
        const std::size_t offset = std::countr_zero(hop);
        const std::size_t pos = wrap(home + offset);

        if (is_occupied(pos) && hashes_[pos] == hash && *keys_[pos] == key) {
            return pos;
        }

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
    for (std::size_t d = Neighborhood - 1; d > 0; --d) {
        const std::size_t candidate_home = wrap(free_pos - d);
        const HopWord hop = hop_info_[candidate_home];

        // Entries at offsets smaller than d can move to free_pos, because
        // free_pos is exactly offset d from candidate_home and d < Neighborhood.
        const HopWord movable_mask = hop & ((HopWord{1} << d) - 1);

        if (movable_mask == 0) {
            continue;
        }

        const std::size_t old_offset = std::countr_zero(movable_mask);
        const std::size_t old_pos = wrap(candidate_home + old_offset);

        move_payload_slot(old_pos, free_pos);

        clear_hop_bit(candidate_home, old_offset);
        set_hop_bit(candidate_home, d);

        free_pos = old_pos;
        return true;
    }

    return false;
}

template <typename Key, typename Value, typename Hasher, std::size_t Neighborhood>
void HopscotchMap<Key, Value, Hasher, Neighborhood>::resize(std::size_t requested_capacity) {
    requested_capacity = normalize_capacity(requested_capacity);

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

            if (!insert_no_resize(*old_keys[i], *old_values[i], old_hashes[i])) {
                success = false;
                break;
            }

            ++size_;
        }

        if (success) {
            return;
        }

        new_capacity *= 2;
    }
}
