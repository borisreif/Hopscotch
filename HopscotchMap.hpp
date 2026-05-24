#pragma once

/**
 * @file HopscotchMap.hpp
 * @brief Declaration of a circular, power-of-two, data-oriented Hopscotch map.
 *
 * This project is intentionally educational. The goal is not to imitate every
 * detail of std::unordered_map, but to make the mechanics of Hopscotch hashing
 * visible in a compact C++20 implementation.
 *
 * The implementation is split across files:
 *
 *     HopscotchMap.hpp  -- class declaration, public API, data members
 *     HopscotchMap.tpp  -- template member function definitions
 *     HopscotchMap.cpp  -- optional translation unit / explicit instantiations
 *     demo.cpp          -- small executable demonstration
 *
 * Important template note:
 *
 *     HopscotchMap is a class template. In normal C++, template definitions
 *     must be visible at the point of instantiation. Therefore this header
 *     includes HopscotchMap.tpp at the bottom.
 *
 * ---------------------------------------------------------------------------
 * 1. What is Hopscotch hashing?
 * ---------------------------------------------------------------------------
 *
 * @link https://en.wikipedia.org/wiki/Hopscotch_hashing
 * @link https://tessil.github.io/2016/08/29/hopscotch-hashing.html
 * @link https://programming.guide/hopscotch-hashing.html
 * @link https://codecapsule.com/2013/08/11/hopscotch-hashing/
 * @link https://github.com/Tessil/hopscotch-map
 * @link https://web.archive.org/web/20221220235913/http://mcg.cs.tau.ac.il/papers/disc2008-hopscotch.pdf
 * @link https://mural.maynoothuniversity.ie/id/eprint/15097/1/BP_lock-free.pdf
 *
 * A key is mapped to a home bucket:
 *
 *     home = hash(key) % capacity
 *
 * This implementation keeps capacity as a power of two, so the modulo becomes
 * a cheap mask operation:
 *
 *     home = hash(key) & (capacity - 1)
 *
 * In ordinary linear probing, an entry may drift far away from its home bucket.
 * Hopscotch hashing avoids that by enforcing a bounded neighborhood around the
 * home bucket.
 *
 * Example with Neighborhood = 8:
 *
 *     home bucket h
 *          |
 *          v
 *     +-----+-----+-----+-----+-----+-----+-----+-----+
 *     | h+0 | h+1 | h+2 | h+3 | h+4 | h+5 | h+6 | h+7 |
 *     +-----+-----+-----+-----+-----+-----+-----+-----+
 *
 * A key whose home is h may physically live in any one of those eight slots.
 *
 * The central invariant is:
 *
 *     distance(home, physical_position) < Neighborhood
 *
 * Because this table is circular, distance is measured modulo capacity.
 *
 * ---------------------------------------------------------------------------
 * 2. The hop bitmap
 * ---------------------------------------------------------------------------
 *
 * Each home bucket stores a small bitmap called hop_info.
 *
 * If bit k of hop_info_[h] is set, then physical slot h+k contains an entry
 * whose home bucket is h.
 *
 * Example:
 *
 *     hop_info_[h] = 0b00101001
 *
 * Bits set: 0, 3, 5.
 *
 * Therefore entries belonging to home bucket h are physically stored at:
 *
 *     h + 0
 *     h + 3
 *     h + 5
 *
 * Diagram:
 *
 *     offset:       0       1       2       3       4       5       6       7
 *                   |                       |               |
 *                   v                       v               v
 *     slots:     +------+-------+-------+------+-------+------+-------+-------+
 *                |  E0  | empty | empty |  E1  | empty |  E2  | empty | empty |
 *                +------+-------+-------+------+-------+------+-------+-------+
 *                  h+0                     h+3             h+5
 *
 * Lookup therefore does not scan the whole table. It computes the home bucket,
 * reads one hop bitmap, and then checks only the indicated candidate offsets.
 *
 * ---------------------------------------------------------------------------
 * 3. Circular indexing
 * ---------------------------------------------------------------------------
 *
 * The table is logically circular:
 *
 *     ... capacity-2, capacity-1, 0, 1, 2, ...
 *
 * Since capacity is a power of two, wrapping is simply:
 *
 *     wrap(i) = i & (capacity - 1)
 *
 * Example with capacity = 16:
 *
 *     wrap(15) = 15
 *     wrap(16) = 0
 *     wrap(17) = 1
 *
 * A neighborhood near the end wraps around:
 *
 *     capacity = 16
 *     Neighborhood = 8
 *     home = 13
 *
 *     neighborhood = 13, 14, 15, 0, 1, 2, 3, 4
 *
 * This is elegant algorithmically. The tradeoff is that wrapped neighborhoods
 * are not physically contiguous in memory, but the boundary region is small
 * when capacity is much larger than Neighborhood.
 *
 * ---------------------------------------------------------------------------
 * 4. Data-oriented storage layout
 * ---------------------------------------------------------------------------
 *
 * A classic object-oriented bucket layout might store one struct per slot:
 *
 *     struct Bucket {
 *         HopWord hop;
 *         std::size_t hash;
 *         std::optional<Key> key;
 *         std::optional<Value> value;
 *     };
 *
 *     std::vector<Bucket> buckets;
 *
 * This implementation instead uses a structure-of-arrays layout:
 *
 *     hop_info_[i]  -- hop bitmap for home bucket i
 *     hashes_[i]    -- stored mixed hash for the entry physically at slot i
 *     keys_[i]      -- optional key physically at slot i
 *     values_[i]    -- optional value physically at slot i
 *
 * ASCII view:
 *
 *     slot index:     0        1        2        3        4       ...
 *
 *     hop_info_:   [ h0 ]   [ h1 ]   [ h2 ]   [ h3 ]   [ h4 ]
 *     hashes_:     [ x0 ]   [ -- ]   [ x2 ]   [ x3 ]   [ -- ]
 *     keys_:       [ k0 ]   [null]   [ k2 ]   [ k3 ]   [null]
 *     values_:     [ v0 ]   [null]   [ v2 ]   [ v3 ]   [null]
 *
 * A slot is occupied exactly when:
 *
 *     keys_[i].has_value()
 *
 * The implementation maintains the invariant:
 *
 *     keys_[i].has_value() == values_[i].has_value()
 *
 * This layout separates relatively hot metadata from payload data. Lookup first
 * reads hop_info_ and hashes_, then only touches keys_ if the hash matches, and
 * only touches values_ after a successful key comparison.
 *
 * ---------------------------------------------------------------------------
 * 5. Important ownership rule for hop_info_
 * ---------------------------------------------------------------------------
 *
 * This is the most important implementation detail:
 *
 *     hop_info_[i] belongs to home bucket i.
 *     It does not belong to the payload physically stored at slot i.
 *
 * Therefore, when Hopscotch insertion moves an entry from one physical slot to
 * another, it moves the physical hash/key/value columns, but it does not move
 * hop_info_. Instead, the table updates the hop bitmap of the moved entry's
 * home bucket.
 *
 * Example:
 *
 *     Before moving an entry owned by home C:
 *
 *         C                 old_pos                 free_pos
 *         |                    |                        |
 *         v                    v                        v
 *         +----+----+----+----+-------+----+----+----+------+
 *         | C  |    |    |    | entry |    |    |    | free |
 *         +----+----+----+----+-------+----+----+----+------+
 *                              ^                        ^
 *                         old_offset                new_offset
 *
 *     After:
 *
 *         C                 old_pos                 free_pos
 *         |                    |                        |
 *         v                    v                        v
 *         +----+----+----+----+------+----+----+----+-------+
 *         | C  |    |    |    | free |    |    |    | entry |
 *         +----+----+----+----+------+----+----+----+-------+
 *
 *     Then:
 *
 *         clear bit old_offset in hop_info_[C]
 *         set   bit new_offset in hop_info_[C]
 *
 * Moving the bitmap alone would be wrong. The bitmap only describes where the
 * entry is; it is not the entry itself.
 *
 * ---------------------------------------------------------------------------
 * 6. Current limitations
 * ---------------------------------------------------------------------------
 *
 * This is a compact educational implementation. It currently does not provide:
 *
 *     - iterators
 *     - allocator support
 *     - heterogeneous lookup
 *     - operator[]
 *     - strong exception-safety guarantees for all throwing move/copy cases
 *     - a dense-payload indirection layout for very large inline values
 *
 * For large payloads, users can store handles, IDs, pointers, or smart pointers
 * as the Value type. A future DenseHopscotchMap variant could avoid moving large
 * inline payloads during relocation by storing payloads separately and moving
 * only slot-to-entry indices.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

/**
 * @class HopscotchMap
 * @brief Circular Hopscotch hash map with metadata columns and optional key/value columns.
 *
 * @tparam Key Key type.
 * @tparam Value Mapped value type.
 * @tparam Hasher Hash functor type. Defaults to std::hash<Key>.
 * @tparam Neighborhood Number of physical slots in one home bucket's neighborhood.
 *
 * @details
 * This class stores entries in an open-addressed table. Every key has a home
 * bucket, and every live entry is kept within @p Neighborhood circular slots of
 * its home bucket.
 *
 * The public API is intentionally small:
 *
 *     - insert_or_assign
 *     - find
 *     - contains
 *     - erase
 *     - clear
 *     - reserve
 *
 * Insertions may relocate existing entries. Relocation moves the physical
 * hash/key/value columns and then updates the home bucket's hop bitmap.
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
     * @brief Integer type used for each hop bitmap.
     *
     * With std::uint32_t, the maximum supported Neighborhood is 32.
     */
    using HopWord = std::uint32_t;

    static_assert(Neighborhood > 0,
                  "Neighborhood must be positive.");

    static_assert(Neighborhood <= sizeof(HopWord) * 8,
                  "Neighborhood must fit inside HopWord.");

    static_assert(std::is_copy_constructible_v<Key>,
                  "insert_or_assign(const Key&, ...) requires copy-constructible Key.");

    static_assert(std::is_copy_constructible_v<Value>,
                  "insert_or_assign(..., const Value&) requires copy-constructible Value.");

    static_assert(std::is_move_constructible_v<Key>,
                  "Hopscotch relocation requires move-constructible Key.");

    static_assert(std::is_move_constructible_v<Value>,
                  "Hopscotch relocation requires move-constructible Value.");

