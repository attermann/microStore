/*
 * Tests for the POSIX and stdio filesystem adapters against a real
 * temporary directory.
 */

#include <unity.h>
#include <microStore/Adapters/PosixFileSystem.h>
#include <microStore/Adapters/StdioFileSystem.h>

#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static std::string tmp;

static bool is_dir(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/* ---- rmdir ---- */

static void check_rmdir(microStore::FileSystem fs) {
    const std::string dir = tmp + "/sub";
    TEST_ASSERT_TRUE(fs.mkdir(dir.c_str()));
    TEST_ASSERT_TRUE(fs.rmdir(dir.c_str()));
    TEST_ASSERT_FALSE(is_dir(dir));
    TEST_ASSERT_FALSE(fs.rmdir(dir.c_str()));
}

void test_posix_rmdir_reports_result() {
    check_rmdir(microStore::Adapters::PosixFileSystem());
}

void test_stdio_rmdir_reports_result() {
    check_rmdir(microStore::Adapters::StdioFileSystem());
}

/* ---- Main ---- */

void setUp() {
    char tmpl[] = "/tmp/microstore_fs_XXXXXX";
    TEST_ASSERT_NOT_NULL(::mkdtemp(tmpl));
    tmp = tmpl;
}

void tearDown() {
    std::string cmd = "rm -rf '" + tmp + "'";
    (void)std::system(cmd.c_str());
}

int runUnityTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_posix_rmdir_reports_result);
    RUN_TEST(test_stdio_rmdir_reports_result);
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
