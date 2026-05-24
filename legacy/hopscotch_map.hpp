
#pragma once

/**
 * @file hopscotch_map.hpp
 * @brief A circular, power-of-two, structure-of-arrays Hopscotch hash map.
 *
 * This file implements a small Hopscotch hash table with the
 * following design choices:
 *
 *   - Circular table indexing.
 *   - Capacity is always a power of two.
 *   - Structure-of-arrays storage layout.
 *   - Metadata is separated from payload.
 *   - The table resizes by doubling and reinserting all live entries.
 *
 * This implementation is intentionally written for readability and learning.
 * It is not intended to be a drop-in replacement for std::unordered_map.
 *
 * ---------------------------------------------------------------------------
 * 1. What is Hopscotch hashing?
 * ---------------------------------------------------------------------------
 *
 * In a normal open-addressed hash table, a key has a home bucket:
 *
 *     home = hash(key) % capacity
 *
 * With power-of-two capacity, this becomes:
 *
 *     home = hash(key) & (capacity - 1)
 *
 * In Hopscotch hashing, an entry does not have to live exactly at its home
 * bucket. Instead, it must live within a small fixed-size neighborhood of its
 * home bucket.
 *
 * Example with Neighborhood = 8:
 *
 *     home bucket h
 *          |
 *          v
 *     +----+----+----+----+----+----+----+----+
 *     | h  |h+1 |h+2 |h+3 |h+4 |h+5 |h+6 |h+7 |
 *     +----+----+----+----+----+----+----+----+
 *
 * A key whose home bucket is h may be stored in any one of those slots.
 *
 * The main invariant is:
 *
 *     distance(home, actual_position) < Neighborhood
 *
 * In this implementation, the table is circular, so distance is measured modulo
 * the capacity.
 *
 * ---------------------------------------------------------------------------
 * 2. What does the hop bitmap mean?
 * ---------------------------------------------------------------------------
 *
 * Each bucket has a small bitmap called hop_info. The bitmap belongs to the
 * home bucket, not to the entry physically stored in that bucket.
 *
 * If bit k of hop_info[h] is set, then slot h + k contains an entry whose home
 * bucket is h.
 *
 * Example with Neighborhood = 8:
 *
 *     hop_info[h] = 0b00101001
 *
 * Bits set: 0, 3, 5
 *
 *     h owns entries at offsets 0, 3, and 5:
 *
 *     offset:  0    1    2    3    4    5    6    7
 *              |              |         |
 *              v              v         v
 *     +--------+----+----+--------+----+--------+----+----+
 *     | entry  |    |    | entry  |    | entry  |    |    |
 *     +--------+----+----+--------+----+--------+----+----+
 *       h+0              h+3            h+5
 *
 * Lookup only checks the positions indicated by the bitmap. This is what makes
 * Hopscotch lookup efficient: it only examines a tiny neighborhood.
 *
 * ---------------------------------------------------------------------------
 * 3. Circular indexing
 * ---------------------------------------------------------------------------
 *
 * The table capacity is always a power of two, so wrapping is cheap:
 *
 *     wrap(i) = i & (capacity - 1)
 *
 * Example with capacity = 16:
 *
 *     wrap(16) = 0
 *     wrap(17) = 1
 *     wrap(18) = 2
 *
 * A neighborhood near the physical end of the array wraps back to the front:
 *
 *     capacity = 16, Neighborhood = 8, home = 13
 *
 *     logical neighborhood:
 *
 *     13, 14, 15, 0, 1, 2, 3, 4
 *
 *     Physical memory is still linear, but the algorithm treats it as circular.
 *
 * ---------------------------------------------------------------------------
 * 4. Structure-of-arrays architecture
 * ---------------------------------------------------------------------------
 *
 * A classic object-oriented bucket layout would use an array of structs:
 *
 *     struct Bucket {
 *         HopWord hop;
 *         bool occupied;
 *         size_t hash;
 *         Key key;
 *         Value value;
 *     };
 *
 *     std::vector<Bucket> buckets;
 *
 * This implementation instead uses a structure-of-arrays layout:
 *
 *     hop_info_[i]   -- hop bitmap for home bucket i
 *     hashes_[i]     -- stored hash of the entry physically at slot i
 *     occupied_[i]   -- whether slot i physically contains an entry
 *     keys_[i]       -- key physically stored at slot i
 *     values_[i]     -- value physically stored at slot i
 *
 * ASCII view:
 *
 *     slot index:    0      1      2      3      4      ...
 *
 *     hop_info_:   [ h0 ] [ h1 ] [ h2 ] [ h3 ] [ h4 ]
 *     hashes_:     [ x0 ] [ x1 ] [ x2 ] [ x3 ] [ x4 ]
 *     occupied_:   [  1 ] [  0 ] [  1 ] [  1 ] [  0 ]
 *     keys_:       [ k0 ] [ -- ] [ k2 ] [ k3 ] [ -- ]
 *     values_:     [ v0 ] [ -- ] [ v2 ] [ v3 ] [ -- ]
 *
 * This keeps metadata and payload separated. Lookup normally touches metadata
 * first and only touches keys/values when a hash match occurs.
 *
 * Important note:
 *
 *     hop_info_[i] belongs to home bucket i.
 *     It is not part of the payload physically stored at slot i.
 *
 * Therefore, when we move an entry from one physical slot to another, we move
 * hash/key/value/occupied state, but we do not move hop_info_. Instead, we
 * update the relevant home bucket's bitmap.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

/**
 * @class HopscotchMap
 * @brief A small educational Hopscotch hash map using circular indexing and SoA storage.
 *
 * @tparam Key Key type.
 * @tparam Value Value type.
 * @tparam Hasher Hash functor type. Defaults to std::hash<Key>.
 * @tparam Neighborhood Number of nearby slots that belong to a home bucket's neighborhood.
 *
 * @details
 * This implementation stores all arrays at length capacity. Therefore, Key and
 * Value must be default-constructible. Empty slots still contain default-created
 * Key and Value objects, but occupied_[i] determines whether the slot is live.
 *
 * This is the simpler structure-of-arrays design discussed earlier. It avoids
 * the extra complexity of dense payload storage with slot_to_entry and
 * entry_to_slot mappings.
 */
