# Hopscotch

A small C++20 implementation of **Hopscotch hashing**.

This project is primarily educational. It explores how a Hopscotch hash table can be implemented with:

- circular indexing
- power-of-two capacity
- a bounded Hopscotch neighborhood
- structure-of-arrays storage
- separated metadata and payload columns
- `std::optional` key/value columns
- resize-by-reinsertion

The goal is to make the algorithm visible and understandable, not to replace `std::unordered_map`.

---

## Current status

This is an experimental learning implementation.

Implemented:

- insertion with `insert_or_assign`
- lookup with `find` and `contains`
- deletion with `erase`
- clearing with `clear`
- capacity reservation with `reserve`
- dynamic resizing by rebuilding the table
- circular power-of-two indexing
- internal hash mixing for power-of-two bucket selection
- optional key/value columns so empty slots do not contain live `Key` or `Value` objects

Not implemented yet:

- iterators
- `operator[]`
- `emplace` / `try_emplace`
- allocator support
- heterogeneous lookup
- full STL container compatibility
- benchmarking
- production-grade exception-safety guarantees for all throwing move/copy cases

---

## What is Hopscotch hashing?

In a normal hash table, each key has a home bucket:

```cpp
home = hash(key) % capacity;
```

This implementation keeps capacity as a power of two, so bucket selection becomes:

```cpp
home = hash(key) & (capacity - 1);
```

Hopscotch hashing allows an entry to live near its home bucket, but not arbitrarily far away.

For example, with a neighborhood size of 8:

```text
home bucket h
     |
     v
+-----+-----+-----+-----+-----+-----+-----+-----+
| h+0 | h+1 | h+2 | h+3 | h+4 | h+5 | h+6 | h+7 |
+-----+-----+-----+-----+-----+-----+-----+-----+
```

A key whose home bucket is `h` must be stored in one of those slots.

The central invariant is:

```text
distance(home, physical_position) < Neighborhood
```

Because this table is circular, distance is measured modulo the table capacity.

---

## Hop bitmaps

Each bucket has a small bitmap called `hop_info`.

If bit `k` of `hop_info_[h]` is set, then physical slot `h + k` contains an entry whose home bucket is `h`.

Example:

```text
hop_info_[h] = 0b00101001
```

Bits set: `0`, `3`, and `5`.

So home bucket `h` owns entries at:

```text
h + 0
h + 3
h + 5
```

Diagram:

```text
offset:       0       1       2       3       4       5       6       7
              |                       |               |
              v                       v               v
slots:     +------+-------+-------+------+-------+------+-------+-------+
           |  E0  | empty | empty |  E1  | empty |  E2  | empty | empty |
           +------+-------+-------+------+-------+------+-------+-------+
             h+0                     h+3             h+5
```

Lookup does not scan the whole table. It computes the home bucket, reads one hop bitmap, and checks only the indicated candidate offsets.

---

## Data-oriented layout

A classic object-oriented bucket layout might look like this:

```cpp
struct Bucket {
    HopWord hop;
    std::size_t hash;
    std::optional<Key> key;
    std::optional<Value> value;
};

std::vector<Bucket> buckets;
```

This implementation instead uses a **structure-of-arrays** layout:

```cpp
std::vector<HopWord> hop_info_;
std::vector<std::size_t> hashes_;

std::vector<std::optional<Key>> keys_;
std::vector<std::optional<Value>> values_;
```

ASCII view:

```text
slot index:     0        1        2        3        4       ...

hop_info_:   [ h0 ]   [ h1 ]   [ h2 ]   [ h3 ]   [ h4 ]
hashes_:     [ x0 ]   [ -- ]   [ x2 ]   [ x3 ]   [ -- ]
keys_:       [ k0 ]   [null]   [ k2 ]   [ k3 ]   [null]
values_:     [ v0 ]   [null]   [ v2 ]   [ v3 ]   [null]
```

A slot is occupied exactly when:

```cpp
keys_[i].has_value()
```

The implementation maintains this invariant:

```cpp
keys_[i].has_value() == values_[i].has_value()
```

The metadata and payload are separated so lookup can first inspect compact metadata and only touch keys/values when needed.

---

## Important implementation rule

`hop_info_[i]` belongs to **home bucket `i`**.

It does **not** belong to the payload physically stored at slot `i`.

When Hopscotch insertion moves an entry, it moves the physical hash/key/value columns, but it does not move `hop_info_`. Instead, it updates the bitmap of the moved entry's home bucket.

Example:

