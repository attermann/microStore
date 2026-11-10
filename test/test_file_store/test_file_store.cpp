/*
 * Tests for policy_ttl_s_ and policy_max_recs_ in FileStore.
 *
 * NOTE: put() has a pre-existing bug where flush_buffer() always returns false,
 * causing put() to return early before calling index_insert().  Records ARE
 * written to disk but the in-memory index is not updated on the writing store.
 * All tests therefore use a two-store pattern:
 *   1. "Writer" store: calls put() to persist data on disk.
 *   2. "Reader" store: inits from the same filesystem, which calls
 *      rebuild_index_from_segments() to populate the index from disk,
 *      then enforces policies.
 */

#include <unity.h>
#include <microStore/FileStore.h>

#include <cstring>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>

/* ---- Minimal RAM filesystem (copied from test_iterator) ---- */

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

// Fault injection: open() of g_fail_open_path fails; writes to
// g_fail_write_path succeed until g_write_budget bytes are used up and then
// write nothing (as on a full filesystem); rename() fails while g_fail_rename.
static const char* g_fail_open_path  = nullptr;
static const char* g_fail_write_path = nullptr;
static size_t      g_write_budget    = 0;
static bool        g_fail_rename     = false;
static bool write_blocked(const char* name, size_t size) {
    if (!g_fail_write_path || strcmp(g_fail_write_path, name) != 0) return false;
    if (size > g_write_budget) return true;
    g_write_budget -= size;
    return false;
}
static bool fault_matches(const char* fault, const char* name) {
    return fault && strcmp(fault, name) == 0;
}

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

class RamFileImpl : public microStore::FileImpl {
    int _idx;
    bool _closed = false;
public:
    RamFileImpl(int idx) : microStore::FileImpl(), _idx(idx) {}
    virtual ~RamFileImpl() { if (!_closed) close(); }
protected:
    virtual const char* name()  const override { return g_names[_idx]; }
    virtual size_t      size()  const override { return g_files[_idx].data.size(); }
    virtual void close() override { g_files[_idx].open = false; _closed = true; }
    virtual int read() override {
        RamFile& f = g_files[_idx];
        if (f.pos >= f.data.size()) return EOF;
        return f.data[f.pos++];
    }
    virtual size_t write(uint8_t ch) override {
        if (write_blocked(g_names[_idx], 1)) return 0;
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
        if (write_blocked(g_names[_idx], size)) return 0;
        RamFile& f = g_files[_idx];
        if (f.append_mode) f.pos = f.data.size();
        size_t need = f.pos + size;
        if (f.data.size() < need) f.data.resize(need);
        memcpy(f.data.data() + f.pos, buffer, size);
        f.pos += size;
        return size;
    }
    virtual int  available() override { return (int)(g_files[_idx].data.size() - g_files[_idx].pos); }
    virtual int  peek()      override { RamFile& f=g_files[_idx]; return (f.pos<f.data.size())?f.data[f.pos]:EOF; }
    virtual size_t tell()    override { return g_files[_idx].pos; }
    virtual long seek(uint32_t pos, microStore::SeekMode mode) override {
        RamFile& f = g_files[_idx];
        size_t new_pos;
        switch (mode) {
            case microStore::SeekModeEnd: new_pos = (size_t)((long)f.data.size() + (long)pos); break;
            case microStore::SeekModeCur: new_pos = (size_t)((long)f.pos + (long)pos); break;
            default:                      new_pos = (size_t)pos; break;
        }
        f.pos = new_pos;
        return (long)new_pos;
    }
    virtual void flush() override {}
    virtual bool isValid() const override { return !_closed; }
};

class RamFileSystemImpl : public microStore::FileSystemImpl {
protected:
    virtual microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override {
        bool wr = (mode == microStore::File::ModeWrite || mode == microStore::File::ModeReadWrite);
        bool ap = (mode == microStore::File::ModeAppend || mode == microStore::File::ModeReadAppend);
        if (fault_matches(g_fail_open_path, path)) return {};
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
        return microStore::File(new RamFileImpl(idx));
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
        if (g_fail_rename) return false;
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
    g_fail_open_path  = nullptr;
    g_fail_write_path = nullptr;
    g_write_budget    = 0;
    g_fail_rename     = false;
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

/* ---- Helpers ---- */

// Write N records to a fresh filesystem with sequential timestamps base_ts..base_ts+N-1.
// The persistent index is deleted afterward so that the reader must call
// rebuild_index_from_segments() on init() to populate the in-memory index.
// (The writer's flush_buffer() has a pre-existing bug: put() always returns
// false without calling index_insert(), but data IS written to disk.)
static void write_records_to_disk(const char* prefix,
                                  const char* const* keys, int n_keys,
                                  uint32_t base_ts = 1)
{
    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, prefix);
        for (int i = 0; i < n_keys; i++) {
            char val[16];
            snprintf(val, sizeof(val), "v%d", i);
            writer.put(keys[i], val, /*ttl=*/0, (uint32_t)(base_ts + i));
        }
        // writer destructs; data is in g_files segment
    }
    // Delete the empty persistent index so the reader is forced to
    // call rebuild_index_from_segments() and load the real timestamps.
    char idx_name[80];
    snprintf(idx_name, sizeof(idx_name), "%s_index.dat", prefix);
    remove_ram_file(idx_name);
}