template <
    typename Key,
    typename Value,
    typename Hasher = std::hash<Key>,
    std::size_t Neighborhood = 32
>
class HopscotchMap {
private:
    /**
     * @brief Integer type used for the hop bitmap.
     *
     * With std::uint32_t, Neighborhood may be at most 32.
     */
    using HopWord = std::uint32_t;

    static_assert(Neighborhood > 0,
                  "Neighborhood must be positive.");

    static_assert(Neighborhood <= sizeof(HopWord) * 8,
                  "Neighborhood must fit inside HopWord.");

    static_assert(std::is_default_constructible_v<Key>,
                  "This simple SoA version requires default-constructible Key.");

    static_assert(std::is_default_constructible_v<Value>,
                  "This simple SoA version requires default-constructible Value.");

    static_assert(std::is_move_assignable_v<Key>,
                  "This implementation moves keys during hopscotch relocation.");

    static_assert(std::is_move_assignable_v<Value>,
                  "This implementation moves values during hopscotch relocation.");

public:
    /**
     * @brief Construct a Hopscotch map.
     *
     * @param initial_capacity Desired initial capacity. It is rounded up to a
     * power of two and to at least Neighborhood * 2.
     *
     * @details
     * Capacity is normalized so that circular masking works:
     *
     *     index & (capacity - 1)
     */
    explicit HopscotchMap(std::size_t initial_capacity = 64)
    {
        initial_capacity = normalize_capacity(initial_capacity);
        allocate_empty(initial_capacity);
    }

    /**
     * @brief Return the number of live key/value pairs.
     */
    std::size_t size() const noexcept {
        return size_;
    }

