#pragma once

#if defined(USTORE_USE_SPIFFS)

#include "../File.h"
#include "../FileSystem.h"

#include <SPIFFS.h>
#define FS SPIFFS

namespace microStoreImpl {

class SPIFFSFileSystemImpl : public microStore::FileSystemImpl {

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
			fs::SeekMode smode;
			switch (mode) {
				case microStore::SeekMode::SeekModeCur:
					smode = fs::SeekMode::SeekCur;
					break;
				case microStore::SeekMode::SeekModeEnd:
					smode = fs::SeekMode::SeekEnd;
					break;
				case microStore::SeekMode::SeekModeSet:
				default:
					smode = fs::SeekMode::SeekSet;
					break;
			}
			return _file->seek(pos, smode);
		}
		inline virtual void flush() { _file->flush(); }

		inline virtual bool isValid() const { if (!_file) return false; return !_closed; }

	};

public:
	SPIFFSFileSystemImpl() {}

public:

	virtual bool format() override {
		if (!FS.format()) {
			return false;
		}
		return true;
	}

	virtual bool init() override {
		printf("[ustore] Initializing SPIFFSFileSystem\n");
		// Initialize SPIFFS
		if (!SPIFFS.begin(true, "")) {
			return false;
		}
/*
		// Ensure filesystem is writable and reformat if not
		if (writeFile("/test", "test", 4) < 4) {
			format();
		}
		else {
			remove("/test");
		}
*/
		return true;
	}


	virtual microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override {
		const char* pmode;
		if (mode == microStore::File::ModeRead) {
			pmode = FILE_READ;
		}
		else if (mode == microStore::File::ModeWrite) {
			pmode = FILE_WRITE;
		}
		else if (mode == microStore::File::ModeAppend) {
			pmode = FILE_APPEND;
		}
		else {
			return {};
		}
		// CBA Using copy constructor to obtain File*
		File* file = new File(FS.open(path, pmode));
		if (file == nullptr || !(*file)) {
			return {};
		}
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
		if (!FS.rmdir(path)) {
			return false;
		}
		return true;
	}


	virtual bool isDirectory(const char* path) override {
		File file = FS.open(path, FILE_READ);
		if (file) {
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

	virtual size_t storageSize() override {
		return SPIFFS.totalBytes();
	}

	virtual size_t storageAvailable() override {
		return (SPIFFS.totalBytes() - SPIFFS.usedBytes());
	}

};

}

#endif