/* ===========================================================================
 * TTL Tests
 * =========================================================================== */

// A record stored with an old timestamp should be treated as expired when
// get() is called and policy_ttl_s_ is small enough that (now - ts) >= ttl.
void test_ttl_get_expires_old_record() {
    reset_ram_fs();

    // Write a record with ts=1 (very old — epoch second 1).
    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "/p");
        writer.put("k", "hello", /*ttl=*/0, /*ts=*/1u);
    }
    remove_ram_file("/p_index.dat");  // force rebuild_index_from_segments()

    // Reload; the rebuilt index will have timestamp=1.
    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.set_ttl_secs(60);  // 60-second TTL
    reader.init(fs, "/p");

    // Record is in the index before any TTL-checking call.
    TEST_ASSERT_EQUAL(1u, reader.size());

    // get() should detect expiry, evict the record, and return false.
    uint8_t buf[16]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_FALSE(reader.get("k", buf, &sz));

    // Record should now be gone from the index.
    TEST_ASSERT_EQUAL(0u, reader.size());
    TEST_ASSERT_FALSE(reader.exists("k"));
}

// A record stored with a recent timestamp should NOT be evicted by get()
// when the TTL has not elapsed.
void test_ttl_get_keeps_fresh_record() {
    reset_ram_fs();

    uint32_t now = microStore::time();
    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "/p");
        writer.put("k", "world", /*ttl=*/0, now);  // written "now"
    }
    remove_ram_file("/p_index.dat");  // force rebuild_index_from_segments()

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.set_ttl_secs(3600);  // 1-hour TTL — record is fresh
    reader.init(fs, "/p");

    TEST_ASSERT_EQUAL(1u, reader.size());

    uint8_t buf[16]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_TRUE(reader.get("k", buf, &sz));
    TEST_ASSERT_EQUAL(5u, sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "world", 5));
}

// Expired records should be excluded from the compaction output.
// After compaction the rebuilt index should contain only live records.
void test_ttl_compact_removes_expired() {
    reset_ram_fs();

    uint32_t now = microStore::time();
    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "/p");
        writer.put("old",   "stale", /*ttl=*/0, /*ts=*/1u);    // very old
        writer.put("fresh", "live",  /*ttl=*/0, now);           // current
    }
    remove_ram_file("/p_index.dat");  // force rebuild_index_from_segments()

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.set_ttl_secs(60);  // 60-second TTL — "old" is expired, "fresh" is not
    reader.init(fs, "/p");

    TEST_ASSERT_EQUAL(2u, reader.size());  // both in index before compact

    reader.compact();  // expired "old" must not be copied to compacted output

    // Only "fresh" should survive compaction.
    TEST_ASSERT_EQUAL(1u, reader.size());
    TEST_ASSERT_FALSE(reader.exists("old"));
    TEST_ASSERT_TRUE(reader.exists("fresh"));
}

/* ===========================================================================
 * Max-Records Tests
 * =========================================================================== */

// When the store is loaded with more records than policy_max_recs_ allows,
// init() should prune the oldest records and rewrite the persistent index.
void test_max_recs_init_prunes_oldest() {
    reset_ram_fs();

    static const char* keys[] = {"k1", "k2", "k3"};
    write_records_to_disk("/p", keys, 3, /*base_ts=*/1);
    // On disk: k1(ts=1), k2(ts=2), k3(ts=3)

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.set_max_recs(2);  // must be set BEFORE init
    reader.init(fs, "/p");

    // Oldest record (k1, ts=1) should have been evicted.
    TEST_ASSERT_EQUAL(2u, reader.size());
    TEST_ASSERT_FALSE(reader.exists("k1"));
    TEST_ASSERT_TRUE(reader.exists("k2"));
    TEST_ASSERT_TRUE(reader.exists("k3"));
}