    /**
     * @brief Return the number of physical slots in the table.
     */
    std::size_t capacity() const noexcept {
        return capacity_;
    }

    /**
     * @brief Return true if the table contains no live entries.
     */
    bool empty() const noexcept {
        return size_ == 0;
    }

    /**
     * @brief Return the current load factor.
     */
    double load_factor() const noexcept {
        if (capacity_ == 0) {
            return 0.0;
        }

        return static_cast<double>(size_) / static_cast<double>(capacity_);
    }

    /**
     * @brief Set the maximum load factor.
     *
     * @param lf Desired load factor in the open interval (0, 1).
     *
     * @throws std::invalid_argument if lf is not in (0, 1).
     */
    void max_load_factor(double lf) {
        if (lf <= 0.0 || lf >= 1.0) {
            throw std::invalid_argument("max_load_factor must be in range (0, 1).");
        }

        max_load_factor_ = lf;
    }

    /**
     * @brief Return the configured maximum load factor.
     */
    double max_load_factor() const noexcept {
        return max_load_factor_;
    }

    /**
     * @brief Find a mutable value by key.
     *
     * @param key Key to search for.
     * @return Pointer to value if found, otherwise nullptr.
     *
     * @details
     * Lookup process:
     *
     *     1. Compute hash.
     *     2. Compute home bucket.
     *     3. Read home bucket's hop bitmap.
     *     4. Visit only candidate slots indicated by the bitmap.
     *     5. Compare stored hash first, then key.
     */
    Value* find(const Key& key) {
        auto pos = find_position(key);
        if (!pos) {
            return nullptr;
        }

        return &values_[*pos];
    }

    /**
     * @brief Find a const value by key.
     *
     * @param key Key to search for.
     * @return Pointer to value if found, otherwise nullptr.
     */
    const Value* find(const Key& key) const {
        auto pos = find_position(key);
        if (!pos) {
            return nullptr;
        }

        return &values_[*pos];
    }

    /**
     * @brief Return true if the map contains key.
     */
    bool contains(const Key& key) const {
        return find_position(key).has_value();
    }

    /**
     * @brief Insert a new key/value pair or assign an existing value.
     *
     * @param key Key to insert or update.
     * @param value Value to insert or assign.
     * @return true if a new entry was inserted; false if an existing value was assigned.
     *
     * @details
     * The insertion algorithm is:
     *
     *     insert_or_assign(key, value):
     *         if key already exists:
     *             assign value
     *             return false
     *
     *         if load factor would become too high:
     *             resize table
     *
     *         try normal Hopscotch insertion
     *
     *         if insertion fails:
     *             resize and retry
     *
     * Normal Hopscotch insertion may fail even before the table is completely
     * full. Failure means we could not move a free slot close enough to the
     * key's home bucket while preserving all neighborhood invariants.
     */
    bool insert_or_assign(const Key& key, const Value& value) {
        std::size_t hash = hash_key(key);

        if (auto pos = find_position_with_hash(key, hash)) {
            values_[*pos] = value;
            return false;
        }

        ensure_capacity_for_one_more();

        while (!insert_no_resize(key, value, hash)) {
            resize(capacity_ * 2);
        }

        ++size_;
        return true;
    }

    /**
     * @brief Erase key from the map.
     *
     * @param key Key to erase.
     * @return true if an entry was erased; false if key was not found.
     *
     * @details
     * Deletion is straightforward in Hopscotch hashing. We only need to clear:
     *
     *     - the physical occupied flag, and
     *     - the corresponding bit in the home bucket's hop bitmap.
     *
     * No tombstones are needed because lookup only follows hop bitmap bits.
     */
    bool erase(const Key& key) {
        std::size_t hash = hash_key(key);
        std::size_t home = home_index(hash);

        HopWord hop = hop_info_[home];

        while (hop != 0) {
            std::size_t offset = std::countr_zero(hop);
            std::size_t pos = wrap(home + offset);

            if (occupied_[pos] && hashes_[pos] == hash && keys_[pos] == key) {
                clear_hop_bit(home, offset);
                occupied_[pos] = 0;
                --size_;
                return true;
            }

            hop &= hop - 1;
        }

        return false;
    }

