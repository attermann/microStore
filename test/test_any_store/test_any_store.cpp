/*
 * Unit tests for microStore::AnyStore — type-erased runtime store wrapper.
 *
 * Most tests are written once against AnyStore& and run against both a
 * HeapStore backend and a FileStore backend on a RAM filesystem.
 */

#include <unity.h>
#include <microStore/AnyStore.h>
#include <microStore/FileStore.h>
#include <microStore/HeapStore.h>
#include <microStore/TypedStore.h>

#include <cstring>
#include <cstdio>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

using microStore::AnyStore;
using microStore::FileStore;
using microStore::HeapStore;

/* ---- Minimal RAM filesystem (copied from test_file_store) ---- */

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
        if (g_names[i][0] != '\0' && strcmp(g_names[i], name) == 0) return i;
    return -1;
}

// Find first slot with an empty name (tombstone or never used), or extend g_nfiles.
static int alloc_slot() {
    for (int i = 0; i < g_nfiles; i++)
        if (g_names[i][0] == '\0') return i;
    if (g_nfiles >= 16) return -1;
    return g_nfiles++;
}

// Unlike the copy in test_file_store, read position and append mode are kept
// per handle, not per file. FileStore keeps the active segment open for append
// while get() opens a separate read handle on the same file; with shared state
// the read handle would clobber the writer's append mode.
class RamFileImpl : public microStore::FileImpl {
    int _idx;
    size_t _pos;
    bool _append;
    bool _closed = false;
public:
    RamFileImpl(int idx, size_t pos, bool append) : microStore::FileImpl(), _idx(idx), _pos(pos), _append(append) {}
    virtual ~RamFileImpl() { if (!_closed) close(); }
protected:
    virtual const char* name()  const override { return g_names[_idx]; }
    virtual size_t      size()  const override { return g_files[_idx].data.size(); }
    virtual void close() override { g_files[_idx].open = false; _closed = true; }
    virtual int read() override {
        RamFile& f = g_files[_idx];
        if (_pos >= f.data.size()) return EOF;
        return f.data[_pos++];
    }
    virtual size_t write(uint8_t ch) override { return write(&ch, 1); }
    virtual size_t read(uint8_t* buffer, size_t size) override {
        RamFile& f = g_files[_idx];
        size_t avail = (_pos < f.data.size()) ? f.data.size() - _pos : 0;
        size_t n = (size < avail) ? size : avail;
        memcpy(buffer, f.data.data() + _pos, n);
        _pos += n;
        return n;
    }
    virtual size_t write(const uint8_t* buffer, size_t size) override {
        RamFile& f = g_files[_idx];
        if (_append) _pos = f.data.size();
        size_t need = _pos + size;
        if (f.data.size() < need) f.data.resize(need);
        memcpy(f.data.data() + _pos, buffer, size);
        _pos += size;
        return size;
    }
    virtual int  available() override { RamFile& f=g_files[_idx]; return (_pos<f.data.size())?(int)(f.data.size()-_pos):0; }
    virtual int  peek()      override { RamFile& f=g_files[_idx]; return (_pos<f.data.size())?f.data[_pos]:EOF; }
    virtual size_t tell()    override { return _pos; }
    virtual long seek(uint32_t pos, microStore::SeekMode mode) override {
        RamFile& f = g_files[_idx];
        switch (mode) {
            case microStore::SeekModeEnd: _pos = (size_t)((long)f.data.size() + (long)pos); break;
            case microStore::SeekModeCur: _pos = (size_t)((long)_pos + (long)pos); break;
            default:                      _pos = (size_t)pos; break;
        }
        return (long)_pos;
    }
    virtual void flush() override {}
    virtual bool isValid() const override { return !_closed; }
};