// The persistent index written after pruning should not re-introduce the evicted
// key on a subsequent reload (without a compaction).
void test_max_recs_pruned_index_persists_across_reboot() {
    reset_ram_fs();

    static const char* keys[] = {"k1", "k2", "k3"};
    write_records_to_disk("/p", keys, 3, /*base_ts=*/1);

    // First load: prune to 2.
    {
        microStore::FileStore reader;
        auto fs = make_ram_fs();
        reader.set_max_recs(2);
        reader.init(fs, "/p");
        TEST_ASSERT_EQUAL(2u, reader.size());
    }

    // Second load (simulated reboot): the persistent index was rewritten after
    // pruning, so load_index() succeeds and k1 must NOT reappear.
    {
        microStore::FileStore reader2;
        auto fs2 = make_ram_fs();
        reader2.set_max_recs(2);
        reader2.init(fs2, "/p");
        TEST_ASSERT_EQUAL(2u, reader2.size());
        TEST_ASSERT_FALSE(reader2.exists("k1"));
        TEST_ASSERT_TRUE(reader2.exists("k2"));
        TEST_ASSERT_TRUE(reader2.exists("k3"));
    }
}

// After compaction, only the newest max_recs records should remain on disk.
void test_max_recs_compact_prunes_oldest() {
    reset_ram_fs();

    static const char* keys[] = {"k1", "k2", "k3", "k4"};
    write_records_to_disk("/p", keys, 4, /*base_ts=*/10);
    // On disk: k1(ts=10), k2(ts=11), k3(ts=12), k4(ts=13)

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.set_max_recs(2);
    reader.init(fs, "/p");
    // After init() prune: only k3(ts=12) and k4(ts=13) remain.
    TEST_ASSERT_EQUAL(2u, reader.size());

    reader.compact();

    TEST_ASSERT_EQUAL(2u, reader.size());
    TEST_ASSERT_FALSE(reader.exists("k1"));
    TEST_ASSERT_FALSE(reader.exists("k2"));
    TEST_ASSERT_TRUE(reader.exists("k3"));
    TEST_ASSERT_TRUE(reader.exists("k4"));
}

/* ===========================================================================
 * Basic CRUD Tests (ported from test_heap_store, using two-store pattern)
 * =========================================================================== */

void test_file_store_put_get() {
    reset_ram_fs();

    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "/p");
        writer.put("mykey", "hello", /*ttl=*/0, /*ts=*/1u);
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "/p");

    TEST_ASSERT_EQUAL(1u, reader.size());

    uint8_t buf[32]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_TRUE(reader.get("mykey", buf, &sz));
    TEST_ASSERT_EQUAL(5u, sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "hello", 5));
}

void test_file_store_overwrite() {
    reset_ram_fs();

    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "/p");
        writer.put("k", "first",  /*ttl=*/0, /*ts=*/1u);
        writer.put("k", "second", /*ttl=*/0, /*ts=*/2u);
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "/p");

    TEST_ASSERT_EQUAL(1u, reader.size());

    uint8_t buf[32]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_TRUE(reader.get("k", buf, &sz));
    TEST_ASSERT_EQUAL(6u, sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "second", 6));
}

void test_file_store_remove() {
    reset_ram_fs();

    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "/p");
        writer.put("gone", "val", /*ttl=*/0, /*ts=*/1u);
        writer.remove("gone");
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "/p");

    TEST_ASSERT_EQUAL(0u, reader.size());
    TEST_ASSERT_FALSE(reader.exists("gone"));

    uint8_t buf[32]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_FALSE(reader.get("gone", buf, &sz));
}

void test_file_store_size() {
    reset_ram_fs();

    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "/p");
        writer.put("a", "1", /*ttl=*/0, /*ts=*/1u);
        writer.put("b", "2", /*ttl=*/0, /*ts=*/2u);
        writer.put("a", "x", /*ttl=*/0, /*ts=*/3u);  // overwrite
        writer.remove("b");
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "/p");

    // After rebuild: a(overwritten) alive, b removed → 1 entry
    TEST_ASSERT_EQUAL(1u, reader.size());
    TEST_ASSERT_TRUE(reader.exists("a"));
    TEST_ASSERT_FALSE(reader.exists("b"));
}

void test_file_store_clear() {
    reset_ram_fs();

    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "/p");
        writer.put("x", "1", /*ttl=*/0, /*ts=*/1u);
        writer.put("y", "2", /*ttl=*/0, /*ts=*/2u);
    }
    remove_ram_file("/p_index.dat");

    {
        microStore::FileStore reader;
        auto fs = make_ram_fs();
        reader.init(fs, "/p");
        TEST_ASSERT_EQUAL(2u, reader.size());
        reader.clear();
        TEST_ASSERT_EQUAL(0u, reader.size());
    }

    // Re-init after clear; store must still be empty.
    microStore::FileStore reader2;
    auto fs2 = make_ram_fs();
    reader2.init(fs2, "/p");
    TEST_ASSERT_EQUAL(0u, reader2.size());
}

void test_file_store_exists() {
    reset_ram_fs();

    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "/p");
        writer.put("present", "v", /*ttl=*/0, /*ts=*/1u);
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "/p");

    TEST_ASSERT_TRUE(reader.exists("present"));
    TEST_ASSERT_FALSE(reader.exists("absent"));
}

