/*
 * Unit tests for microStore::Path, the allocation-free path resolution shared
 * by all FileSystem adapters.
 */

#include <unity.h>
#include <microStore/Path.h>

#include <string>

using namespace microStore;

static char buf[USTORE_MAX_PATH_LEN];

static void expect_norm(const char* in, const char* want) {
    const char* got = Path::normalize(in, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL_MESSAGE(got, in ? in : "(null)");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got, in ? in : "(null)");
}

static void expect_join(const char* base, const char* in, const char* want) {
    const char* got = Path::join(base, in, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL_MESSAGE(got, in ? in : "(null)");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got, in ? in : "(null)");
}

void test_normalize_spellings_are_equivalent() {
    expect_norm("x", "/x");
    expect_norm("./x", "/x");
    expect_norm("/x", "/x");
    expect_norm("//x", "/x");
    expect_norm("././x", "/x");
    expect_norm(".//x", "/x");
    expect_norm("a//b///c", "/a/b/c");
    expect_norm("./kv/seg0.dat", "/kv/seg0.dat");
}

void test_normalize_root() {
    expect_norm(nullptr, "/");
    expect_norm("", "/");
    expect_norm(".", "/");
    expect_norm("./", "/");
    expect_norm("/", "/");
    expect_norm("//", "/");
}

void test_normalize_keeps_trailing_slash() {
    expect_norm("./kv/", "/kv/");
    expect_norm("kv//", "/kv/");
}

void test_normalize_does_not_interpret_dotdot() {
    expect_norm("../x", "/../x");
    expect_norm("a/./b", "/a/./b");
    expect_norm(".hidden", "/.hidden");
}

void test_normalize_canonical_is_not_copied() {
    const char* in = "/kv/seg0.dat";
    TEST_ASSERT_EQUAL_PTR(in, Path::normalize(in, buf, sizeof(buf)));
    const char* rel = "./kv/seg0.dat";
    TEST_ASSERT_EQUAL_PTR(buf, Path::normalize(rel, buf, sizeof(buf)));
}

void test_normalize_overflow() {
    char small[8];
    TEST_ASSERT_NOT_NULL(Path::normalize("abcdef", small, sizeof(small)));   // "/abcdef" + NUL = 8
    TEST_ASSERT_EQUAL_STRING("/abcdef", small);
    TEST_ASSERT_NULL(Path::normalize("abcdefg", small, sizeof(small)));
    // Exactly at USTORE_MAX_PATH_LEN.
    std::string fits(USTORE_MAX_PATH_LEN - 2, 'f');
    TEST_ASSERT_NOT_NULL(Path::normalize(fits.c_str(), buf, sizeof(buf)));
    std::string over(USTORE_MAX_PATH_LEN - 1, 'o');
    TEST_ASSERT_NULL(Path::normalize(over.c_str(), buf, sizeof(buf)));
}

void test_join() {
    expect_join(".", "./x", "./x");
    expect_join(".", "/x", "./x");
    expect_join(".", "x", "./x");
    expect_join(".", "./", "./");
    expect_join(".", nullptr, "./");
    expect_join("/", "/tmp/x", "/tmp/x");
    expect_join("/", "./x", "/x");
    expect_join("/var/app", "/kv/", "/var/app/kv/");
    expect_join("/var/app/", "kv", "/var/app/kv");
    expect_join("/var/app//", "//kv", "/var/app/kv");
    expect_join("", "./x", "/x");
    expect_join(nullptr, "x", "/x");
    expect_join("data", "x", "data/x");
}

void test_join_overflow() {
    char small[8];
    TEST_ASSERT_NOT_NULL(Path::join("/ab", "cd", small, sizeof(small)));  // "/ab/cd" + NUL = 7
    TEST_ASSERT_NULL(Path::join("/abcd", "efg", small, sizeof(small)));
    TEST_ASSERT_NULL(Path::join("/abcdefgh", "x", small, sizeof(small)));
}

void test_copy_base() {
    char dst[USTORE_MAX_BASEPATH_LEN];
    TEST_ASSERT_TRUE(Path::copy_base(dst, sizeof(dst), nullptr));
    TEST_ASSERT_EQUAL_STRING("", dst);
    TEST_ASSERT_TRUE(Path::copy_base(dst, sizeof(dst), "/"));
    TEST_ASSERT_EQUAL_STRING("/", dst);
    TEST_ASSERT_TRUE(Path::copy_base(dst, sizeof(dst), "//"));
    TEST_ASSERT_EQUAL_STRING("/", dst);
    TEST_ASSERT_TRUE(Path::copy_base(dst, sizeof(dst), "/littlefs/"));
    TEST_ASSERT_EQUAL_STRING("/littlefs", dst);
    std::string fits(USTORE_MAX_BASEPATH_LEN - 1, 'b');
    TEST_ASSERT_TRUE(Path::copy_base(dst, sizeof(dst), fits.c_str()));
    std::string over(USTORE_MAX_BASEPATH_LEN, 'b');
    TEST_ASSERT_FALSE(Path::copy_base(dst, sizeof(dst), over.c_str()));
    TEST_ASSERT_EQUAL_STRING("", dst);
}

void test_persistent_mount() {
    bool conflict = true;
    const char* m = Path::persistent_mount("/littlefs", &conflict);
    TEST_ASSERT_EQUAL_STRING("/littlefs", m);
    TEST_ASSERT_FALSE(conflict);
    // Same storage every time; a different base keeps the first mount.
    TEST_ASSERT_EQUAL_PTR(m, Path::persistent_mount("/littlefs", &conflict));
    TEST_ASSERT_FALSE(conflict);
    TEST_ASSERT_EQUAL_PTR(m, Path::persistent_mount("/other", &conflict));
    TEST_ASSERT_TRUE(conflict);
    TEST_ASSERT_EQUAL_STRING("/littlefs", m);
}

void setUp() {}
void tearDown() {}

int runUnityTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_normalize_spellings_are_equivalent);
    RUN_TEST(test_normalize_root);
    RUN_TEST(test_normalize_keeps_trailing_slash);
    RUN_TEST(test_normalize_does_not_interpret_dotdot);
    RUN_TEST(test_normalize_canonical_is_not_copied);
    RUN_TEST(test_normalize_overflow);
    RUN_TEST(test_join);
    RUN_TEST(test_join_overflow);
    RUN_TEST(test_copy_base);
    RUN_TEST(test_persistent_mount);
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