class RamFileSystemImpl : public microStore::FileSystemImpl {
protected:
    virtual microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override {
        bool wr = (mode == microStore::File::ModeWrite || mode == microStore::File::ModeReadWrite);
        bool ap = (mode == microStore::File::ModeAppend || mode == microStore::File::ModeReadAppend);
        int idx = find_file(path);
        if (wr) {
            if (idx < 0) { idx = alloc_slot(); if (idx < 0) return {}; strncpy(g_names[idx], path, 63); g_names[idx][63] = '\0'; }
            g_files[idx].data.clear(); g_files[idx].pos = 0; g_files[idx].open = true;
            g_files[idx].write_mode = true; g_files[idx].append_mode = false;
        } else if (ap) {
            if (idx < 0) {
                idx = alloc_slot(); if (idx < 0) return {};
                strncpy(g_names[idx], path, 63); g_names[idx][63] = '\0';
                g_files[idx].data.clear(); g_files[idx].pos = 0;
            }
            g_files[idx].open = true; g_files[idx].write_mode = true; g_files[idx].append_mode = true;
            g_files[idx].pos = g_files[idx].data.size();
        } else {
            if (idx < 0) return {};
            g_files[idx].pos = 0; g_files[idx].open = true;
            g_files[idx].write_mode = false; g_files[idx].append_mode = false;
        }
        (void)create;
        return microStore::File(new RamFileImpl(idx, g_files[idx].pos, g_files[idx].append_mode));
    }
    virtual bool exists(const char* path) override { return find_file(path) >= 0; }
    virtual bool remove(const char* path) override {
        int idx = find_file(path);
        if (idx < 0) return false;
        // Tombstone: clear data and mark name empty so the slot can be reused.
        // Do NOT shift other entries — that would invalidate stored _idx values in
        // any open RamFileImpl objects.
        g_files[idx].data.clear(); g_files[idx].pos = 0;
        g_files[idx].open = false; g_files[idx].write_mode = false; g_files[idx].append_mode = false;
        g_names[idx][0] = '\0';
        // Trim g_nfiles past trailing tombstones so alloc_slot() still works.
        while (g_nfiles > 0 && g_names[g_nfiles - 1][0] == '\0') g_nfiles--;
        return true;
    }
    virtual bool rename(const char* src, const char* dst) override {
        int si = find_file(src); if (si < 0) return false;
        int di = find_file(dst);
        if (di >= 0) {
            // Remove dst in-place (no shift), then rename src.
            g_files[di].data.clear(); g_files[di].pos = 0;
            g_files[di].open = false; g_files[di].write_mode = false; g_files[di].append_mode = false;
            g_names[di][0] = '\0';
            while (g_nfiles > 0 && g_names[g_nfiles - 1][0] == '\0') g_nfiles--;
            // si is still valid (no shift happened).
        }
        strncpy(g_names[si], dst, 63); g_names[si][63] = '\0';
        return true;
    }
    virtual bool mkdir(const char* path) override { (void)path; return true; }
    virtual bool rmdir(const char* path) override { (void)path; return true; }
    virtual bool isDirectory(const char* path) override { (void)path; return false; }
    virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing cb = nullptr) override {
        (void)path; (void)cb; return {};
    }
    virtual size_t storageSize()      override { return 0; }
    virtual size_t storageAvailable() override { return 0; }
};

static void reset_ram_fs() {
    for (int i = 0; i < 16; i++) {
        g_files[i].data.clear(); g_files[i].pos = 0;
        g_files[i].open = false; g_files[i].write_mode = false; g_files[i].append_mode = false;
        g_names[i][0] = '\0';
    }
    g_nfiles = 0;
}
static microStore::FileSystem make_ram_fs() { return microStore::FileSystem{new RamFileSystemImpl()}; }

static void remove_ram_file(const char* name) {
    int idx = find_file(name);
    if (idx < 0) return;
    g_files[idx].data.clear(); g_files[idx].pos = 0;
    g_files[idx].open = false; g_files[idx].write_mode = false; g_files[idx].append_mode = false;
    g_names[idx][0] = '\0';
    while (g_nfiles > 0 && g_names[g_nfiles - 1][0] == '\0') g_nfiles--;
}


/* ---- Backend selection ---- */

enum Backend { BACKEND_HEAP, BACKEND_FILE };

// Build an initialized AnyStore for the given backend. For FileStore this
// resets the RAM filesystem first so each test starts from a clean disk.
static AnyStore make_store(Backend b, uint32_t max_recs = 0, uint32_t ttl_secs = 0) {
    if (b == BACKEND_FILE) {
        reset_ram_fs();
        AnyStore s = AnyStore::make<FileStore>();
        if (max_recs) s.set_max_recs(max_recs);
        if (ttl_secs) s.set_ttl_secs(ttl_secs);
        auto fs = make_ram_fs();
        s.target<FileStore>()->init(fs, "/p");
        return s;
    }
    AnyStore s = AnyStore::make<HeapStore>();
    if (max_recs) s.set_max_recs(max_recs);
    if (ttl_secs) s.set_ttl_secs(ttl_secs);
    s.target<HeapStore>()->init();
    return s;
}