void test_file_store_iterator() {
    reset_ram_fs();

    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "/p");
        writer.put("alpha", "AAA", /*ttl=*/0, /*ts=*/1u);
        writer.put("beta",  "BB",  /*ttl=*/0, /*ts=*/2u);
        writer.put("gamma", "C",   /*ttl=*/0, /*ts=*/3u);
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "/p");

    std::map<std::string, std::string> seen;
    for (auto& e : reader) {
        std::string k(e.key.begin(), e.key.end());
        std::string v(e.value.begin(), e.value.end());
        seen[k] = v;
    }

    TEST_ASSERT_EQUAL(3, (int)seen.size());
    TEST_ASSERT_EQUAL_STRING("AAA", seen["alpha"].c_str());
    TEST_ASSERT_EQUAL_STRING("BB",  seen["beta"].c_str());
    TEST_ASSERT_EQUAL_STRING("C",   seen["gamma"].c_str());
}

void test_file_store_iterator_traits() {
    using It = microStore::FileStore::iterator;
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
            microStore::FileStore::Entry
        >::value,
        "value_type must be Entry"
    );
    TEST_PASS();
}

// set_max_recs(N) on the reader (live-index) store evicts oldest on put.
void test_file_store_max_recs_put_eviction() {
    reset_ram_fs();

    static const char* keys[] = {"first", "second"};
    write_records_to_disk("/p", keys, 2, /*base_ts=*/1);
    // On disk: first(ts=1), second(ts=2)

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.set_max_recs(2);
    reader.init(fs, "/p");
    TEST_ASSERT_EQUAL(2u, reader.size());

    // A new put on the live-index store should evict "first" (oldest).
    reader.put("third", "v", /*ttl=*/0, microStore::time());
    TEST_ASSERT_EQUAL(2u, reader.size());
    TEST_ASSERT_FALSE(reader.exists("first"));
    TEST_ASSERT_TRUE(reader.exists("second"));
    TEST_ASSERT_TRUE(reader.exists("third"));
}

/* ===========================================================================
 * Additional FileStore-specific edge-case tests
 * =========================================================================== */

// exists() should honor TTL and evict an expired record.
void test_file_store_ttl_exists_expires() {
    reset_ram_fs();

    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "/p");
        writer.put("k", "hello", /*ttl=*/0, /*ts=*/1u);
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.set_ttl_secs(60);
    reader.init(fs, "/p");

    TEST_ASSERT_EQUAL(1u, reader.size());
    // exists() must detect expiry (same TTL logic as get()).
    TEST_ASSERT_FALSE(reader.exists("k"));
    TEST_ASSERT_EQUAL(0u, reader.size());
}

// compact() consolidates a store with multiple writes into a single live copy.
void test_file_store_compact_basic() {
    reset_ram_fs();

    uint32_t now = microStore::time() - 2;  // past: init() sweeps future-dated records
    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "/p");
        writer.put("a", "v1", /*ttl=*/0, now);
        writer.put("b", "v2", /*ttl=*/0, now);
        writer.put("a", "v3", /*ttl=*/0, now + 1);  // overwrite "a"
        writer.remove("b");              // delete "b"
        writer.put("c", "v4", /*ttl=*/0, now + 2);
    }
    remove_ram_file("/p_index.dat");

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "/p");

    TEST_ASSERT_EQUAL(2u, reader.size());  // a(v3) and c(v4) alive

    reader.compact();

    TEST_ASSERT_EQUAL(2u, reader.size());
    TEST_ASSERT_TRUE(reader.exists("a"));
    TEST_ASSERT_FALSE(reader.exists("b"));
    TEST_ASSERT_TRUE(reader.exists("c"));

    uint8_t buf[32]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_TRUE(reader.get("a", buf, &sz));
    TEST_ASSERT_EQUAL(2u, sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "v3", 2));
}

// A direct compact() call must leave the store writable. The active segment is
// one of the source segments compaction deletes, so compact() has to close it
// (LittleFS/FatFS refuse to unlink an open file) and reopen a segment before
// returning; otherwise records written afterwards go to a stale handle and are
// lost on reload.
void test_file_store_compact_keeps_store_writable() {
    reset_ram_fs();

    uint32_t now = microStore::time();
    {
        microStore::FileStore store;
        auto fs = make_ram_fs();
        store.init(fs, "/p");
        store.put("a", "v1", /*ttl=*/0, now);
        store.put("b", "v2", /*ttl=*/0, now);
        store.remove("b");
        TEST_ASSERT_TRUE(store.compact());
        store.put("c", "v3", /*ttl=*/0, now);   // written after compaction
        TEST_ASSERT_TRUE(store.exists("c"));
    }
    remove_ram_file("/p_index.dat");   // force a rebuild from the segment files

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "/p");

    TEST_ASSERT_EQUAL(2u, reader.size());   // a(v1) and c(v3)
    uint8_t buf[32]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_TRUE(reader.get("c", buf, &sz));
    TEST_ASSERT_EQUAL(2u, sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "v3", 2));
}