public:
    /**
     * @brief Construct a map with at least @p capacity_hint physical slots.
     *
     * @param capacity_hint Minimum desired capacity. The actual capacity is
     * rounded up to a valid power-of-two capacity and is at least Neighborhood.
     *
     * @note The constructor argument is a hint/minimum, not an exact capacity.
     * For example, with Neighborhood = 32, passing 8 creates capacity 32.
     */
    explicit HopscotchMap(std::size_t capacity_hint = 64);

    /** @brief Return the number of live entries. */
    [[nodiscard]] std::size_t size() const noexcept;

    /** @brief Return the number of physical slots in the table. */
    [[nodiscard]] std::size_t capacity() const noexcept;

    /** @brief Return true if the map contains no live entries. */
    [[nodiscard]] bool empty() const noexcept;

    /** @brief Return size() / capacity(). */
    [[nodiscard]] double load_factor() const noexcept;

    /**
     * @brief Set maximum load factor.
     *
     * @param lf Desired load factor in the open interval (0, 1).
     *
     * @throws std::invalid_argument if @p lf is not in the open interval (0, 1).
     */
    void max_load_factor(double lf);

    /** @brief Return configured maximum load factor. */
    [[nodiscard]] double max_load_factor() const noexcept;

    /**
     * @brief Find a mutable value by key.
     *
     * @param key Key to search for.
     * @return Pointer to the mapped value if found, otherwise nullptr.
     *
     * @warning The returned pointer is invalidated by operations that may move
     * or resize entries, such as insert_or_assign(), erase(), clear(), reserve(),
     * or any operation that triggers resize.
     */
    [[nodiscard]] Value* find(const Key& key);

    /**
     * @brief Find a const value by key.
     *
     * @param key Key to search for.
     * @return Pointer to the mapped value if found, otherwise nullptr.
     */
    [[nodiscard]] const Value* find(const Key& key) const;

    /**
     * @brief Return true if key is present.
     *
     * @param key Key to search for.
     */
    [[nodiscard]] bool contains(const Key& key) const;

    /**
     * @brief Insert a new key/value pair or replace an existing value.
     *
     * @param key Key to insert or update.
     * @param value Value to insert or replace.
     * @return true if a new entry was inserted; false if an existing value was replaced.
     *
     * @details
     * Existing values are replaced via reset() + emplace(), so Value does not
     * need to be copy-assignable. It only needs to be copy-constructible for
     * this overload.
     */
    bool insert_or_assign(const Key& key, const Value& value);

    /**
     * @brief Erase key from the map.
     *
     * @param key Key to erase.
     * @return true if an entry was erased; false if key was not found.
     *
     * @details
     * No tombstone is needed. Lookup follows hop bitmap bits, so deletion only
     * has to clear the relevant hop bit and reset the optional key/value slot.
     */
    bool erase(const Key& key);

    /**
     * @brief Remove all entries while preserving allocated capacity.
     *
     * @details
     * All hop bitmaps are cleared and all optional key/value slots are reset.
     */
    void clear();

    /**
     * @brief Reserve enough buckets for approximately @p requested_size entries.
     *
     * @param requested_size Number of entries the caller would like to store
     * without exceeding the configured maximum load factor.
     */
    void reserve(std::size_t requested_size);

