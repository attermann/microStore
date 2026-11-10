/*
 * Tests for the POSIX and stdio filesystem adapters against a real
 * temporary directory, including base-path / relative-path resolution.
 *
 * Path model: every path is relative to the adapter's root ("/a", "./a" and
 * "a" are the same file); the base path says where the root is, and an empty
 * base means the current directory.
 */

#include <unity.h>
#include <microStore/Adapters/PosixFileSystem.h>
#include <microStore/Adapters/StdioFileSystem.h>
#include <microStore/FileStore.h>

#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using microStore::FileSystem;
using microStore::Adapters::PosixFileSystem;
using microStore::Adapters::StdioFileSystem;

static std::string tmp;
static char saved_cwd[1024];

typedef FileSystem (*MakeFs)(const char* base);
static FileSystem make_posix(const char* base) { return PosixFileSystem(base); }
static FileSystem make_stdio(const char* base) { return StdioFileSystem(base); }
static FileSystem make_posix_default(const char*) { return PosixFileSystem(); }
static FileSystem make_stdio_default(const char*) { return StdioFileSystem(); }

static bool is_dir(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0 && !is_dir(path);
}

static void write_file(FileSystem& fs, const char* path) {
    microStore::File f = fs.open(path, microStore::File::ModeWrite, true);
    TEST_ASSERT_TRUE_MESSAGE((bool)f, path);
    TEST_ASSERT_EQUAL(4, f.write("test", 4));
    f.close();
}

/* ---- rmdir (base "/" gives the host root, so absolute host paths work) ---- */

static void check_rmdir(MakeFs make) {
    FileSystem fs = make("/");
    const std::string dir = tmp + "/sub";
    TEST_ASSERT_TRUE(fs.mkdir(dir.c_str()));
    TEST_ASSERT_TRUE(fs.rmdir(dir.c_str()));
    TEST_ASSERT_FALSE(is_dir(dir));
    TEST_ASSERT_FALSE(fs.rmdir(dir.c_str()));
}

void test_posix_rmdir_reports_result() { check_rmdir(make_posix); }
void test_stdio_rmdir_reports_result() { check_rmdir(make_stdio); }

/* ---- Every spelling of a path lands under the base, through every method ---- */

static void check_paths_under_base(MakeFs make, const char* dir, const char* file, const char* moved) {
    FileSystem fs = make(tmp.c_str());
    TEST_ASSERT_TRUE(fs.mkdir(dir));
    TEST_ASSERT_TRUE(is_dir(tmp + "/kv"));
    TEST_ASSERT_TRUE(fs.isDirectory(dir));

    write_file(fs, file);
    TEST_ASSERT_TRUE(is_file(tmp + "/kv/a"));
    TEST_ASSERT_TRUE(fs.exists(file));
    TEST_ASSERT_EQUAL(4, fs.size(file));
    TEST_ASSERT_EQUAL(1, fs.listDirectory(dir).size());

    TEST_ASSERT_TRUE(fs.rename(file, moved));
    TEST_ASSERT_TRUE(is_file(tmp + "/kv/b"));
    TEST_ASSERT_FALSE(is_file(tmp + "/kv/a"));
    TEST_ASSERT_TRUE(fs.remove(moved));
    TEST_ASSERT_FALSE(fs.exists(moved));
    TEST_ASSERT_TRUE(fs.rmdir(dir));
    TEST_ASSERT_FALSE(is_dir(tmp + "/kv"));
}

static void check_all_spellings(MakeFs make) {
    check_paths_under_base(make, "/kv", "/kv/a", "/kv/b");
    check_paths_under_base(make, "./kv/", "./kv/a", "./kv/b");
    check_paths_under_base(make, "kv", "kv/a", "kv//b");
}

void test_posix_paths_resolve_under_base() { check_all_spellings(make_posix); }
void test_stdio_paths_resolve_under_base() { check_all_spellings(make_stdio); }

/* ---- Empty (default) base: root is the current directory ---- */

static void check_default_base_is_cwd(MakeFs make) {
    TEST_ASSERT_EQUAL(0, ::chdir(tmp.c_str()));
    FileSystem fs = make("");
    write_file(fs, "./rel");
    write_file(fs, "/abs");
    write_file(fs, "bare");
    TEST_ASSERT_EQUAL(0, ::chdir(saved_cwd));
    TEST_ASSERT_TRUE(is_file(tmp + "/rel"));
    TEST_ASSERT_TRUE(is_file(tmp + "/abs"));
    TEST_ASSERT_TRUE(is_file(tmp + "/bare"));
}

void test_posix_default_base_is_cwd() { check_default_base_is_cwd(make_posix_default); }
void test_stdio_default_base_is_cwd() { check_default_base_is_cwd(make_stdio_default); }