// Reload the store from its segment files only and check that `key` holds `val`.
static void assert_survives_rebuild(const char* key, const char* val) {
    remove_ram_file("/p_index.dat");   // force a rebuild from the segment files
    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "/p");
    uint8_t buf[32]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_TRUE(reader.get(key, buf, &sz));
    TEST_ASSERT_EQUAL(strlen(val), sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, val, sz));
}

// A direct compact() that fails before writing anything (compact.tmp cannot be
// opened) must still leave an active segment open for subsequent writes.
void test_file_store_compact_failure_keeps_store_writable() {
    reset_ram_fs();

    uint32_t now = microStore::time();
    {
        microStore::FileStore store;
        auto fs = make_ram_fs();
        store.init(fs, "/p");
        store.put("a", "v1", /*ttl=*/0, now);
        g_fail_open_path = "/p_compact.tmp";
        TEST_ASSERT_FALSE(store.compact());
        g_fail_open_path = nullptr;
        TEST_ASSERT_TRUE(store.put("c", "v3", /*ttl=*/0, now));
        TEST_ASSERT_TRUE(store.exists("c"));
    }
    assert_survives_rebuild("a", "v1");
    assert_survives_rebuild("c", "v3");
}

// A failed threshold-triggered compaction must leave the store writable. With
// max_recs=4 a single dead record crosses the 25% threshold, so every put and
// remove below attempts (and fails) a compaction.
void test_file_store_threshold_compact_failure_keeps_store_writable() {
    reset_ram_fs();

    uint32_t now = microStore::time();
    {
        microStore::FileStore store;
        store.set_max_recs(4);
        auto fs = make_ram_fs();
        store.init(fs, "/p");
        store.put("a", "v1", /*ttl=*/0, now);
        store.put("b", "v2", /*ttl=*/0, now);
        g_fail_open_path = "/p_compact.tmp";
        TEST_ASSERT_TRUE(store.remove("a"));                 // triggers a failed compaction
        TEST_ASSERT_TRUE(store.put("c", "v3", /*ttl=*/0, now));  // must still be writable
        TEST_ASSERT_TRUE(store.exists("c"));
        g_fail_open_path = nullptr;
    }
    assert_survives_rebuild("b", "v2");
    assert_survives_rebuild("c", "v3");
}

// When rotation runs out of segments it advances current_segment to
// segment_count before compacting. If that compaction fails, the store must
// reopen the last real segment and must not create seg<segment_count>.dat.
void test_file_store_rotation_compact_failure_no_stray_segment() {
    reset_ram_fs();

    uint32_t now = microStore::time();
    const char* val = "0123456789012345678901234567890123456789"
                      "0123456789012345678901234567890123456789";   // 80 bytes
    {
        microStore::FileStore store;
        auto fs = make_ram_fs();
        store.init(fs, "/p", /*clearOnInit=*/false, /*segment_size=*/256, /*segment_count=*/2);
        g_fail_write_path = "/p_compact.tmp";                // compaction aborts on first record
        char key[8];
        for (int i = 0; i < 8; i++) {
            snprintf(key, sizeof(key), "k%d", i);
            store.put(key, val, /*ttl=*/0, now);
        }
        TEST_ASSERT_TRUE(find_file("/p_seg1.dat") >= 0);    // rotation reached the last segment
        TEST_ASSERT_TRUE(find_file("/p_seg2.dat") < 0);     // no out-of-range segment created
        TEST_ASSERT_TRUE(find_file("/p_compact.tmp") < 0);  // aborted compaction cleaned up
        g_fail_write_path = nullptr;
        TEST_ASSERT_TRUE(store.put("z", "v9", /*ttl=*/0, now));
        TEST_ASSERT_TRUE(store.exists("z"));
    }
    TEST_ASSERT_TRUE(find_file("/p_seg2.dat") < 0);
    assert_survives_rebuild("z", "v9");
}

/* ===========================================================================
 * Compaction recovery tests
 *
 * These recreate the on-flash state left by a power loss or filesystem error
 * at specific points in compaction, then check that the data survives.
 * =========================================================================== */

static size_t ram_file_size(const char* name) {
    int idx = find_file(name);
    return idx < 0 ? 0 : g_files[idx].data.size();
}

