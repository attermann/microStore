/*
 * Compile-and-smoke test for the microStore forward iterator.
 * Uses a RAM-backed filesystem to verify basic iterator behaviour.
 */

#include <unity.h>
#include <microStore.hpp>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>

/* ---- Minimal RAM filesystem ---- */

struct RamFile {
    std::vector<uint8_t> data;
    size_t pos = 0;
    bool open = false;
    bool write_mode = false;
    bool append_mode = false;
};

static RamFile g_files[16];
static char    g_names[16][64];
static int     g_nfiles = 0;

static int find_file(const char* name) {
    for (int i = 0; i < g_nfiles; i++)
        if (strcmp(g_names[i], name) == 0) return i;
    return -1;
}

static microStore::FileHandle ram_open(const char* name, const char* mode) {
    microStore::FileHandle fh; fh.ctx = nullptr;

    bool rd = (strchr(mode, 'r') != nullptr);
    bool wr = (strchr(mode, 'w') != nullptr);
    bool ap = (strchr(mode, 'a') != nullptr);

    int idx = find_file(name);

    if (wr) {
        if (idx < 0) {
            if (g_nfiles >= 16) return fh;
            idx = g_nfiles++;
            strncpy(g_names[idx], name, 63);
        }
        g_files[idx].data.clear();
        g_files[idx].pos = 0;
        g_files[idx].open = true;
        g_files[idx].write_mode = true;
        g_files[idx].append_mode = false;
    } else if (ap) {
        if (idx < 0) {
            if (g_nfiles >= 16) return fh;
            idx = g_nfiles++;
            strncpy(g_names[idx], name, 63);
        }
        g_files[idx].open = true;
        g_files[idx].write_mode = true;
        g_files[idx].append_mode = true;
        g_files[idx].pos = g_files[idx].data.size();  // seek to end
    } else {
        if (idx < 0) return fh;  // read: file must exist
        g_files[idx].pos = 0;
        g_files[idx].open = true;
        g_files[idx].write_mode = false;
        g_files[idx].append_mode = false;
    }
    (void)rd;
    fh.ctx = (void*)(intptr_t)(idx + 1);
    return fh;
}

static size_t ram_read(microStore::FileHandle fh, void* buf, size_t len) {
    int idx = (int)(intptr_t)fh.ctx - 1;
    RamFile& f = g_files[idx];
    size_t avail = f.data.size() - f.pos;
    size_t n = (len < avail) ? len : avail;
    memcpy(buf, f.data.data() + f.pos, n);
    f.pos += n;
    return n;
}

static size_t ram_write(microStore::FileHandle fh, const void* buf, size_t len) {
    int idx = (int)(intptr_t)fh.ctx - 1;
    RamFile& f = g_files[idx];
    if (f.append_mode) f.pos = f.data.size();
    size_t need = f.pos + len;
    if (f.data.size() < need) f.data.resize(need);
    memcpy(f.data.data() + f.pos, buf, len);
    f.pos += len;
    return len;
}

static int ram_seek(microStore::FileHandle fh, long off, int whence) {
    int idx = (int)(intptr_t)fh.ctx - 1;
    RamFile& f = g_files[idx];
    size_t new_pos;
    if (whence == SEEK_SET)       new_pos = (size_t)off;
    else if (whence == SEEK_END)  new_pos = (size_t)((long)f.data.size() + off);
    else                          new_pos = (size_t)((long)f.pos + off);
    f.pos = new_pos;
    return 0;
}

static long ram_tell(microStore::FileHandle fh) {
    int idx = (int)(intptr_t)fh.ctx - 1;
    return (long)g_files[idx].pos;
}

static int  ram_flush(microStore::FileHandle) { return 0; }

static int  ram_close(microStore::FileHandle fh) {
    int idx = (int)(intptr_t)fh.ctx - 1;
    g_files[idx].open = false;
    return 0;
}

static int  ram_remove(const char* name) {
    int idx = find_file(name);
    if (idx < 0) return -1;
    g_files[idx].data.clear();
    g_files[idx].pos = 0;
    // Shift remaining entries down
    for (int i = idx; i < g_nfiles - 1; i++) {
        g_files[i] = g_files[i + 1];
        strncpy(g_names[i], g_names[i + 1], 63);
    }
    g_nfiles--;
    return 0;
}