/* ---- Base "/" reaches the host root (and a trailing "/" on a base is ignored) ---- */

static void check_host_root(MakeFs make) {
    FileSystem root = make("/");
    const std::string abs = tmp + "/host_abs";
    write_file(root, abs.c_str());
    TEST_ASSERT_TRUE(is_file(abs));
    TEST_ASSERT_TRUE(root.exists(abs.c_str()));

    const std::string slashed = tmp + "/";
    FileSystem fs = make(slashed.c_str());
    write_file(fs, "trailing");
    TEST_ASSERT_TRUE(is_file(tmp + "/trailing"));
}

void test_posix_host_root() { check_host_root(make_posix); }
void test_stdio_host_root() { check_host_root(make_stdio); }

/* ---- The caller's base string is copied, never kept ---- */

static void check_base_copied(MakeFs make) {
    std::string* base = new std::string(tmp);
    FileSystem fs = make(base->c_str());
    base->assign(base->size(), 'x');
    delete base;
    write_file(fs, "./copied");
    TEST_ASSERT_TRUE(is_file(tmp + "/copied"));
}

void test_posix_base_copied() { check_base_copied(make_posix); }
void test_stdio_base_copied() { check_base_copied(make_stdio); }

/* ---- Limits: too-long base fails init and every op; too-long path fails the op ---- */

static void check_limits(MakeFs make) {
    std::string long_base(USTORE_MAX_BASEPATH_LEN, 'b');
    FileSystem bad = make(long_base.c_str());
    TEST_ASSERT_FALSE(bad.init(false));
    TEST_ASSERT_FALSE(bad.exists("x"));
    TEST_ASSERT_FALSE((bool)bad.open("x", microStore::File::ModeWrite, true));

    FileSystem fs = make(tmp.c_str());
    // Resolved length is tmp + "/" + name; make it exactly one too many.
    std::string name(USTORE_MAX_PATH_LEN - tmp.size() - 1, 'n');
    TEST_ASSERT_FALSE((bool)fs.open(name.c_str(), microStore::File::ModeWrite, true));
    name.pop_back();
    write_file(fs, name.c_str());
    TEST_ASSERT_TRUE(is_file(tmp + "/" + name));
}

void test_posix_limits() { check_limits(make_posix); }
void test_stdio_limits() { check_limits(make_stdio); }

/* ---- init() write check and a FileStore both live under the base ---- */

static void check_store_under_base(MakeFs make, const char* prefix) {
    FileSystem fs = make(tmp.c_str());
    TEST_ASSERT_TRUE(fs.init(true));
    TEST_ASSERT_FALSE(is_file(tmp + "/__init_test__"));
    microStore::FileStore store;
    TEST_ASSERT_TRUE(store.init(fs, prefix));
    TEST_ASSERT_TRUE(store.put("k", (const uint8_t*)"v", 1));
    store.close();
    TEST_ASSERT_TRUE(is_dir(tmp + "/kv"));
    TEST_ASSERT_TRUE(is_file(tmp + "/kv/seg0.dat"));
}

void test_posix_store_under_base() {
    check_store_under_base(make_posix, "/kv/");
}
void test_stdio_store_under_base() {
    check_store_under_base(make_stdio, "./kv/");
}

/* ---- Main ---- */

void setUp() {
    char tmpl[] = "/tmp/microstore_fs_XXXXXX";
    TEST_ASSERT_NOT_NULL(::mkdtemp(tmpl));
    tmp = tmpl;
}

void tearDown() {
    (void)::chdir(saved_cwd);
    std::string cmd = "rm -rf '" + tmp + "'";
    (void)std::system(cmd.c_str());
}

int runUnityTests(void) {
    TEST_ASSERT_NOT_NULL(::getcwd(saved_cwd, sizeof(saved_cwd)));
    UNITY_BEGIN();
    RUN_TEST(test_posix_rmdir_reports_result);
    RUN_TEST(test_stdio_rmdir_reports_result);
    RUN_TEST(test_posix_paths_resolve_under_base);
    RUN_TEST(test_stdio_paths_resolve_under_base);
    RUN_TEST(test_posix_default_base_is_cwd);
    RUN_TEST(test_stdio_default_base_is_cwd);
    RUN_TEST(test_posix_host_root);
    RUN_TEST(test_stdio_host_root);
    RUN_TEST(test_posix_base_copied);
    RUN_TEST(test_stdio_base_copied);
    RUN_TEST(test_posix_limits);
    RUN_TEST(test_stdio_limits);
    RUN_TEST(test_posix_store_under_base);
    RUN_TEST(test_stdio_store_under_base);
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