private:
    // ---------------------------------------------------------------------
    // Table state
    // ---------------------------------------------------------------------

    /** @brief Number of live entries currently stored. */
    std::size_t size_ = 0;

    /** @brief Number of physical slots. Always a power of two. */
    std::size_t capacity_ = 0;

    /** @brief Bitmask used for circular indexing. Always capacity_ - 1. */
    std::size_t mask_ = 0;

    /** @brief Maximum allowed size()/capacity() before growing. */
    double max_load_factor_ = 0.85;

    /**
     * @brief Maximum resize retries for one public insertion.
     *
     * This prevents an accidental infinite loop with pathological hashers that
     * map too many distinct keys to the same home bucket forever.
     */
    static constexpr int max_insert_resize_attempts_ = 8;

    /** @brief User-provided or default hash functor. */
    Hasher hasher_{};

    // ---------------------------------------------------------------------
    // Metadata columns, one element per physical slot
    // ---------------------------------------------------------------------

    /**
     * @brief Hop bitmap for each home bucket.
     *
     * If bit k of hop_info_[h] is set, then wrap(h + k) contains an entry whose
     * home bucket is h.
     */
    std::vector<HopWord> hop_info_;

    /**
     * @brief Stored mixed hash for the entry physically located at each slot.
     *
     * hashes_[i] is meaningful only when is_occupied(i) is true.
     */
    std::vector<std::size_t> hashes_;

    // ---------------------------------------------------------------------
    // Optional payload columns, one element per physical slot
    // ---------------------------------------------------------------------

    /**
     * @brief Optional key physically stored at each slot.
     *
     * keys_[i].has_value() is the occupancy test for slot i.
     */
    std::vector<std::optional<Key>> keys_;

    /**
     * @brief Optional value physically stored at each slot.
     *
     * The implementation maintains:
     *
     *     keys_[i].has_value() == values_[i].has_value()
     */
    std::vector<std::optional<Value>> values_;