static int  ram_rename(const char* src, const char* dst) {
    int si = find_file(src);
    if (si < 0) return -1;
    int di = find_file(dst);
    if (di >= 0) ram_remove(dst);
    di = find_file(src);  // re-find after possible remove
    strncpy(g_names[di], dst, 63);
    return 0;
}

static microStore::FileSystemInterface g_fs = {
    ram_open, ram_read, ram_write, ram_seek, ram_tell,
    ram_flush, ram_close, ram_remove, ram_rename
};

/* ---- Helpers ---- */

static void reset_ram_fs() {
    for (int i = 0; i < g_nfiles; i++) g_files[i].data.clear();
    g_nfiles = 0;
}

/* ---- Tests ---- */

void test_iterator_empty_store() {
    reset_ram_fs();
    microStore::Storage store;
    store.init(&g_fs, "/test");

    int count = 0;
    for (auto& e : store) {
        (void)e;
        count++;
    }
    TEST_ASSERT_EQUAL(0, count);
}

void test_iterator_single_record() {
    reset_ram_fs();
    microStore::Storage store;
    store.init(&g_fs, "/test");

    const char* val = "hello";
    store.put("mykey", 42, val, (uint16_t)strlen(val));

    int count = 0;
    for (auto& e : store) {
        count++;
        TEST_ASSERT_EQUAL(5, (int)e.key.size());
        TEST_ASSERT_EQUAL(0, memcmp(e.key.data(), "mykey", 5));
        TEST_ASSERT_EQUAL(5, (int)e.value.size());
        TEST_ASSERT_EQUAL(0, memcmp(e.value.data(), "hello", 5));
        TEST_ASSERT_EQUAL(42u, e.timestamp);
    }
    TEST_ASSERT_EQUAL(1, count);
}

void test_iterator_multiple_records() {
    reset_ram_fs();
    microStore::Storage store;
    store.init(&g_fs, "/test");

    store.put("alpha", 1, "AAA", 3);
    store.put("beta",  2, "BB",  2);
    store.put("gamma", 3, "C",   1);

    // Collect results into a map for order-independent comparison
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

void test_iterator_skips_deleted_keys() {
    reset_ram_fs();
    microStore::Storage store;
    store.init(&g_fs, "/test");

    store.put("keep",   1, "yes", 3);
    store.put("delete", 2, "no",  2);
    store.remove("delete");

    std::map<std::string, std::string> seen;
    for (auto& e : store) {
        std::string k(e.key.begin(), e.key.end());
        std::string v(e.value.begin(), e.value.end());
        seen[k] = v;
    }

    TEST_ASSERT_EQUAL(1, (int)seen.size());
    TEST_ASSERT_EQUAL_STRING("yes", seen["keep"].c_str());
    TEST_ASSERT_FALSE(seen.count("delete"));
}

void test_iterator_overwrite_shows_latest() {
    reset_ram_fs();
    microStore::Storage store;
    store.init(&g_fs, "/test");

    store.put("k", 1, "first",  5);
    store.put("k", 2, "second", 6);

    std::vector<std::string> vals;
    for (auto& e : store) {
        vals.push_back(std::string(e.value.begin(), e.value.end()));
    }

    TEST_ASSERT_EQUAL(1, (int)vals.size());
    TEST_ASSERT_EQUAL_STRING("second", vals[0].c_str());
}

void test_iterator_satisfies_forward_iterator() {
    // Compile-time check: iterator_traits must resolve correctly.
    using It = microStore::Storage::iterator;
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
            microStore::Storage::Entry
        >::value,
        "value_type must be Entry"
    );
    TEST_PASS();
}

/* ---- Main ---- */

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_iterator_empty_store);
    RUN_TEST(test_iterator_single_record);
    RUN_TEST(test_iterator_multiple_records);
    RUN_TEST(test_iterator_skips_deleted_keys);
    RUN_TEST(test_iterator_overwrite_shows_latest);
    RUN_TEST(test_iterator_satisfies_forward_iterator);
    return UNITY_END();
}
