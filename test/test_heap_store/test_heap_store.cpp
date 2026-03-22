/*
 * Unit tests for microStore::HeapStore — in-memory KV store.
 */

#include <unity.h>
#include <microStore/HeapStore.h>

#include <cstring>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>

using microStore::HeapStore;

/* ---- Tests ---- */

void test_heap_store_put_get() {
    HeapStore store;
    store.init();

    const char* val = "hello";
    TEST_ASSERT_TRUE(store.put("mykey", val, (uint16_t)strlen(val)));

    char buf[32];
    uint16_t size = sizeof(buf);
    TEST_ASSERT_TRUE(store.get("mykey", buf, &size));
    TEST_ASSERT_EQUAL(5u, size);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "hello", 5));
}

void test_heap_store_overwrite() {
    HeapStore store;
    store.init();

    store.put("k", "first",  5u);
    store.put("k", "second", 6u);

    char buf[32];
    uint16_t size = sizeof(buf);
    TEST_ASSERT_TRUE(store.get("k", buf, &size));
    TEST_ASSERT_EQUAL(6u, size);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "second", 6));
}

void test_heap_store_remove() {
    HeapStore store;
    store.init();

    store.put("gone", "val", 3u);
    TEST_ASSERT_TRUE(store.exists("gone"));

    store.remove("gone");
    TEST_ASSERT_FALSE(store.exists("gone"));

    char buf[32];
    uint16_t size = sizeof(buf);
    TEST_ASSERT_FALSE(store.get("gone", buf, &size));
}

void test_heap_store_size() {
    HeapStore store;
    store.init();

    TEST_ASSERT_EQUAL(0u, store.size());

    store.put("a", "1", 1u);
    TEST_ASSERT_EQUAL(1u, store.size());

    store.put("b", "2", 1u);
    TEST_ASSERT_EQUAL(2u, store.size());

    // Overwrite does not increase size
    store.put("a", "x", 1u);
    TEST_ASSERT_EQUAL(2u, store.size());

    store.remove("a");
    TEST_ASSERT_EQUAL(1u, store.size());
}

void test_heap_store_clear() {
    HeapStore store;
    store.init();

    store.put("x", "1", 1u);
    store.put("y", "2", 1u);
    TEST_ASSERT_EQUAL(2u, store.size());

    store.clear();
    TEST_ASSERT_EQUAL(0u, store.size());
    TEST_ASSERT_FALSE(store.exists("x"));
    TEST_ASSERT_FALSE(store.exists("y"));
}

void test_heap_store_iterator() {
    HeapStore store;
    store.init();

    store.put("alpha", "AAA", 3u, 1u);
    store.put("beta",  "BB",  2u, 2u);
    store.put("gamma", "C",   1u, 3u);

    std::map<std::string, std::string> seen;
    for (auto& e : store) {
        std::string k(e.key.begin(), e.key.end());
        std::string v(e.value.begin(), e.value.end());
        seen[k] = v;
    }

    TEST_ASSERT_EQUAL(3, (int)seen.size());
    TEST_ASSERT_EQUAL_STRING("AAA", seen["alpha"].c_str());
    TEST_ASSERT_EQUAL_STRING("BB",  seen["beta"].c_str());
    TEST_ASSERT_EQUAL_STRING("C",   seen["gamma"].c_str());
}

void test_heap_store_policy_max_recs() {
    HeapStore store;
    store.init();

    // Allow at most 2 records; oldest is evicted when limit is exceeded
    store.set_ttl_secs(0);
    store.set_max_recs(2);

    store.put("first",  "1", 1u, 1u);
    store.put("second", "2", 1u, 2u);
    TEST_ASSERT_EQUAL(2u, store.size());

    // Adding a third key evicts the smallest (first alphabetically — "first")
    store.put("third", "3", 1u, 3u);
    TEST_ASSERT_EQUAL(2u, store.size());

    // Overwriting an existing key does not evict
    store.put("second", "updated", 7u, 4u);
    TEST_ASSERT_EQUAL(2u, store.size());
    TEST_ASSERT_TRUE(store.exists("second"));
}

/* ---- Iterator trait check ---- */

void test_heap_store_iterator_traits() {
    using It = HeapStore::iterator;
    static_assert(
        std::is_same<
            std::iterator_traits<It>::iterator_category,
            std::forward_iterator_tag
        >::value,
        "iterator_category must be forward_iterator_tag"
    );
    static_assert(
        std::is_same<
            std::iterator_traits<It>::value_type,
            HeapStore::Entry
        >::value,
        "value_type must be Entry"
    );
    TEST_PASS();
}

/* ---- Main ---- */

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_heap_store_put_get);
    RUN_TEST(test_heap_store_overwrite);
    RUN_TEST(test_heap_store_remove);
    RUN_TEST(test_heap_store_size);
    RUN_TEST(test_heap_store_clear);
    RUN_TEST(test_heap_store_iterator);
    RUN_TEST(test_heap_store_policy_max_recs);
    RUN_TEST(test_heap_store_iterator_traits);
    return UNITY_END();
}
