#pragma once

#if defined(MICROSTORE_USE_INTERNALFS)

#include "../File.hpp"
#include "../FileSystem.hpp"

#include <InternalFileSystem.h>
#define FS InternalFS
using namespace Adafruit_LittleFS_Namespace;

namespace microStoreImpl {

class InternalFSFileSystemImpl : public microStore::FileSystemImpl {

protected:

	class FileImpl : public microStore::FileImpl {

	private:
		std::unique_ptr<File> _file;
		bool _closed = false;

	public:
		FileImpl(File* file) : microStore::FileImpl(), _file(file) {}
		virtual ~FileImpl() { if (!_closed) close(); }

	public:
		inline virtual const char* name() const { return _file->name(); }
		inline virtual size_t size() const { return _file->size(); }
		inline virtual void close() { _file->close(); _closed = true; }

		inline virtual int read() { return _file->read(); }
		inline virtual size_t write(uint8_t ch) { return _file->write(ch); }
		inline virtual size_t read(uint8_t* buffer, size_t size) { return _file->read((uint8_t*)buffer, size); }
		inline virtual size_t write(const uint8_t* buffer, size_t size) { return _file->write(buffer, size); }

		inline virtual int available() { return _file->available(); }
		inline virtual int peek() { return _file->peek(); }
		inline virtual size_t tell() { return _file->position(); }
		inline virtual long seek(uint32_t pos, microStore::SeekMode mode) {
			uint8_t smode;
			switch (mode) {
				case microStore::SeekMode::SeekModeCur:
					smode = SEEK_O_CUR;
					break;
				case microStore::SeekMode::SeekModeEnd:
					smode = SEEK_O_END;
					break;
				case microStore::SeekMode::SeekModeSet:
				default:
					smode = SEEK_O_SET;
					break;
			}
			return _file->seek(pos, smode);
		}
		inline virtual void flush() { _file->flush(); }

		inline virtual bool isValid() const { if (!_file) return false; return !_closed; }

	};

public:
	InternalFSFileSystemImpl() {}

public:

	bool format() {
		if (!FS.format()) {
			return false;
		}
		return true;
	}


	virtual bool init() override {
		// Initialize InternalFileSystem
		if (!InternalFS.begin()) {
			return false;
		}
/*
		// Ensure filesystem is writable and reformat if not
		RNS::Bytes test("test");
		if (writeFile("/test", test) < 4) {
			format();
		}
		else {
			remove("/test");
		}
*/
		return true;
	}


	virtual microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override {
		int pmode;
		if (mode == microStore::File::ModeRead) {
			pmode = FILE_O_READ;
		}
		else if (mode == microStore::File::ModeWrite) {
			pmode = FILE_O_WRITE;
			// CBA TODO Replace remove with working truncation
			if (FS.exists(path)) {
				FS.remove(path);
			}
		}
		else if (mode == microStore::File::ModeAppend) {
			// CBA This is the default write mode for nrf52 littlefs
			pmode = FILE_O_WRITE;
		}
		else {
			return {};
		}
		File* file = new File(FS);
		if (!file->open(path, pmode)) {
			return {};
		}
		// Seek to beginning to overwrite (this is failing on nrf52)
		//if (mode == microStore::File::ModeWrite) {
		//	file->seek(0);
		//	file->truncate(0);
		//}
		return microStore::File(new FileImpl(file));
	}


	virtual bool exists(const char* path) override {
		return FS.exists(path);
	}

	virtual bool remove(const char* path) override {
		return FS.remove(path);
	}

	virtual bool rename(const char* from_path, const char* to_path) override {
		return FS.rename(from_path, to_path);
	}

	virtual bool mkdir(const char* path) override {
		if (!FS.mkdir(path)) {
			return false;
		}
		return true;
	}

	virtual bool rmdir(const char* path) override {
		if (!FS.rmdir_r(path)) {
			return false;
		}
		return true;
	}


/*
	virtual size_t readFile(const char* path, RNS::Bytes& data) override {
		size_t read = 0;
		File file(FS);
		if (file.open(path, FILE_O_READ)) {
			size_t size = file.size();
			read = file.readBytes((char*)data.writable(size), size);
			if (read != size) {
				data.resize(read);
			}
			file.close();
		}
		return read;
	}

	virtual size_t writeFile(const char* path, const RNS::Bytes& data) override {
		// CBA TODO Replace remove with working truncation
		if (FS.exists(path)) {
			FS.remove(path);
		}
		size_t wrote = 0;
		File file(FS);
		if (file.open(path, FILE_O_WRITE)) {
			// Seek to beginning to overwrite
			//file.seek(0);
			//file.truncate(0);
			wrote = file.write(data.data(), data.size());
			//TRACE("write_file: closing output file");
			file.close();
		}
		return wrote;
	}
*/

	virtual bool isDirectory(const char* path) override {
		File file(FS);
		if (file.open(path, FILE_O_READ)) {
			bool is_directory = file.isDirectory();
			file.close();
			return is_directory;
		}
		return false;
	}

	virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) override {
		std::list<std::string> files;
		File root = FS.open(path);
		if (!root) {
			return files;
		}
		File file = root.openNextFile();
		while (file) {
			if (!file.isDirectory()) {
				char* name = (char*)file.name();
				if (callback) callback(name);
				else files.push_back(name);
			}
			// CBA Following close required to avoid leaking memory
			file.close();
			file = root.openNextFile();
		}
		root.close();
		return files;
	}


	static int _countLfsBlock(void *p, lfs_block_t block){
		lfs_size_t *size = (lfs_size_t*) p;
		*size += 1;
		return 0;
	}

	static lfs_ssize_t getUsedBlockCount() {
		lfs_size_t size = 0;
		lfs_traverse(InternalFS._getFS(), _countLfsBlock, &size);
		return size;
	}

	static int totalBytes() {
		const lfs_config* config = InternalFS._getFS()->cfg;
		return config->block_size * config->block_count;
	}

	static int usedBytes() {
		const lfs_config* config = InternalFS._getFS()->cfg;
		const int usedBlockCount = getUsedBlockCount();
		return config->block_size * usedBlockCount;
	}

	virtual size_t storageSize()  override{
		//return totalBytes();
		return totalBytes();
	}

	virtual size_t storageAvailable() override {
		//return (totalBytes() - usedBytes());
		return (totalBytes() - usedBytes());
	}

};

}

#endif
