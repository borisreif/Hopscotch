#pragma once

/**
 * @file HopscotchMap.hpp
 * @brief Declaration of a circular, power-of-two, data-oriented Hopscotch map.
 *
 * File split:
 *
 *   HopscotchMap.hpp  -- class declaration, public API, data members
 *   HopscotchMap.tpp  -- template member function definitions
 *   HopscotchMap.cpp  -- optional translation unit / explicit instantiations
 *   demo.cpp          -- small executable demonstration
 *
 * Important template note:
 *
 *   HopscotchMap is a class template. In normal C++, template definitions must
 *   be visible at the point of instantiation. Therefore, this header includes
 *   HopscotchMap.tpp at the bottom.
 *
 * Design summary:
 *
 *   - Capacity is always a power of two.
 *   - Table indexing is circular.
 *   - Storage is columnar / structure-of-arrays.
 *   - Metadata is separated from payload.
 *   - Key and value payload columns are separated.
 *   - Empty slots use std::optional, so empty slots do not contain live Key or
 *     Value objects.
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
 * @tparam Value Value type.
 * @tparam Hasher Hash functor type. Defaults to std::hash<Key>.
 * @tparam Neighborhood Number of slots in one home bucket's neighborhood.
 *
 * @details
 * This is an educational implementation. It is intentionally small and avoids
 * advanced allocator/lifetime machinery.
 *
 * Main invariant:
 *
 *   If a key has home bucket h and physically lives at slot p, then:
 *
 *       distance(h, p) < Neighborhood
 *
 * The hop bitmap hop_info_[h] stores which offsets from h currently contain
 * entries whose home bucket is h.
 */
template <
    typename Key,
    typename Value,
    typename Hasher = std::hash<Key>,
    std::size_t Neighborhood = 32
>
class HopscotchMap {
private:
    /** @brief Integer type used for the hop bitmap. */
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
     * @brief Construct a map with at least capacity_hint slots.
     *
     * @param capacity_hint Minimum desired capacity. The actual capacity is
     * rounded up to a valid power-of-two capacity and at least Neighborhood.
     */
    explicit HopscotchMap(std::size_t capacity_hint = 64);

    /** @brief Return the number of live entries. */
    [[nodiscard]] std::size_t size() const noexcept;

    /** @brief Return the number of physical slots. */
    [[nodiscard]] std::size_t capacity() const noexcept;

    /** @brief Return true if the map contains no live entries. */
    [[nodiscard]] bool empty() const noexcept;

    /** @brief Return size() / capacity(). */
    [[nodiscard]] double load_factor() const noexcept;

    /**
     * @brief Set maximum load factor.
     *
     * @throws std::invalid_argument if lf is not in the open interval (0, 1).
     */
    void max_load_factor(double lf);

    /** @brief Return configured maximum load factor. */
    [[nodiscard]] double max_load_factor() const noexcept;

    /**
     * @brief Find a mutable value by key.
     *
     * @return Pointer to value if found, otherwise nullptr.
     */
    [[nodiscard]] Value* find(const Key& key);

    /**
     * @brief Find a const value by key.
     *
     * @return Pointer to value if found, otherwise nullptr.
     */
    [[nodiscard]] const Value* find(const Key& key) const;

    /** @brief Return true if key is present. */
    [[nodiscard]] bool contains(const Key& key) const;

    /**
     * @brief Insert a new key/value pair or replace an existing value.
     *
     * @return true if a new entry was inserted; false if an existing value was replaced.
     */
    bool insert_or_assign(const Key& key, const Value& value);

    /**
     * @brief Erase key from the map.
     *
     * @return true if an entry was erased; false if key was not found.
     */
    bool erase(const Key& key);

    /** @brief Remove all entries while preserving allocated capacity. */
    void clear();

    /** @brief Reserve enough buckets for approximately requested_size entries. */
    void reserve(std::size_t requested_size);

private:
    // ---------------------------------------------------------------------
    // Table state
    // ---------------------------------------------------------------------

    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    double max_load_factor_ = 0.85;

    static constexpr int max_insert_resize_attempts_ = 8;

    Hasher hasher_{};

    // ---------------------------------------------------------------------
    // Metadata columns, one element per physical slot
    // ---------------------------------------------------------------------

    /**
     * @brief Hop bitmap for each home bucket.
     *
     * If bit k of hop_info_[h] is set, then slot h + k contains an entry whose
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

    /** @brief Optional key physically stored at each slot. */
    std::vector<std::optional<Key>> keys_;

    /** @brief Optional value physically stored at each slot. */
    std::vector<std::optional<Value>> values_;

private:
    // ---------------------------------------------------------------------
    // Capacity / indexing helpers
    // ---------------------------------------------------------------------

    static std::size_t normalize_capacity(std::size_t n);
    void allocate_empty(std::size_t new_capacity);

    [[nodiscard]] std::size_t wrap(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t distance(std::size_t from, std::size_t to) const noexcept;
    [[nodiscard]] std::size_t home_index(std::size_t hash) const noexcept;

    // ---------------------------------------------------------------------
    // Hashing
    // ---------------------------------------------------------------------

    static std::uint64_t mix64(std::uint64_t x) noexcept;
    [[nodiscard]] std::size_t hash_key(const Key& key) const;

    // ---------------------------------------------------------------------
    // Hop bitmap helpers
    // ---------------------------------------------------------------------

    void set_hop_bit(std::size_t home, std::size_t offset);
    void clear_hop_bit(std::size_t home, std::size_t offset);

    // ---------------------------------------------------------------------
    // Occupancy / slot helpers
    // ---------------------------------------------------------------------

    [[nodiscard]] bool is_occupied(std::size_t pos) const;
    void clear_slot(std::size_t pos);
    void place_slot(std::size_t pos, const Key& key, const Value& value, std::size_t hash);
    void move_payload_slot(std::size_t from, std::size_t to);

    // ---------------------------------------------------------------------
    // Lookup helpers
    // ---------------------------------------------------------------------

    [[nodiscard]] std::optional<std::size_t> find_position(const Key& key) const;
    [[nodiscard]] std::optional<std::size_t> find_position_with_hash(
        const Key& key,
        std::size_t hash
    ) const;

    // ---------------------------------------------------------------------
    // Insertion / resize helpers
    // ---------------------------------------------------------------------

    void ensure_capacity_for_one_more();
    [[nodiscard]] std::optional<std::size_t> find_free_slot(std::size_t home) const;
    [[nodiscard]] bool insert_no_resize(const Key& key, const Value& value, std::size_t hash);
    [[nodiscard]] bool move_free_slot_closer(std::size_t& free_pos);
    void resize(std::size_t requested_capacity);
};

#include "HopscotchMap.tpp"