static std::string get_str(AnyStore& s, const char* key) {
    std::vector<uint8_t> out;
    if (!s.get(std::vector<uint8_t>(key, key + strlen(key)), out)) return "<missing>";
    return std::string(out.begin(), out.end());
}

/* ===========================================================================
 * Empty / uninitialized stores
 * =========================================================================== */

void test_empty_store_is_invalid_and_safe() {
    AnyStore s;
    TEST_ASSERT_FALSE(s.isValid());
    TEST_ASSERT_FALSE((bool)s);
    TEST_ASSERT_FALSE(s.hasStore());

    uint8_t buf[8]; uint16_t sz = sizeof(buf);
    std::vector<uint8_t> k(1, 'k'), v;
    TEST_ASSERT_FALSE(s.put("k", "v", 1u));
    TEST_ASSERT_FALSE(s.put(k, k));
    TEST_ASSERT_FALSE(s.get("k", buf, &sz));
    TEST_ASSERT_FALSE(s.get(k, v));
    TEST_ASSERT_FALSE(s.exists("k"));
    TEST_ASSERT_FALSE(s.exists(k));
    TEST_ASSERT_FALSE(s.remove("k"));
    TEST_ASSERT_FALSE(s.remove(k));
    TEST_ASSERT_EQUAL(0u, s.size());
    s.clear();
    s.close();
    s.set_ttl_secs(10);
    s.set_max_recs(10);
    s.dumpInfo(true);
    TEST_ASSERT_TRUE(s.begin() == s.end());
    TEST_ASSERT_FALSE(s.begin() != s.end());
    TEST_ASSERT_NULL(s.target<HeapStore>());
    TEST_ASSERT_NULL(s.target<FileStore>());

    // Through TypedStore as well
    microStore::TypedStore<std::string, std::string, AnyStore> t(s);
    std::string out;
    TEST_ASSERT_FALSE(t.isValid());
    TEST_ASSERT_FALSE(t.put("a", "b"));
    TEST_ASSERT_FALSE(t.get("a", out));
    TEST_ASSERT_EQUAL(0u, t.size());
    TEST_ASSERT_FALSE(t.begin() != t.end());
}

void test_uninitialized_filestore_backend_is_invalid() {
    reset_ram_fs();
    AnyStore s = AnyStore::make<FileStore>();
    TEST_ASSERT_TRUE(s.hasStore());
    TEST_ASSERT_FALSE(s.isValid());
    TEST_ASSERT_FALSE(s.put("k", "v", 1u));
    TEST_ASSERT_FALSE(s.exists("k"));
    TEST_ASSERT_EQUAL(0u, s.size());
    TEST_ASSERT_NOT_NULL(s.target<FileStore>());
}

void test_reset_makes_store_empty() {
    AnyStore s = make_store(BACKEND_HEAP);
    TEST_ASSERT_TRUE(s.put("k", "v", 1u));
    s.reset();
    TEST_ASSERT_FALSE(s.isValid());
    TEST_ASSERT_EQUAL(0u, s.size());
    TEST_ASSERT_FALSE(s.exists("k"));
}

/* ===========================================================================
 * Shared behaviour, run against both backends
 * =========================================================================== */

