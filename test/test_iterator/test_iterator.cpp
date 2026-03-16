/*
 * Compile-and-smoke test for the microStore forward iterator.
 * Uses a RAM-backed filesystem to verify basic iterator behaviour.
 */

#include <unity.h>
#include <microStore/Store.hpp>

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

class RamFileImpl : public microStore::FileImpl {
    int _idx;
    bool _closed = false;

public:
    RamFileImpl(int idx) : microStore::FileImpl(), _idx(idx) {}
    virtual ~RamFileImpl() { if (!_closed) close(); }

protected:
    virtual const char* name() const override { return g_names[_idx]; }

    virtual size_t size() const override { return g_files[_idx].data.size(); }

    virtual void close() override {
        g_files[_idx].open = false;
        _closed = true;
    }

    virtual int read() override {
        RamFile& f = g_files[_idx];
        if (f.pos >= f.data.size()) return EOF;
        return f.data[f.pos++];
    }

    virtual size_t write(uint8_t ch) override {
        RamFile& f = g_files[_idx];
        if (f.append_mode) f.pos = f.data.size();
        if (f.pos >= f.data.size()) f.data.resize(f.pos + 1);
        f.data[f.pos++] = ch;
        return 1;
    }

    virtual size_t read(uint8_t* buffer, size_t size) override {
        RamFile& f = g_files[_idx];
        size_t avail = f.data.size() - f.pos;
        size_t n = (size < avail) ? size : avail;
        memcpy(buffer, f.data.data() + f.pos, n);
        f.pos += n;
        return n;
    }

    virtual size_t write(const uint8_t* buffer, size_t size) override {
        RamFile& f = g_files[_idx];
        if (f.append_mode) f.pos = f.data.size();
        size_t need = f.pos + size;
        if (f.data.size() < need) f.data.resize(need);
        memcpy(f.data.data() + f.pos, buffer, size);
        f.pos += size;
        return size;
    }

    virtual int available() override {
        RamFile& f = g_files[_idx];
        return (int)(f.data.size() - f.pos);
    }

    virtual int peek() override {
        RamFile& f = g_files[_idx];
        if (f.pos >= f.data.size()) return EOF;
        return f.data[f.pos];
    }

    virtual size_t tell() override { return g_files[_idx].pos; }

    virtual long seek(uint32_t pos, microStore::SeekMode mode) override {
        RamFile& f = g_files[_idx];
        size_t new_pos;
        switch (mode) {
            case microStore::SeekModeEnd: new_pos = (size_t)((long)f.data.size() + (long)pos); break;
            case microStore::SeekModeCur: new_pos = (size_t)((long)f.pos + (long)pos); break;
            case microStore::SeekModeSet:
            default:                      new_pos = (size_t)pos; break;
        }
        f.pos = new_pos;
        return (long)new_pos;
    }

    virtual void flush() override {}

    virtual bool isValid() const override { return !_closed; }
};

class RamFileSystemImpl : public microStore::FileSystemImpl {
public:
    RamFileSystemImpl() {}

protected:
    virtual microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override {
        bool wr = (mode == microStore::File::ModeWrite || mode == microStore::File::ModeReadWrite);
        bool ap = (mode == microStore::File::ModeAppend || mode == microStore::File::ModeReadAppend);

        int idx = find_file(path);

        if (wr) {
            if (idx < 0) {
                if (g_nfiles >= 16) return {};
                idx = g_nfiles++;
                strncpy(g_names[idx], path, 63);
                g_names[idx][63] = '\0';
            }
            g_files[idx].data.clear();
            g_files[idx].pos = 0;
            g_files[idx].open = true;
            g_files[idx].write_mode = true;
            g_files[idx].append_mode = false;
        } else if (ap) {
            if (idx < 0) {
                if (g_nfiles >= 16) return {};
                idx = g_nfiles++;
                strncpy(g_names[idx], path, 63);
                g_names[idx][63] = '\0';
            }
            g_files[idx].open = true;
            g_files[idx].write_mode = true;
            g_files[idx].append_mode = true;
            g_files[idx].pos = g_files[idx].data.size();
        } else {
            if (idx < 0) return {};
            g_files[idx].pos = 0;
            g_files[idx].open = true;
            g_files[idx].write_mode = false;
            g_files[idx].append_mode = false;
        }
        (void)create;
        return microStore::File(new RamFileImpl(idx));
    }

    virtual bool exists(const char* path) override { return find_file(path) >= 0; }

    virtual bool remove(const char* path) override {
        int idx = find_file(path);
        if (idx < 0) return false;
        g_files[idx].data.clear();
        g_files[idx].pos = 0;
        for (int i = idx; i < g_nfiles - 1; i++) {
            g_files[i] = g_files[i + 1];
            strncpy(g_names[i], g_names[i + 1], 63);
        }
        g_nfiles--;
        return true;
    }

    virtual bool rename(const char* src, const char* dst) override {
        int si = find_file(src);
        if (si < 0) return false;
        int di = find_file(dst);
        if (di >= 0) remove(dst);
        di = find_file(src);
        strncpy(g_names[di], dst, 63);
        g_names[di][63] = '\0';
        return true;
    }

    virtual bool mkdir(const char* path) override { (void)path; return true; }
    virtual bool rmdir(const char* path) override { (void)path; return true; }
    virtual bool isDirectory(const char* path) override { (void)path; return false; }

    virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) override {
        (void)path; (void)callback;
        return {};
    }

    virtual size_t storageSize() override { return 0; }
    virtual size_t storageAvailable() override { return 0; }
};

/* ---- Helpers ---- */

static void reset_ram_fs() {
    for (int i = 0; i < g_nfiles; i++) {
        g_files[i].data.clear();
        g_files[i].pos = 0;
        g_files[i].open = false;
    }
    g_nfiles = 0;
}

static microStore::FileSystem make_ram_fs() {
    return microStore::FileSystem(new RamFileSystemImpl());
}

/* ---- Tests ---- */

void test_iterator_empty_store() {
    reset_ram_fs();
    microStore::Store store;
    store.init(make_ram_fs(), "/test");

    int count = 0;
    for (auto& e : store) {
        (void)e;
        count++;
    }
    TEST_ASSERT_EQUAL(0, count);
}

void test_iterator_single_record() {
    reset_ram_fs();
    microStore::Store store;
    store.init(make_ram_fs(), "/test");

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
    microStore::Store store;
    store.init(make_ram_fs(), "/test");

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
    microStore::Store store;
    store.init(make_ram_fs(), "/test");

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
    microStore::Store store;
    store.init(make_ram_fs(), "/test");

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
    using It = microStore::Store::iterator;
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
            microStore::Store::Entry
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