```text
Before moving an entry owned by home C:

    C                 old_pos                 free_pos
    |                    |                        |
    v                    v                        v
    +----+----+----+----+-------+----+----+----+------+
    | C  |    |    |    | entry |    |    |    | free |
    +----+----+----+----+-------+----+----+----+------+
                         ^                        ^
                    old_offset                new_offset

After:

    C                 old_pos                 free_pos
    |                    |                        |
    v                    v                        v
    +----+----+----+----+------+----+----+----+-------+
    | C  |    |    |    | free |    |    |    | entry |
    +----+----+----+----+------+----+----+----+-------+

Then:

    clear bit old_offset in hop_info_[C]
    set   bit new_offset in hop_info_[C]
```

Changing the bitmap alone would be wrong. The bitmap only describes where the entry is; it is not the entry itself.

---

## Basic usage

```cpp
#include "HopscotchMap.hpp"

#include <iostream>
#include <string>

int main() {
    HopscotchMap<std::string, int> map;

    map.insert_or_assign("apple", 10);
    map.insert_or_assign("banana", 20);
    map.insert_or_assign("cherry", 30);

    if (auto* value = map.find("banana")) {
        std::cout << "banana -> " << *value << '\n';
    }

    map.insert_or_assign("banana", 99);

    if (map.contains("banana")) {
        std::cout << "banana is present\n";
    }

    map.erase("apple");

    std::cout << "size: " << map.size() << '\n';
}
```

---

## API overview

### Construction

```cpp
HopscotchMap<Key, Value> map;
HopscotchMap<Key, Value> map_with_hint(128);
```

The constructor argument is a **capacity hint**, not an exact capacity. The actual capacity is rounded up to a valid internal capacity:

- at least `Neighborhood`
- a power of two

### Insertion and assignment

```cpp
bool inserted = map.insert_or_assign(key, value);
```

Return value:

```text
true  = inserted a new key
false = key already existed and its value was replaced
```

### Lookup

```cpp
Value* value = map.find(key);
```

Returns `nullptr` if the key is not present.

Const lookup is also supported:

```cpp
const Value* value = map.find(key);
```

### Contains

```cpp
if (map.contains(key)) {
    // key exists
}
```

### Erase

```cpp
bool removed = map.erase(key);
```

Return value:

```text
true  = key was found and erased
false = key was not present
```

### Capacity and size

```cpp
map.size();
map.capacity();
map.empty();
map.load_factor();
map.max_load_factor();
map.max_load_factor(0.80);
map.reserve(1000);
```

---

## Build

The project uses C++20.

On Ubuntu, install a compiler and debugger:

```bash
sudo apt update
sudo apt install build-essential gdb
```

Build the demo manually:

```bash
g++ -std=c++20 -Wall -Wextra -pedantic demo.cpp HopscotchMap.cpp -o demo
```

Run it:

```bash
./demo
```

For debugging, add `-g -O0`:

```bash
g++ -std=c++20 -Wall -Wextra -pedantic -g -O0 demo.cpp HopscotchMap.cpp -o demo
```

---

## File layout

```text
HopscotchMap.hpp   class declaration, public API, data members, documentation
HopscotchMap.tpp   template member function definitions
HopscotchMap.cpp   optional translation unit / future explicit instantiations
demo.cpp           small example and sanity check program
legacy/            earlier experiments
.vscode/           optional VSCode build/debug configuration
```

Important template note:

`HopscotchMap` is a class template. Template definitions must be visible at the point of instantiation, so `HopscotchMap.hpp` includes `HopscotchMap.tpp` at the bottom.

Users should normally include only:

```cpp
#include "HopscotchMap.hpp"
```

---

## Hashing

This table uses power-of-two capacity, so bucket selection uses low bits:

```cpp
home = hash & mask;
```

To reduce problems with weak low bits, the implementation applies an internal 64-bit mixer to the result of the hasher before indexing.

The default hasher is:

```cpp
std::hash<Key>
```

Future improvements may include easier injection of custom/stateful hashers and an optional mixer policy.

---

## Payload size notes

This implementation stores keys and values directly in table slots, using optional columns:

```cpp
std::vector<std::optional<Key>> keys_;
std::vector<std::optional<Value>> values_;
```

During Hopscotch insertion, entries may be relocated. That means keys and values may be moved to another physical slot.

For small or cheaply movable values, this is fine.

For large inline values, consider storing an indirection:

```cpp
HopscotchMap<std::string, std::shared_ptr<Image>> images;
HopscotchMap<std::string, ImageId> image_index;
HopscotchMap<std::string, std::unique_ptr<Image>> owned_images;
```

A future dense-payload variant could avoid moving large inline payloads by storing payloads separately and moving only slot-to-entry indices.

---

## Roadmap

Possible next steps:

- add unit tests
- add randomized differential tests against `std::unordered_map`
- add CMake support
- add custom hasher constructor support
- add `insert` that does not replace existing values
- add `emplace` / `try_emplace`
- add benchmarks
- investigate fingerprint metadata
- investigate dense-payload storage as a second variant
- investigate iterator support

---

## License

No license has been specified yet. Add one before sharing or reusing the code more broadly.