static void check_put_get_exists_remove_size(Backend b) {
    AnyStore s = make_store(b);
    TEST_ASSERT_TRUE(s.isValid());
    TEST_ASSERT_TRUE((bool)s);
    TEST_ASSERT_EQUAL(0u, s.size());

    // const char* key + raw data
    TEST_ASSERT_TRUE(s.put("alpha", "AAA", 3u));
    // raw key
    const uint8_t rk[] = {0x01, 0x02, 0x03};
    const uint8_t rv[] = {0xDE, 0xAD};
    TEST_ASSERT_TRUE(s.put(rk, sizeof(rk), rv, sizeof(rv)));
    // vector key + raw data
    std::vector<uint8_t> vk = {'b', 'e', 't', 'a'};
    TEST_ASSERT_TRUE(s.put(vk, "BB", 2u));
    // vector key + vector data
    std::vector<uint8_t> vk2 = {'g', 'a', 'm'};
    std::vector<uint8_t> vv2 = {'C', 'C', 'C', 'C'};
    TEST_ASSERT_TRUE(s.put(vk2, vv2));

    TEST_ASSERT_EQUAL(4u, s.size());

    char buf[32]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_TRUE(s.get("alpha", buf, &sz));
    TEST_ASSERT_EQUAL(3u, sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "AAA", 3));

    sz = sizeof(buf);
    TEST_ASSERT_TRUE(s.get(rk, sizeof(rk), buf, &sz));
    TEST_ASSERT_EQUAL(2u, sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, rv, 2));

    sz = sizeof(buf);
    TEST_ASSERT_TRUE(s.get(vk, buf, &sz));
    TEST_ASSERT_EQUAL(2u, sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "BB", 2));

    std::vector<uint8_t> out;
    TEST_ASSERT_TRUE(s.get(vk2, out));
    TEST_ASSERT_TRUE(out == vv2);

    TEST_ASSERT_TRUE(s.exists("alpha"));
    TEST_ASSERT_TRUE(s.exists(rk, sizeof(rk)));
    TEST_ASSERT_TRUE(s.exists(vk));
    TEST_ASSERT_FALSE(s.exists("nope"));

    // overwrite does not change size
    TEST_ASSERT_TRUE(s.put("alpha", "second", 6u));
    TEST_ASSERT_EQUAL(4u, s.size());
    TEST_ASSERT_EQUAL_STRING("second", get_str(s, "alpha").c_str());

    TEST_ASSERT_TRUE(s.remove("alpha"));
    TEST_ASSERT_TRUE(s.remove(rk, sizeof(rk)));
    TEST_ASSERT_TRUE(s.remove(vk));
    TEST_ASSERT_EQUAL(1u, s.size());
    TEST_ASSERT_FALSE(s.exists("alpha"));
    TEST_ASSERT_FALSE(s.exists(vk));
    sz = sizeof(buf);
    TEST_ASSERT_FALSE(s.get("alpha", buf, &sz));

    s.clear();
    TEST_ASSERT_EQUAL(0u, s.size());
    TEST_ASSERT_FALSE(s.exists(vk2));
}
void test_heap_put_get_exists_remove_size() { check_put_get_exists_remove_size(BACKEND_HEAP); }
void test_file_put_get_exists_remove_size() { check_put_get_exists_remove_size(BACKEND_FILE); }

static void check_get_small_buffer_reports_full_size(Backend b) {
    AnyStore s = make_store(b);
    TEST_ASSERT_TRUE(s.put("k", "0123456789", 10u));
    char buf[4]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_TRUE(s.get("k", buf, &sz));
    TEST_ASSERT_EQUAL(10u, sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "0123", 4));
}
void test_heap_get_small_buffer() { check_get_small_buffer_reports_full_size(BACKEND_HEAP); }
void test_file_get_small_buffer() { check_get_small_buffer_reports_full_size(BACKEND_FILE); }

static void check_iteration(Backend b) {
    AnyStore s = make_store(b);
    TEST_ASSERT_TRUE(s.begin() == s.end());

    // Current timestamps: HeapStore sweeps records whose own ttl has elapsed.
    uint32_t now = microStore::time();
    s.put("alpha", "AAA", 3u, /*ttl=*/0,    now - 2);
    s.put("beta",  "BB",  2u, /*ttl=*/3600, now - 1);
    s.put("gamma", "C",   1u, /*ttl=*/0,    now);

    std::map<std::string, std::string> values;
    std::map<std::string, uint32_t> stamps;
    for (const AnyStore::Entry& e : s) {
        std::string k(e.key.begin(), e.key.end());
        values[k] = std::string(e.value.begin(), e.value.end());
        stamps[k] = e.timestamp;
    }
    TEST_ASSERT_EQUAL(3u, values.size());
    TEST_ASSERT_EQUAL_STRING("AAA", values["alpha"].c_str());
    TEST_ASSERT_EQUAL_STRING("BB",  values["beta"].c_str());
    TEST_ASSERT_EQUAL_STRING("C",   values["gamma"].c_str());
    TEST_ASSERT_EQUAL(now - 2, stamps["alpha"]);
    TEST_ASSERT_EQUAL(now - 1, stamps["beta"]);
    TEST_ASSERT_EQUAL(now,     stamps["gamma"]);

    TEST_ASSERT_EQUAL(3, (int)std::distance(s.begin(), s.end()));

    // operator-> is metadata-only; operator* loads the value.
    for (AnyStore::iterator it = s.begin(); it != s.end(); ++it) {
        std::string k(it->key.begin(), it->key.end());
        TEST_ASSERT_TRUE(it->value.empty());
        if (k == "beta") TEST_ASSERT_EQUAL(3600u, it->ttl);
        TEST_ASSERT_EQUAL_STRING(values[k].c_str(),
            std::string((*it).value.begin(), (*it).value.end()).c_str());
        TEST_ASSERT_FALSE(it->value.empty());  // cached after operator*
    }

    // post-increment returns an independent copy of the old position
    AnyStore::iterator a = s.begin();
    std::string first(a->key.begin(), a->key.end());
    AnyStore::iterator old = a++;
    TEST_ASSERT_EQUAL_STRING(first.c_str(), std::string(old->key.begin(), old->key.end()).c_str());
    TEST_ASSERT_TRUE(old != a);
    ++old;
    TEST_ASSERT_TRUE(old == a);

    // copies share position until one advances
    AnyStore::iterator c1 = s.begin();
    AnyStore::iterator c2 = c1;
    ++c2;
    TEST_ASSERT_TRUE(c1 == s.begin());
    TEST_ASSERT_TRUE(c1 != c2);

    // iterating to the end compares equal to the null end() sentinel
    AnyStore::iterator e = s.begin();
    ++e; ++e; ++e;
    TEST_ASSERT_TRUE(e == s.end());
    TEST_ASSERT_TRUE(s.end() == e);
}
void test_heap_iteration() { check_iteration(BACKEND_HEAP); }
void test_file_iteration() { check_iteration(BACKEND_FILE); }