static void write_ram_journal(uint32_t state, uint32_t next_seg, uint32_t tmp_valid_size) {
    microStore::Journal j;
    j.magic = microStore::JOURNAL_MAGIC; j.state = state;
    j.next_seg = next_seg; j.tmp_valid_size = tmp_valid_size;
    int idx = find_file("/p_journal.dat");
    if (idx < 0) { idx = alloc_slot(); strncpy(g_names[idx], "/p_journal.dat", 63); }
    g_files[idx].data.assign((uint8_t*)&j, (uint8_t*)&j + sizeof(j));
    g_files[idx].pos = 0;
}

static void assert_value(microStore::FileStore& store, const char* key, const char* val) {
    uint8_t buf[128]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_TRUE_MESSAGE(store.get(key, buf, &sz), key);
    TEST_ASSERT_EQUAL(strlen(val), sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, val, sz));
}

static const char* VAL80 = "0123456789012345678901234567890123456789"
                           "0123456789012345678901234567890123456789";

// Power loss after finalize_compaction() renamed compact.tmp to seg0 but before
// the COMMIT journal was cleared. Boot recovery must not finalize a second time:
// that would delete the freshly compacted seg0 and find no compact.tmp to rename.
void test_recovery_commit_journal_after_swap_keeps_data() {
    reset_ram_fs();

    uint32_t now = microStore::time();
    {
        microStore::FileStore store;
        auto fs = make_ram_fs();
        store.init(fs, "/p");
        store.put("a", "v1", /*ttl=*/0, now);
        store.put("b", "v2", /*ttl=*/0, now);
        store.remove("b");
        TEST_ASSERT_TRUE(store.compact());
    }
    write_ram_journal(microStore::JOURNAL_COMMIT, 0, 0);   // journal never cleared

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "/p");
    TEST_ASSERT_TRUE(find_file("/p_journal.dat") < 0);
    TEST_ASSERT_EQUAL(1u, reader.size());
    assert_value(reader, "a", "v1");
}

// If renaming compact.tmp to seg0 fails after the source segments are deleted,
// compact.tmp is the only copy of the data. It must be kept, compact() must
// report failure, no further compaction may overwrite it, and the next boot
// must complete the swap.
void test_recovery_finalize_rename_failure_keeps_data() {
    reset_ram_fs();

    uint32_t now = microStore::time();
    {
        microStore::FileStore store;
        auto fs = make_ram_fs();
        store.init(fs, "/p");
        store.put("a", "v1", /*ttl=*/0, now);
        store.put("b", "v2", /*ttl=*/0, now);
        g_fail_rename = true;
        TEST_ASSERT_FALSE(store.compact());
        TEST_ASSERT_TRUE(find_file("/p_compact.tmp") >= 0);
        TEST_ASSERT_TRUE(find_file("/p_journal.dat") >= 0);
        size_t tmp_size = ram_file_size("/p_compact.tmp");
        TEST_ASSERT_FALSE(store.compact());                 // refused: recovery pending
        TEST_ASSERT_EQUAL(tmp_size, ram_file_size("/p_compact.tmp"));
        g_fail_rename = false;
    }

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "/p");
    TEST_ASSERT_TRUE(find_file("/p_compact.tmp") < 0);
    TEST_ASSERT_TRUE(find_file("/p_journal.dat") < 0);
    TEST_ASSERT_EQUAL(2u, reader.size());
    assert_value(reader, "a", "v1");
    assert_value(reader, "b", "v2");
}

// Builds a store whose compaction aborts mid-way: seg0 holds a dead copy of "a"
// and a live "b"; seg1 holds "c" and the live "a". compact.tmp has room for
// exactly one record, so seg0 is committed and deleted, then seg1 fails.
// Because "a" is dead in seg0, "b" moves from offset R in seg0 to offset 0 in
// compact.tmp, so a stale index would point at the wrong record.
static void put_midway_abort_records(microStore::FileStore& store) {
    uint32_t now = microStore::time();
    store.put("a", VAL80, /*ttl=*/0, now);
    store.put("b", "vb" , /*ttl=*/0, now);
    store.put("c", VAL80, /*ttl=*/0, now);
    store.put("a", "va" , /*ttl=*/0, now);
    TEST_ASSERT_TRUE(find_file("/p_seg1.dat") >= 0);
    TEST_ASSERT_TRUE(find_file("/p_seg2.dat") < 0);
    g_fail_write_path = "/p_compact.tmp";
    g_write_budget    = ram_file_size("/p_seg0.dat") - (sizeof(microStore::RecordHeader) + 1 + 80 + sizeof(microStore::RecordCommit));
}

static void assert_midway_records(microStore::FileStore& store) {
    assert_value(store, "a", "va");
    assert_value(store, "b", "vb");
    assert_value(store, "c", VAL80);
}

