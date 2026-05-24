#include "HopscotchMap.hpp"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    HopscotchMap<std::string, int> map(8);

    std::cout << "Initial capacity: " << map.capacity() << '\n';

    assert(map.empty());

    assert(map.insert_or_assign("apple", 10));
    assert(map.insert_or_assign("banana", 20));
    assert(map.insert_or_assign("cherry", 30));

    assert(map.size() == 3);
    assert(map.contains("apple"));
    assert(map.contains("banana"));
    assert(map.contains("cherry"));
    assert(!map.contains("date"));

    if (auto* value = map.find("banana")) {
        std::cout << "banana -> " << *value << '\n';
        assert(*value == 20);
    } else {
        assert(false && "banana should be present");
    }

    // Assign existing key.
    assert(!map.insert_or_assign("banana", 99));
    assert(*map.find("banana") == 99);

    // Erase.
    assert(map.erase("cherry"));
    assert(!map.contains("cherry"));
    assert(!map.erase("not-present"));

    // Trigger growth and verify all entries survive resizing.
    const std::size_t old_capacity = map.capacity();

    for (int i = 0; i < 1000; ++i) {
        const std::string key = "key_" + std::to_string(i);
        assert(map.insert_or_assign(key, i));
    }

    std::cout << "Old capacity: " << old_capacity << '\n';
    std::cout << "New capacity: " << map.capacity() << '\n';
    std::cout << "Size:         " << map.size() << '\n';
    std::cout << "Load factor:  " << map.load_factor() << '\n';

    for (int i = 0; i < 1000; ++i) {
        const std::string key = "key_" + std::to_string(i);
        auto* value = map.find(key);
        assert(value != nullptr);
        assert(*value == i);
    }

    // Mutate through find().
    *map.find("apple") = 1234;
    assert(*map.find("apple") == 1234);

    map.clear();
    assert(map.empty());
    assert(!map.contains("apple"));
    assert(!map.contains("key_500"));

    std::cout << "All demo checks passed.\n";
    return 0;
}