void test_iterator_traits() {
    static_assert(std::is_same<
        std::iterator_traits<AnyStore::iterator>::iterator_category,
        std::forward_iterator_tag>::value, "must be a forward iterator");
    static_assert(std::is_same<
        std::iterator_traits<AnyStore::iterator>::value_type,
        AnyStore::Entry>::value, "value_type must be Entry");
    TEST_PASS();
}

static void check_typed_store(Backend b) {
    AnyStore s = make_store(b);
    microStore::TypedStore<std::string, std::string, AnyStore> t(s);
    TEST_ASSERT_TRUE(t.isValid());
    TEST_ASSERT_TRUE(t.put("one", "1"));
    TEST_ASSERT_TRUE(t.put("two", "22"));
    TEST_ASSERT_TRUE(t.put("three", "333"));
    TEST_ASSERT_EQUAL(3u, t.size());

    std::string v;
    TEST_ASSERT_TRUE(t.get("two", v));
    TEST_ASSERT_EQUAL_STRING("22", v.c_str());
    TEST_ASSERT_TRUE(t.exists("three"));
    TEST_ASSERT_TRUE(t.remove("three"));
    TEST_ASSERT_FALSE(t.exists("three"));

    std::map<std::string, std::string> seen;
    for (auto& e : t) seen[e.key] = e.value;
    TEST_ASSERT_EQUAL(2u, seen.size());
    TEST_ASSERT_EQUAL_STRING("1",  seen["one"].c_str());
    TEST_ASSERT_EQUAL_STRING("22", seen["two"].c_str());

    // Key-only set over the same AnyStore type
    AnyStore s2 = make_store(b);
    microStore::TypedKeyStore<std::string, AnyStore> keys(s2);
    TEST_ASSERT_TRUE(keys.put("alpha", {}));
    TEST_ASSERT_TRUE(keys.put("beta", {}));
    TEST_ASSERT_TRUE(keys.exists("alpha"));
    TEST_ASSERT_FALSE(keys.exists("gamma"));
    int n = 0;
    for (auto& e : keys) { (void)e; n++; }
    TEST_ASSERT_EQUAL(2, n);
}
void test_heap_typed_store() { check_typed_store(BACKEND_HEAP); }
void test_file_typed_store() { check_typed_store(BACKEND_FILE); }

/* ===========================================================================
 * Policy: max_recs and TTL
 * =========================================================================== */

void test_heap_max_recs_before_init() {
    AnyStore s = AnyStore::make<HeapStore>();
    s.set_max_recs(2);  // before init()
    s.target<HeapStore>()->init();
    s.put("k1", "1", 1u, 0, 1u);
    s.put("k2", "2", 1u, 0, 2u);
    s.put("k3", "3", 1u, 0, 3u);
    TEST_ASSERT_EQUAL(2u, s.size());
    TEST_ASSERT_FALSE(s.exists("k1"));
    TEST_ASSERT_TRUE(s.exists("k2"));
    TEST_ASSERT_TRUE(s.exists("k3"));
}