private:
    // ---------------------------------------------------------------------
    // Capacity / indexing helpers
    // ---------------------------------------------------------------------

    /** @brief Round n up to a valid capacity. */
    static std::size_t normalize_capacity(std::size_t n);

    /** @brief Allocate a new empty table and discard old storage. */
    void allocate_empty(std::size_t new_capacity);

    /** @brief Map an arbitrary integer index into the circular table. */
    [[nodiscard]] std::size_t wrap(std::size_t index) const noexcept;

    /** @brief Circular forward distance from slot @p from to slot @p to. */
    [[nodiscard]] std::size_t distance(std::size_t from, std::size_t to) const noexcept;

    /** @brief Compute home bucket from a mixed hash value. */
    [[nodiscard]] std::size_t home_index(std::size_t hash) const noexcept;

    // ---------------------------------------------------------------------
    // Hashing
    // ---------------------------------------------------------------------

    /** @brief Finalizer-style mixer used before power-of-two masking. */
    static std::uint64_t mix64(std::uint64_t x) noexcept;

    /** @brief Apply user hasher and internal mixer. */
    [[nodiscard]] std::size_t hash_key(const Key& key) const;

    // ---------------------------------------------------------------------
    // Hop bitmap helpers
    // ---------------------------------------------------------------------

    /** @brief Set offset bit in the hop bitmap of @p home. */
    void set_hop_bit(std::size_t home, std::size_t offset);

    /** @brief Clear offset bit in the hop bitmap of @p home. */
    void clear_hop_bit(std::size_t home, std::size_t offset);

    // ---------------------------------------------------------------------
    // Occupancy / slot helpers
    // ---------------------------------------------------------------------

    /** @brief Return true if physical slot @p pos contains a live entry. */
    [[nodiscard]] bool is_occupied(std::size_t pos) const;

    /** @brief Reset optional key/value payloads at physical slot @p pos. */
    void clear_slot(std::size_t pos);

    /** @brief Place a new key/value/hash triple into an empty physical slot. */
    void place_slot(std::size_t pos, const Key& key, const Value& value, std::size_t hash);

    /** @brief Move hash/key/value columns from one physical slot to another. */
    void move_payload_slot(std::size_t from, std::size_t to);

    // ---------------------------------------------------------------------
    // Lookup helpers
    // ---------------------------------------------------------------------

    /** @brief Find physical position of key, computing the hash internally. */
    [[nodiscard]] std::optional<std::size_t> find_position(const Key& key) const;

    /** @brief Find physical position of key using a precomputed mixed hash. */
    [[nodiscard]] std::optional<std::size_t> find_position_with_hash(
        const Key& key,
        std::size_t hash
    ) const;

    // ---------------------------------------------------------------------
    // Insertion / resize helpers
    // ---------------------------------------------------------------------

    /** @brief Grow if one additional entry would exceed max_load_factor_. */
    void ensure_capacity_for_one_more();

    /** @brief Find the first empty slot at or after @p home in circular order. */
    [[nodiscard]] std::optional<std::size_t> find_free_slot(std::size_t home) const;

    /** @brief Try to insert without growing the table. */
    [[nodiscard]] bool insert_no_resize(const Key& key, const Value& value, std::size_t hash);

    /** @brief Try to move the current free slot closer to the desired home. */
    [[nodiscard]] bool move_free_slot_closer(std::size_t& free_pos);

    /** @brief Allocate a larger table and reinsert all live entries. */
    void resize(std::size_t requested_capacity);
};

#include "HopscotchMap.tpp"