    /**
     * @brief Remove all entries while keeping allocated capacity.
     *
     * @details
     * This clears metadata only. The Key and Value objects in keys_ and values_
     * remain constructed because this implementation uses capacity-sized
     * vectors for payload storage.
     */
    void clear() {
        for (std::size_t i = 0; i < capacity_; ++i) {
            hop_info_[i] = 0;
            occupied_[i] = 0;
        }

        size_ = 0;
    }

    /**
     * @brief Reserve enough capacity for at least requested_size entries.
     *
     * @param requested_size Number of entries the caller intends to store.
     */
    void reserve(std::size_t requested_size) {
        std::size_t needed_capacity = static_cast<std::size_t>(
            static_cast<double>(requested_size) / max_load_factor_
        ) + 1;

        needed_capacity = normalize_capacity(needed_capacity);

        if (needed_capacity > capacity_) {
            resize(needed_capacity);
        }
    }

private:
    // ---------------------------------------------------------------------
    // Table state
    // ---------------------------------------------------------------------

    /** @brief Number of live entries. */
    std::size_t size_ = 0;

    /** @brief Number of physical slots. Always a power of two. */
    std::size_t capacity_ = 0;

    /** @brief Bitmask used for circular indexing. Equal to capacity_ - 1. */
    std::size_t mask_ = 0;

    /** @brief Maximum allowed load factor before resizing. */
    double max_load_factor_ = 0.85;

    /** @brief User-provided or default hash functor. */
    Hasher hasher_{};

    // ---------------------------------------------------------------------
    // Metadata arrays: one element per physical slot
    // ---------------------------------------------------------------------

    /**
     * @brief Hop bitmap for each home bucket.
     *
     * hop_info_[h] tells us which slots in h's neighborhood contain entries
     * whose home bucket is h.
     */
    std::vector<HopWord> hop_info_;

    /**
     * @brief Stored mixed hash for the entry physically located at each slot.
     *
     * hashes_[i] is meaningful only when occupied_[i] is true.
     */
    std::vector<std::size_t> hashes_;

    /**
     * @brief Occupancy byte for each physical slot.
     *
     * occupied_[i] is 1 if slot i contains a live entry and 0 otherwise.
     */
    std::vector<std::uint8_t> occupied_;

    // ---------------------------------------------------------------------
    // Payload arrays: also one element per physical slot in this simple version
    // ---------------------------------------------------------------------

    /**
     * @brief Key physically stored at each slot.
     *
     * keys_[i] is meaningful only when occupied_[i] is true.
     */
    std::vector<Key> keys_;

    /**
     * @brief Value physically stored at each slot.
     *
     * values_[i] is meaningful only when occupied_[i] is true.
     */
    std::vector<Value> values_;

private:
    // ---------------------------------------------------------------------
    // Capacity / indexing helpers
    // ---------------------------------------------------------------------

    /**
     * @brief Normalize a requested capacity.
     *
     * @param n Requested capacity.
     * @return Power-of-two capacity at least Neighborhood * 2.
     *
     * @details
     * We require capacity to be a power of two so that wrapping can be done with
     * a cheap bitmask operation.
     */
    static std::size_t normalize_capacity(std::size_t n) {
        if (n < Neighborhood * 2) {
            n = Neighborhood * 2;
        }

        return std::bit_ceil(n);
    }

    /**
     * @brief Allocate an empty table with new_capacity slots.
     *
     * @param new_capacity Desired capacity. It is normalized internally.
     *
     * @details
     * This function destroys the current table contents. It is used by the
     * constructor and by resize after old storage has been moved out.
     */
    void allocate_empty(std::size_t new_capacity) {
        capacity_ = normalize_capacity(new_capacity);
        mask_ = capacity_ - 1;
        size_ = 0;

        hop_info_.assign(capacity_, 0);
        hashes_.assign(capacity_, 0);
        occupied_.assign(capacity_, 0);

        keys_.clear();
        values_.clear();

        keys_.resize(capacity_);
        values_.resize(capacity_);
    }