void test_file_max_recs_before_init() {
    reset_ram_fs();
    {
        AnyStore w = AnyStore::make<FileStore>();
        auto fs = make_ram_fs();
        w.target<FileStore>()->init(fs, "/p");
        w.put("k1", "1", 1u, 0, 1u);
        w.put("k2", "2", 1u, 0, 2u);
        w.put("k3", "3", 1u, 0, 3u);
        TEST_ASSERT_EQUAL(3u, w.size());
    }

    // Policy set through AnyStore before FileStore::init() -> boot-time pruning.
    AnyStore s = AnyStore::make<FileStore>();
    s.set_max_recs(2);
    auto fs = make_ram_fs();
    s.target<FileStore>()->init(fs, "/p");
    TEST_ASSERT_EQUAL(2u, s.size());
    TEST_ASSERT_FALSE(s.exists("k1"));
    TEST_ASSERT_TRUE(s.exists("k2"));
    TEST_ASSERT_TRUE(s.exists("k3"));

    // And put-time eviction of the oldest record.
    s.put("k4", "4", 1u, 0, microStore::time());
    TEST_ASSERT_EQUAL(2u, s.size());
    TEST_ASSERT_FALSE(s.exists("k2"));
    TEST_ASSERT_TRUE(s.exists("k3"));
    TEST_ASSERT_TRUE(s.exists("k4"));
}

static void check_ttl(Backend b) {
    AnyStore s = make_store(b);
    uint32_t now = microStore::time();
    s.put("old",   "stale", 5u, /*ttl=*/0, /*ts=*/1u);
    s.put("fresh", "live",  4u, /*ttl=*/0, now);
    TEST_ASSERT_EQUAL(2u, s.size());

    s.set_ttl_secs(60);
    char buf[16]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_FALSE(s.get("old", buf, &sz));
    sz = sizeof(buf);
    TEST_ASSERT_TRUE(s.get("fresh", buf, &sz));
    TEST_ASSERT_EQUAL(4u, sz);
    TEST_ASSERT_EQUAL(1u, s.size());
}
void test_heap_ttl_expires_on_get() { check_ttl(BACKEND_HEAP); }
void test_file_ttl_expires_on_get() { check_ttl(BACKEND_FILE); }

/* ===========================================================================
 * Construction, ownership and target<S>()
 * =========================================================================== */

void test_target_matches_backend_type() {
    AnyStore h = make_store(BACKEND_HEAP);
    TEST_ASSERT_NOT_NULL(h.target<HeapStore>());
    TEST_ASSERT_NULL(h.target<FileStore>());

    AnyStore f = make_store(BACKEND_FILE);
    TEST_ASSERT_NOT_NULL(f.target<FileStore>());
    TEST_ASSERT_NULL(f.target<HeapStore>());

    // const overload
    const AnyStore& ch = h;
    TEST_ASSERT_NOT_NULL(ch.target<HeapStore>());

    // writes through target are visible through the handle
    h.target<HeapStore>()->put("direct", "x", 1u);
    TEST_ASSERT_TRUE(h.exists("direct"));
}

void test_adopt_shared_ptr() {
    std::shared_ptr<HeapStore> heap = std::make_shared<HeapStore>();
    heap->init();
    heap->put("pre", "1", 1u);

    AnyStore s(heap);
    TEST_ASSERT_TRUE(s.isValid());
    TEST_ASSERT_TRUE(s.exists("pre"));
    TEST_ASSERT_EQUAL_PTR(heap.get(), s.target<HeapStore>());

    s.put("post", "2", 1u);
    TEST_ASSERT_TRUE(heap->exists("post"));

    // Shared ownership: the store survives the original pointer going away.
    heap.reset();
    TEST_ASSERT_TRUE(s.exists("post"));

    // Adopting a null pointer yields an empty store.
    AnyStore n((std::shared_ptr<HeapStore>()));
    TEST_ASSERT_FALSE(n.hasStore());
    TEST_ASSERT_FALSE(n.isValid());
}

void test_adopt_initialized_filestore() {
    reset_ram_fs();
    std::shared_ptr<FileStore> file = std::make_shared<FileStore>();
    auto fs = make_ram_fs();
    TEST_ASSERT_TRUE(file->init(fs, "/p"));
    AnyStore s(file);
    TEST_ASSERT_TRUE(s.isValid());
    TEST_ASSERT_TRUE(s.put("k", "v", 1u));
    TEST_ASSERT_EQUAL_STRING("v", get_str(s, "k").c_str());
    TEST_ASSERT_EQUAL_PTR(file.get(), s.target<FileStore>());
}

