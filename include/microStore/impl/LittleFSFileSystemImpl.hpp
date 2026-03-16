#pragma once

#if defined(MICROSTORE_USE_LITTLEFS)

#include "../File.hpp"
#include "../FileSystem.hpp"

#include <LittleFS.h>

namespace microStoreImpl {

class LittleFSFileSystemImpl : public microStore::FileSystemImpl {

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
		inline virtual size_t write(const uint8_t* buffer, size_t size) { return _file->write((uint8_t*)buffer, size); }

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
	LittleFSFileSystemImpl() {}

public:

	bool format() {
		if (!LittleFS.format()) {
			return false;
		}
		return true;
	}


	virtual bool init() override {
		// Initialize LittleFS
		if (!LittleFS.begin(true, "")) {
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
		const char* pmode;
		switch (mode) {
			case microStore::File::ModeRead:
				pmode = FILE_READ;
				break;
			case microStore::File::ModeWrite:
				pmode = FILE_WRITE;
				break;
			case microStore::File::ModeAppend:
				pmode = FILE_APPEND;
				break;
			case microStore::File::ModeReadWrite:
				pmode = "w+";
				break;
			case microStore::File::ModeReadAppend:
				pmode = "a+";
				break;
			default:
				return {};
		}
		// CBA Using copy constructor to obtain File*
		File* file = new File(LittleFS.open(path, pmode));
		if (file == nullptr || !(*file)) {
			return {};
		}
		return microStore::File(new FileImpl(file));
	}


	virtual bool exists(const char* path) override {
		return LittleFS.exists(path);
	}

	virtual bool remove(const char* path) override {
		return LittleFS.remove(path);
	}

	virtual bool rename(const char* from_path, const char* to_path) override {
		return LittleFS.rename(from_path, to_path);
	}

	virtual bool mkdir(const char* path) override {
		if (!LittleFS.mkdir(path)) {
			return false;
		}
		return true;
	}

	virtual bool rmdir(const char* path) override {
		if (!LittleFS.rmdir(path)) {
			return false;
		}
		return true;
	}


/*
	virtual size_t readFile(const char* path, RNS::Bytes& data) override {
		size_t read = 0;
		File file = LittleFS.open(path, FILE_READ);
		if (file) {
			size_t size = file.size();
			read = file.readBytes((char*)data.writable(size), size);
			if (read != size) {
				data.resize(read);
			}
			//TRACE("read_file: closing input file");
			file.close();
		}
		return read;
	}

	virtual size_t writeFile(const char* path, const RNS::Bytes& data) override {
		// CBA TODO Replace remove with working truncation
		if (LittleFS.exists(path)) {
			LittleFS.remove(path);
		}
		size_t wrote = 0;
		File file = LittleFS.open(path, FILE_WRITE);
		if (file) {
			// Seek to beginning to overwrite
			//file.seek(0);
			//file.truncate(0);
			wrote = file.write(data.data(), data.size());
			file.close();
		}
		return wrote;
	}
*/

	virtual bool isDirectory(const char* path) override {
		File file = LittleFS.open(path, FILE_READ);
		if (file) {
			bool is_directory = file.isDirectory();
			file.close();
			return is_directory;
		}
		return false;
	}

	virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) override {
		std::list<std::string> files;
		File root = LittleFS.open(path);
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
		return LittleFS.totalBytes();
	}

	virtual size_t storageAvailable() override {
		return (LittleFS.totalBytes() - LittleFS.usedBytes());
	}

};

}

#endif