    /**
     * @brief Wrap an arbitrary index into the circular table.
     *
     * @param index Possibly out-of-range index.
     * @return index modulo capacity_.
     *
     * @details
     * Because capacity_ is a power of two, this is equivalent to:
     *
     *     index % capacity_
     *
     * but generally cheaper.
     */
    std::size_t wrap(std::size_t index) const noexcept {
        return index & mask_;
    }

    /**
     * @brief Compute circular forward distance from one slot to another.
     *
     * @param from Starting slot.
     * @param to Ending slot.
     * @return Number of forward circular steps from from to to.
     *
     * Example with capacity = 16:
     *
     *     distance(13, 2) == 5
     *
     * because 13 -> 14 -> 15 -> 0 -> 1 -> 2.
     */
    std::size_t distance(std::size_t from, std::size_t to) const noexcept {
        return (to - from) & mask_;
    }

    /**
     * @brief Compute home bucket from a mixed hash value.
     */
    std::size_t home_index(std::size_t hash) const noexcept {
        return hash & mask_;
    }

    // ---------------------------------------------------------------------
    // Hashing
    // ---------------------------------------------------------------------

    /**
     * @brief 64-bit finalizer-style hash mixer.
     *
     * @param x Raw hash value.
     * @return Mixed hash value.
     *
     * @details
     * Power-of-two tables use the low bits of the hash. Many hash functions,
     * especially identity hashes for integers, may have weak low bits. This
     * mixer helps distribute those bits before applying the table mask.
     */
    static std::uint64_t mix64(std::uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    /**
     * @brief Hash a key and apply the internal mixer.
     *
     * @param key Key to hash.
     * @return Mixed hash suitable for power-of-two indexing.
     */
    std::size_t hash_key(const Key& key) const {
        std::uint64_t raw = static_cast<std::uint64_t>(hasher_(key));
        return static_cast<std::size_t>(mix64(raw));
    }

    // ---------------------------------------------------------------------
    // Hop bitmap helpers
    // ---------------------------------------------------------------------

    /**
     * @brief Set a bit in a home bucket's hop bitmap.
     *
     * @param home Home bucket.
     * @param offset Offset from home bucket to physical slot.
     */
    void set_hop_bit(std::size_t home, std::size_t offset) {
        hop_info_[home] |= HopWord{1} << offset;
    }

    /**
     * @brief Clear a bit in a home bucket's hop bitmap.
     *
     * @param home Home bucket.
     * @param offset Offset from home bucket to physical slot.
     */
    void clear_hop_bit(std::size_t home, std::size_t offset) {
        hop_info_[home] &= ~(HopWord{1} << offset);
    }

    // ---------------------------------------------------------------------
    // Lookup helpers
    // ---------------------------------------------------------------------

    /**
     * @brief Find the physical slot containing key.
     *
     * @param key Key to search for.
     * @return Slot position if found; std::nullopt otherwise.
     */
    std::optional<std::size_t> find_position(const Key& key) const {
        std::size_t hash = hash_key(key);
        return find_position_with_hash(key, hash);
    }

    /**
     * @brief Find the physical slot containing key using a precomputed hash.
     *
     * @param key Key to search for.
     * @param hash Mixed hash of key.
     * @return Slot position if found; std::nullopt otherwise.
     *
     * @details
     * The hop bitmap ensures that we only inspect slots that could actually
     * contain an entry whose home is the computed home bucket.
     */
    std::optional<std::size_t> find_position_with_hash(
        const Key& key,
        std::size_t hash
    ) const {
        std::size_t home = home_index(hash);
        HopWord hop = hop_info_[home];

        while (hop != 0) {
            std::size_t offset = std::countr_zero(hop);
            std::size_t pos = wrap(home + offset);

            if (occupied_[pos] && hashes_[pos] == hash && keys_[pos] == key) {
                return pos;
            }

            // Clear the lowest set bit and continue.
            hop &= hop - 1;
        }

        return std::nullopt;
    }

    // ---------------------------------------------------------------------
    // Slot operations
    // ---------------------------------------------------------------------

    /**
     * @brief Place a new live entry into an empty physical slot.
     *
     * @param pos Physical slot to write.
     * @param key Key to store.
     * @param value Value to store.
     * @param hash Mixed hash of key.
     *
     * @pre occupied_[pos] must be false.
     */
    void place_slot(
        std::size_t pos,
        const Key& key,
        const Value& value,
        std::size_t hash
    ) {
        hashes_[pos] = hash;
        keys_[pos] = key;
        values_[pos] = value;
        occupied_[pos] = 1;
    }

    /**
     * @brief Move the physical payload state from one slot to another.
     *
     * @param from Occupied source slot.
     * @param to Empty destination slot.
     *
     * @details
     * This moves the entry physically stored at from into to.
     *
     * It moves:
     *
     *     - hashes_[from]
     *     - keys_[from]
     *     - values_[from]
     *     - occupied_[from]
     *
     * But it does NOT move hop_info_.
     *
     * This is crucial:
     *
     *     hop_info_[i] belongs to home bucket i,
     *     not to the entry physically located at slot i.
     */
    void move_payload_slot(std::size_t from, std::size_t to) {
        hashes_[to] = hashes_[from];
        keys_[to] = std::move(keys_[from]);
        values_[to] = std::move(values_[from]);

        occupied_[to] = 1;
        occupied_[from] = 0;
    }

    // ---------------------------------------------------------------------
    // Insertion
    // ---------------------------------------------------------------------

    /**
     * @brief Resize if one more insertion would exceed the load factor.
     */
    void ensure_capacity_for_one_more() {
        if (static_cast<double>(size_ + 1) >
            static_cast<double>(capacity_) * max_load_factor_) {
            resize(capacity_ * 2);
        }
    }

    /**
     * @brief Find the next free slot starting at home and scanning forward.
     *
     * @param home Home bucket from which scanning begins.
     * @return Free slot if found; std::nullopt if the table is full.
     *
     * @details
     * The returned free slot may be outside the desired neighborhood. If it is
     * too far away, insertion will try to move other entries to pull the free
     * slot closer to the original home bucket.
     */
    std::optional<std::size_t> find_free_slot(std::size_t home) const {
        for (std::size_t d = 0; d < capacity_; ++d) {
            std::size_t pos = wrap(home + d);

            if (!occupied_[pos]) {
                return pos;
            }
        }

        return std::nullopt;
    }

    /**
     * @brief Try to insert without resizing.
     *
     * @param key Key to insert.
     * @param value Value to insert.
     * @param hash Mixed hash of key.
     * @return true if insertion succeeded; false if the table must grow.
     *
     * @details
     * High-level algorithm:
     *
     *     home = hash & mask
     *     free = first free slot at or after home
     *
     *     while free is outside home's neighborhood:
     *         try to move some earlier entry into free
     *         this makes that earlier entry's old slot the new free slot
     *
     *     place new entry into the now-close free slot
     *     set corresponding hop bit in home bucket
     *
     * ASCII example:
     *
     *     Need to insert key with home H.
     *     Free slot F is too far away.
     *
     *     H                              F
     *     |                              |
     *     v                              v
     *     [x][x][x][x][x][x][x][x][x][ ]
     *      <--- allowed neighborhood --->
     *
     *     We search backwards from F for an entry that can be moved into F,
     *     without violating that moved entry's own neighborhood invariant.
     */
    bool insert_no_resize(
        const Key& key,
        const Value& value,
        std::size_t hash
    ) {
        std::size_t home = home_index(hash);

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

        std::size_t offset = distance(home, free_pos);

        place_slot(free_pos, key, value, hash);
        set_hop_bit(home, offset);

        return true;
    }

    /**
     * @brief Try to move an occupied slot into free_pos to bring the free slot closer.
     *
     * @param free_pos Reference to the current free slot. Updated on success.
     * @return true if a move was made; false if no legal move exists.
     *
     * @details
     * This is the heart of Hopscotch insertion.
     *
     * Suppose free_pos is too far from the original home bucket H. We inspect
     * candidate home buckets before free_pos:
     *
     *     free_pos - (Neighborhood - 1), ..., free_pos - 1
     *
     * For a candidate home C, if C owns some entry at offset old_offset, and
     * old_offset < d where d is distance(C, free_pos), then that entry can be
     * moved into free_pos while still remaining inside C's neighborhood.
     *
     * Before:
     *
     *     C                 old_pos              free_pos
     *     |                    |                    |
     *     v                    v                    v
     *     [C][ ][ ][ ][ ][ ][ entry ][ ][ ][ ][ ][ empty ]
     *
     * After:
     *
     *     C                 old_pos              free_pos
     *     |                    |                    |
     *     v                    v                    v
     *     [C][ ][ ][ ][ ][ ][ empty ][ ][ ][ ][ ][ entry ]
     *
     * The free slot has moved from free_pos back to old_pos. Repeating this can
     * pull the free slot closer and closer to the original insertion home.
     */
    bool move_free_slot_closer(std::size_t& free_pos) {
        for (std::size_t d = Neighborhood - 1; d > 0; --d) {
            std::size_t candidate_home = wrap(free_pos - d);

            HopWord hop = hop_info_[candidate_home];

            // Entries at offsets less than d can be moved to free_pos.
            // Why? Because free_pos is exactly offset d from candidate_home.
            // If d < Neighborhood, free_pos is still inside candidate_home's
            // neighborhood.
            HopWord movable_mask = hop & ((HopWord{1} << d) - 1);

            if (movable_mask == 0) {
                continue;
            }

            std::size_t old_offset = std::countr_zero(movable_mask);
            std::size_t old_pos = wrap(candidate_home + old_offset);

            move_payload_slot(old_pos, free_pos);

            clear_hop_bit(candidate_home, old_offset);
            set_hop_bit(candidate_home, d);

            free_pos = old_pos;
            return true;
        }

        return false;
    }

    // ---------------------------------------------------------------------
    // Resize
    // ---------------------------------------------------------------------

    /**
     * @brief Resize the table and reinsert all live entries.
     *
     * @param requested_capacity Desired new capacity. It is normalized to a power of two.
     *
     * @details
     * We cannot simply copy old slots into the new table because changing the
     * capacity changes each key's home bucket:
     *
     *     old_home = hash & (old_capacity - 1)
     *     new_home = hash & (new_capacity - 1)
     *
     * Therefore, resize must rebuild the table by reinserting all entries and
     * regenerating all hop bitmaps.
     *
     * If reinsertion unexpectedly fails, the capacity is doubled again and the
     * process is retried.
     */
    void resize(std::size_t requested_capacity) {
        requested_capacity = normalize_capacity(requested_capacity);

        auto old_hop_info = std::move(hop_info_);
        auto old_hashes = std::move(hashes_);
        auto old_occupied = std::move(occupied_);
        auto old_keys = std::move(keys_);
        auto old_values = std::move(values_);

        (void)old_hop_info; // old hop bits are intentionally discarded.

        std::size_t old_capacity = capacity_;
        std::size_t new_capacity = requested_capacity;

        while (true) {
            allocate_empty(new_capacity);

            bool success = true;

            for (std::size_t i = 0; i < old_capacity; ++i) {
                if (!old_occupied[i]) {
                    continue;
                }

                if (!insert_no_resize(old_keys[i], old_values[i], old_hashes[i])) {
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
};
