#include <unity.h>
#include <microStore/FileSystem.hpp>
#include <microStore/impl/PosixFileSystemImpl.hpp>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char* TEST_FILE = "/tmp/test_file_available.bin";

static microStore::File open_test_file(microStore::File::Mode mode) {
    microStoreImpl::PosixFileSystemImpl fs;
    return fs.open(TEST_FILE, mode);
}

// Create a file with known content (10 bytes) before each test
static void create_test_file() {
    FILE* f = ::fopen(TEST_FILE, "wb");
    const uint8_t data[10] = {0,1,2,3,4,5,6,7,8,9};
    ::fwrite(data, 1, 10, f);
    ::fclose(f);
}

void setUp() {
    create_test_file();
}

void tearDown() {
    ::unlink(TEST_FILE);
}

void test_available_initial() {
    microStore::File f = open_test_file(microStore::File::ModeRead);
    TEST_ASSERT_TRUE(f);
    TEST_ASSERT_EQUAL(10, f.available());
    f.close();
}

void test_available_after_read() {
    microStore::File f = open_test_file(microStore::File::ModeRead);
    TEST_ASSERT_TRUE(f);
    uint8_t buf[3];
    f.read(buf, 3);
    TEST_ASSERT_EQUAL(7, f.available());
    f.close();
}

void test_available_after_seek_set() {
    microStore::File f = open_test_file(microStore::File::ModeRead);
    TEST_ASSERT_TRUE(f);
    f.seek(5, microStore::SeekModeSet);
    TEST_ASSERT_EQUAL(5, f.available());
    f.close();
}

void test_available_after_seek_end() {
    microStore::File f = open_test_file(microStore::File::ModeRead);
    TEST_ASSERT_TRUE(f);
    f.seek(0, microStore::SeekModeEnd);
    TEST_ASSERT_EQUAL(0, f.available());
    f.close();
}

void test_available_after_seek_cur() {
    microStore::File f = open_test_file(microStore::File::ModeRead);
    TEST_ASSERT_TRUE(f);
    f.seek(3, microStore::SeekModeCur);
    TEST_ASSERT_EQUAL(7, f.available());
    f.close();
}

void test_available_after_write_extend() {
    // Create an empty file, write 10 bytes — should be at new EOF
    ::unlink(TEST_FILE);
    microStore::File f = open_test_file(microStore::File::ModeReadWrite);
    TEST_ASSERT_TRUE(f);
    const uint8_t data[10] = {0,1,2,3,4,5,6,7,8,9};
    f.write(data, 10);
    TEST_ASSERT_EQUAL(0, f.available());
    f.close();
}

void test_available_after_write_overwrite() {
    // File has 10 bytes. Seek to start, read 4, write 3 in the middle.
    // Position becomes 7, file stays 10 bytes → available == 3.
    microStore::File f = open_test_file(microStore::File::ModeReadAppend);
    TEST_ASSERT_TRUE(f);
    // ModeReadAppend opens with O_RDWR|O_CREAT|O_APPEND; seek back to start
    f.seek(0, microStore::SeekModeSet);
    uint8_t buf[4];
    f.read(buf, 4);                          // position = 4, available = 6
    const uint8_t patch[3] = {0xAA, 0xBB, 0xCC};
    f.write(patch, 3);                       // position = 7, available = 3
    TEST_ASSERT_EQUAL(3, f.available());
    f.close();
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_available_initial);
    RUN_TEST(test_available_after_read);
    RUN_TEST(test_available_after_seek_set);
    RUN_TEST(test_available_after_seek_end);
    RUN_TEST(test_available_after_seek_cur);
    RUN_TEST(test_available_after_write_extend);
    RUN_TEST(test_available_after_write_overwrite);
    return UNITY_END();
}