void test_allocate_with_allocator() {
    AnyStore s = AnyStore::allocate<HeapStore>(std::allocator<uint8_t>());
    TEST_ASSERT_NOT_NULL(s.target<HeapStore>());
    s.target<HeapStore>()->init();
    TEST_ASSERT_TRUE(s.put("k", "v", 1u));
    TEST_ASSERT_EQUAL(1u, s.size());
}

void test_make_forwards_constructor_args() {
    reset_ram_fs();
    AnyStore s = AnyStore::make<FileStore>((uint32_t)4096, (uint8_t)2);
    auto fs = make_ram_fs();
    TEST_ASSERT_TRUE(s.target<FileStore>()->init(fs, "/p"));
    TEST_ASSERT_TRUE(s.put("k", "v", 1u));
    TEST_ASSERT_TRUE(s.exists("k"));
}

void test_copies_share_backend() {
    AnyStore a = make_store(BACKEND_HEAP);
    AnyStore b = a;
    TEST_ASSERT_TRUE(a.put("k", "v", 1u));
    TEST_ASSERT_TRUE(b.exists("k"));
    TEST_ASSERT_EQUAL_PTR(a.target<HeapStore>(), b.target<HeapStore>());

    // Resetting one handle leaves the other usable.
    a.reset();
    TEST_ASSERT_TRUE(b.exists("k"));
}

void test_iterator_keeps_backend_alive() {
    AnyStore s = make_store(BACKEND_HEAP);
    s.put("k", "v", 1u);
    AnyStore::iterator it = s.begin();
    s.reset();  // drop the handle's reference
    TEST_ASSERT_TRUE(it != AnyStore::iterator());
    TEST_ASSERT_EQUAL_STRING("v", std::string((*it).value.begin(), (*it).value.end()).c_str());
    ++it;
    TEST_ASSERT_TRUE(it == AnyStore::iterator());
}

/* ===========================================================================
 * Runtime switching
 * =========================================================================== */

void test_switch_file_heap_file() {
    reset_ram_fs();
    auto fs = make_ram_fs();

    AnyStore s = AnyStore::make<FileStore>();
    s.target<FileStore>()->init(fs, "/p");
    TEST_ASSERT_TRUE(s.put("a", "1", 1u));
    TEST_ASSERT_TRUE(s.put("b", "2", 1u));
    TEST_ASSERT_EQUAL(2u, s.size());

    // File -> Heap: old FileStore is destroyed (flushed), heap starts empty.
    s.close();
    s = AnyStore::make<HeapStore>();
    s.target<HeapStore>()->init();
    TEST_ASSERT_TRUE(s.isValid());
    TEST_ASSERT_NOT_NULL(s.target<HeapStore>());
    TEST_ASSERT_EQUAL(0u, s.size());
    TEST_ASSERT_FALSE(s.exists("a"));
    TEST_ASSERT_TRUE(s.put("heap_only", "h", 1u));

    // Heap -> File: persisted records reload, heap records are gone.
    s = AnyStore::make<FileStore>();
    s.target<FileStore>()->init(fs, "/p");
    TEST_ASSERT_NOT_NULL(s.target<FileStore>());
    TEST_ASSERT_EQUAL(2u, s.size());
    TEST_ASSERT_EQUAL_STRING("1", get_str(s, "a").c_str());
    TEST_ASSERT_EQUAL_STRING("2", get_str(s, "b").c_str());
    TEST_ASSERT_FALSE(s.exists("heap_only"));

    // Heap -> fresh Heap starts empty again.
    s = AnyStore::make<HeapStore>();
    s.target<HeapStore>()->init();
    TEST_ASSERT_EQUAL(0u, s.size());
}

/* ===========================================================================
 * AnyStore as a private class member (documentation example)
 * =========================================================================== */

class PathTable {
public:
    PathTable() : _typed(_store) {}

    bool start(microStore::FileSystem* fs, bool transport_enabled) {
        if (transport_enabled && fs != nullptr) {
            std::shared_ptr<FileStore> file = std::make_shared<FileStore>();
            file->set_max_recs(256);
            if (!file->init(*fs, "/paths/")) return false;
            _store = AnyStore(file);
        } else {
            _store = AnyStore::make<HeapStore>();
            _store.set_max_recs(64);
            _store.set_ttl_secs(3600);
            _store.target<HeapStore>()->init();
        }
        return _store.isValid();
    }

    void stop() {
        _store.close();
        _store.reset();
    }

