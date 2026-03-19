#pragma once

//#if defined(USTORE_USE_NOOPFS)

#include "../File.h"
#include "../FileSystem.h"

namespace microStoreImpl {

class NoopFileSystemImpl : public microStore::FileSystemImpl {

protected:

	class FileImpl : public microStore::FileImpl {

	public:
		FileImpl() : microStore::FileImpl() {}
		virtual ~FileImpl() {}

	public:
		inline virtual const char* name() const { return nullptr; }
		inline virtual size_t size() const { return 0; }
		inline virtual void close() {}

		inline virtual int read() { return 0; }
		inline virtual size_t write(uint8_t ch) {  return 0; }
		inline virtual size_t read(uint8_t* buffer, size_t size) {  return 0; }
		inline virtual size_t write(const uint8_t* buffer, size_t size) {  return 0; }
		inline virtual int available() {  return 0; }
		inline virtual int peek() {  return 0; }
		inline virtual size_t tell() {  return 0; }
		inline virtual long seek(uint32_t pos, microStore::SeekMode mode) { return 0; }
		inline virtual void flush() {}

		inline virtual bool isValid() const { return false; }

	};

public:
	NoopFileSystemImpl() {}

public:

	inline virtual bool format() override { return false; }
	inline virtual bool init() override { return false; }

	inline virtual microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override { return {}; }
	inline virtual bool exists(const char* path) override { return false; }
	inline virtual bool remove(const char* path) override { return false; }
	inline virtual bool rename(const char* from_path, const char* to_path) override { return false; }
	inline virtual bool mkdir(const char* path) override { return false; }
	inline virtual bool rmdir(const char* path) override { return false; }

	inline virtual bool isDirectory(const char* path) override { return false; }
	inline virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) override { return std::list<std::string>(); }
	inline virtual size_t storageSize() override { return 0; }
	inline virtual size_t storageAvailable() override { return 0; }

};

}

//#endif