// A mid-way abort in a running store must recover immediately: records from the
// deleted segments stay readable, and no journal or compact.tmp is left behind
// for a later compaction attempt to truncate.
void test_recovery_midway_abort_recovers_in_session() {
    reset_ram_fs();

    uint32_t now = microStore::time();
    {
        microStore::FileStore store;
        auto fs = make_ram_fs();
        store.init(fs, "/p", /*clearOnInit=*/false, /*segment_size=*/256, /*segment_count=*/4);
        put_midway_abort_records(store);
        TEST_ASSERT_FALSE(store.compact());
        TEST_ASSERT_TRUE(find_file("/p_seg0.dat") >= 0);     // compact.tmp renamed into place
        TEST_ASSERT_TRUE(find_file("/p_compact.tmp") < 0);
        TEST_ASSERT_TRUE(find_file("/p_journal.dat") < 0);
        g_fail_write_path = nullptr;
        // Write before reading: the RAM fs shares one position/mode per file, so
        // a read handle on the active segment would disturb later appends.
        TEST_ASSERT_TRUE(store.put("z", "vz", /*ttl=*/0, now));
        assert_midway_records(store);                        // readable without a reboot
        assert_value(store, "z", "vz");
    }

    microStore::FileStore reader;                            // persistent index intact
    auto fs = make_ram_fs();
    reader.init(fs, "/p", /*clearOnInit=*/false, /*segment_size=*/256, /*segment_count=*/4);
    TEST_ASSERT_EQUAL(4u, reader.size());
    assert_midway_records(reader);
    assert_value(reader, "z", "vz");
}

// If the mid-way abort cannot be recovered in session (rename fails), the
// journal and compact.tmp must be left for boot, no further compaction may
// truncate compact.tmp, and boot recovery must rebuild the index rather than
// trust the persistent index, whose seg0 offsets predate the compaction.
void test_recovery_midway_abort_recovers_at_boot() {
    reset_ram_fs();

    uint32_t now = microStore::time();
    {
        microStore::FileStore store;
        auto fs = make_ram_fs();
        store.init(fs, "/p", /*clearOnInit=*/false, /*segment_size=*/256, /*segment_count=*/4);
        put_midway_abort_records(store);
        g_fail_rename = true;
        TEST_ASSERT_FALSE(store.compact());
        g_fail_write_path = nullptr;
        TEST_ASSERT_TRUE(find_file("/p_compact.tmp") >= 0);
        TEST_ASSERT_TRUE(find_file("/p_journal.dat") >= 0);
        size_t tmp_size = ram_file_size("/p_compact.tmp");
        TEST_ASSERT_FALSE(store.compact());                 // refused: recovery pending
        TEST_ASSERT_EQUAL(tmp_size, ram_file_size("/p_compact.tmp"));
        TEST_ASSERT_TRUE(store.put("z", "vz", /*ttl=*/0, now));
        g_fail_rename = false;
    }

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "/p", /*clearOnInit=*/false, /*segment_size=*/256, /*segment_count=*/4);
    TEST_ASSERT_TRUE(find_file("/p_compact.tmp") < 0);
    TEST_ASSERT_TRUE(find_file("/p_journal.dat") < 0);
    TEST_ASSERT_EQUAL(4u, reader.size());
    assert_midway_records(reader);
    assert_value(reader, "z", "vz");
}

/* ===========================================================================
 * Directory-mode prefix tests
 *
 * When the prefix passed to init() ends with '/', FileStore should treat it as
 * a directory: generated files become "{prefix}0.dat", "{prefix}index.dat",
 * "{prefix}journal.dat", "{prefix}compact.tmp" (no leading '_'). When the
 * prefix does NOT end with '/', the legacy '_' separator must still be used.
 * =========================================================================== */

// Directory-mode prefix produces files with '/' as the separator.
void test_dir_prefix_segment_and_index_names() {
    reset_ram_fs();

    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "sub/");
        writer.put("a", "hi", /*ttl=*/0, microStore::time());
    }

    TEST_ASSERT_TRUE(find_file("sub/seg0.dat") >= 0);
    TEST_ASSERT_TRUE(find_file("sub/index.dat") >= 0);
    // Underscore-mode names must NOT exist.
    TEST_ASSERT_TRUE(find_file("sub_seg0.dat") < 0);
    TEST_ASSERT_TRUE(find_file("sub_index.dat") < 0);
}

// Legacy (non-directory) prefix behavior is preserved exactly.
void test_legacy_prefix_unchanged() {
    reset_ram_fs();

    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "leg");
        writer.put("a", "hi", /*ttl=*/0, microStore::time());
    }

    TEST_ASSERT_TRUE(find_file("leg_seg0.dat") >= 0);
    TEST_ASSERT_TRUE(find_file("leg_index.dat") >= 0);
    TEST_ASSERT_TRUE(find_file("leg/seg0.dat") < 0);
}