    bool add(const std::string& dest, const std::string& path) { return _typed.put(dest, path); }
    bool lookup(const std::string& dest, std::string& path)    { return _typed.get(dest, path); }
    bool forget(const std::string& dest)                       { return _typed.remove(dest); }
    size_t count() const                                       { return _typed.size(); }
    bool persistent() const { return _store.target<FileStore>() != nullptr; }

    std::map<std::string, std::string> all() {
        std::map<std::string, std::string> m;
        for (auto& e : _typed) m[e.key] = e.value;
        return m;
    }

private:
    AnyStore _store;
    microStore::TypedStore<std::string, std::string, AnyStore> _typed;
};

void test_class_member_runtime_selection() {
    reset_ram_fs();
    microStore::FileSystem ramfs = make_ram_fs();
    PathTable table;

    // Safe before start()
    std::string p;
    TEST_ASSERT_FALSE(table.add("x", "y"));
    TEST_ASSERT_FALSE(table.lookup("x", p));
    TEST_ASSERT_FALSE(table.forget("x"));
    TEST_ASSERT_EQUAL(0u, table.count());
    TEST_ASSERT_TRUE(table.all().empty());
    TEST_ASSERT_FALSE(table.persistent());

    // Transport enabled with a filesystem -> FileStore
    TEST_ASSERT_TRUE(table.start(&ramfs, true));
    TEST_ASSERT_TRUE(table.persistent());
    TEST_ASSERT_TRUE(table.add("node1", "via-a"));
    TEST_ASSERT_TRUE(table.add("node2", "via-b"));
    TEST_ASSERT_EQUAL(2u, table.count());
    table.stop();
    TEST_ASSERT_EQUAL(0u, table.count());

    // Transport disabled -> HeapStore, starts empty
    TEST_ASSERT_TRUE(table.start(nullptr, false));
    TEST_ASSERT_FALSE(table.persistent());
    TEST_ASSERT_EQUAL(0u, table.count());
    TEST_ASSERT_TRUE(table.add("ram", "only"));
    TEST_ASSERT_TRUE(table.lookup("ram", p));
    TEST_ASSERT_EQUAL_STRING("only", p.c_str());
    table.stop();

    // Back to FileStore -> persisted records reload
    TEST_ASSERT_TRUE(table.start(&ramfs, true));
    TEST_ASSERT_EQUAL(2u, table.count());
    TEST_ASSERT_TRUE(table.lookup("node1", p));
    TEST_ASSERT_EQUAL_STRING("via-a", p.c_str());
    TEST_ASSERT_FALSE(table.lookup("ram", p));
    std::map<std::string, std::string> m = table.all();
    TEST_ASSERT_EQUAL(2u, m.size());
    TEST_ASSERT_EQUAL_STRING("via-b", m["node2"].c_str());
    table.stop();
}

/* ---- Main ---- */

void setUp()    {}
void tearDown() {}

int runUnityTests(void) {
    UNITY_BEGIN();
    // Empty / uninitialized
    RUN_TEST(test_empty_store_is_invalid_and_safe);
    RUN_TEST(test_uninitialized_filestore_backend_is_invalid);
    RUN_TEST(test_reset_makes_store_empty);
    // CRUD
    RUN_TEST(test_heap_put_get_exists_remove_size);
    RUN_TEST(test_file_put_get_exists_remove_size);
    RUN_TEST(test_heap_get_small_buffer);
    RUN_TEST(test_file_get_small_buffer);
    // Iteration
    RUN_TEST(test_heap_iteration);
    RUN_TEST(test_file_iteration);
    RUN_TEST(test_iterator_traits);
    RUN_TEST(test_heap_typed_store);
    RUN_TEST(test_file_typed_store);
    // Policy
    RUN_TEST(test_heap_max_recs_before_init);
    RUN_TEST(test_file_max_recs_before_init);
    RUN_TEST(test_heap_ttl_expires_on_get);
    RUN_TEST(test_file_ttl_expires_on_get);
    // Construction / ownership
    RUN_TEST(test_target_matches_backend_type);
    RUN_TEST(test_adopt_shared_ptr);
    RUN_TEST(test_adopt_initialized_filestore);
    RUN_TEST(test_allocate_with_allocator);
    RUN_TEST(test_make_forwards_constructor_args);
    RUN_TEST(test_copies_share_backend);
    RUN_TEST(test_iterator_keeps_backend_alive);
    // Runtime switching
    RUN_TEST(test_switch_file_heap_file);
    RUN_TEST(test_class_member_runtime_selection);
    return UNITY_END();
}

// For native dev-platform or for some embedded frameworks
int main(void) {
	return runUnityTests();
}