// Re-init on a directory-mode store reloads the persisted index and prior
// records read back correctly. Covers load_index() name resolution.
void test_dir_prefix_reload_index() {
    reset_ram_fs();

    uint32_t now = microStore::time() - 2;  // past: init() sweeps future-dated records
    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "sub/");
        writer.put("a", "v1", /*ttl=*/0, now);
        writer.put("b", "v2", /*ttl=*/0, now + 1);
    }

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "sub/");
    TEST_ASSERT_EQUAL(2u, reader.size());

    uint8_t buf[8]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_TRUE(reader.get("a", buf, &sz));
    TEST_ASSERT_EQUAL(2u, sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "v1", 2));
}

// Compaction with a directory-mode prefix uses the directory-scoped
// compact.tmp path and leaves the post-compaction layout under the directory.
void test_dir_prefix_compact() {
    reset_ram_fs();

    uint32_t now = microStore::time() - 2;  // past: init() sweeps future-dated records
    {
        microStore::FileStore writer;
        auto fs = make_ram_fs();
        writer.init(fs, "sub/");
        writer.put("a", "v1", /*ttl=*/0, now);
        writer.put("a", "v2", /*ttl=*/0, now + 1);   // overwrite
        writer.put("b", "v3", /*ttl=*/0, now + 2);
    }

    microStore::FileStore reader;
    auto fs = make_ram_fs();
    reader.init(fs, "sub/");
    TEST_ASSERT_EQUAL(2u, reader.size());

    TEST_ASSERT_TRUE(reader.compact());

    // compact.tmp must have been removed after finalize.
    TEST_ASSERT_TRUE(find_file("sub/compact.tmp") < 0);
    TEST_ASSERT_TRUE(find_file("sub_compact.tmp") < 0);
    // Records still readable post-compact.
    TEST_ASSERT_TRUE(reader.exists("a"));
    TEST_ASSERT_TRUE(reader.exists("b"));

    uint8_t buf[8]; uint16_t sz = sizeof(buf);
    TEST_ASSERT_TRUE(reader.get("a", buf, &sz));
    TEST_ASSERT_EQUAL(2u, sz);
    TEST_ASSERT_EQUAL(0, memcmp(buf, "v2", 2));
}

/* ---- Main ---- */

void setUp()    {}
void tearDown() {}

int runUnityTests(void) {
    UNITY_BEGIN();
    // TTL tests
    RUN_TEST(test_ttl_get_expires_old_record);
    RUN_TEST(test_ttl_get_keeps_fresh_record);
    RUN_TEST(test_ttl_compact_removes_expired);
    // Max-records tests
    RUN_TEST(test_max_recs_init_prunes_oldest);
    RUN_TEST(test_max_recs_pruned_index_persists_across_reboot);
    RUN_TEST(test_max_recs_compact_prunes_oldest);
    // Basic CRUD (ported from test_heap_store)
    RUN_TEST(test_file_store_put_get);
    RUN_TEST(test_file_store_overwrite);
    RUN_TEST(test_file_store_remove);
    RUN_TEST(test_file_store_size);
    RUN_TEST(test_file_store_clear);
    RUN_TEST(test_file_store_exists);
    RUN_TEST(test_file_store_iterator);
    RUN_TEST(test_file_store_iterator_traits);
    RUN_TEST(test_file_store_max_recs_put_eviction);
    // Additional edge-case tests
    RUN_TEST(test_file_store_ttl_exists_expires);
    RUN_TEST(test_file_store_compact_basic);
    RUN_TEST(test_file_store_compact_keeps_store_writable);
    RUN_TEST(test_file_store_compact_failure_keeps_store_writable);
    RUN_TEST(test_file_store_threshold_compact_failure_keeps_store_writable);
    RUN_TEST(test_file_store_rotation_compact_failure_no_stray_segment);
    // Compaction recovery
    RUN_TEST(test_recovery_commit_journal_after_swap_keeps_data);
    RUN_TEST(test_recovery_finalize_rename_failure_keeps_data);
    RUN_TEST(test_recovery_midway_abort_recovers_in_session);
    RUN_TEST(test_recovery_midway_abort_recovers_at_boot);
    // Directory-mode prefix
    RUN_TEST(test_dir_prefix_segment_and_index_names);
    RUN_TEST(test_legacy_prefix_unchanged);
    RUN_TEST(test_dir_prefix_reload_index);
    RUN_TEST(test_dir_prefix_compact);
    return UNITY_END();
}

// For native dev-platform or for some embedded frameworks
int main(void) {
	return runUnityTests();
}

#ifdef ARDUINO
// For Arduino framework
void setup() {
	// Wait ~2 seconds before the Unity test runner
	// establishes connection with a board Serial interface
	delay(2000);

	runUnityTests();
}
void loop() {}
#endif

// For ESP-IDF framework
void app_main() {
	runUnityTests();
}
