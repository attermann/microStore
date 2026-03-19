#pragma once

#if defined(USTORE_USE_FLASHFS)

#include "../File.h"
#include "../FileSystem.h"

#include <Cached_SPIFlash.h>
#include <FlashFileSystem.h>
#define FS FlashFS
using namespace Adafruit_LittleFS;

namespace microStoreImpl {

Adafruit_FlashTransport_SPI g_flashTransport(SS, SPI);

//Flash definition structure for GD25Q16C Flash (RAK15001)
Cached_SPIFlash g_flash(&g_flashTransport);
SPIFlash_Device_t g_RAK15001 {
	.total_size = (1UL << 21),
	.start_up_time_us = 5000,
	.manufacturer_id = 0xc8,
	.memory_type = 0x40,
	.capacity = 0x15,
	.max_clock_speed_mhz = 15,
	.quad_enable_bit_mask = 0x00,
	.has_sector_protection = false,
	.supports_fast_read = true,
	.supports_qspi = false,
	.supports_qspi_writes = false,
	.write_status_register_split = false,
	.single_status_byte = true,
};

class FlashFSFileSystemImpl : public microStore::FileSystemImpl {

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
	FlashFSFileSystemImpl() {}

public:

	virtual bool format() override {
		if (!FlashFS.format()) {
			return false;
		}
		return true;
	}

	virtual bool init() override {
		// Initialize FlashFileSystem
		if (!g_flash.begin(&g_RAK15001)) {
			return false;
		}
		if (!FlashFS.begin(&g_flash)) {
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
		int pmode;
		if (mode == microStore::File::ModeRead) {
			pmode = FILE_O_READ;
		}
		else if (mode == microStore::File::ModeWrite) {
			pmode = FILE_O_WRITE;
			// CBA TODO Replace remove with working truncation
			if (FlashFS.exists(path)) {
				FlashFS.remove(path);
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
		return FlashFS.exists(path);
	}

	virtual bool remove(const char* path) override {
		return FlashFS.remove(path);
	}

	virtual bool rename(const char* from_path, const char* to_path) override {
		return FlashFS.rename(from_path, to_path);
	}

	virtual bool mkdir(const char* path) override {
		if (!FlashFS.mkdir(path)) {
			return false;
		}
		return true;
	}

	virtual bool rmdir(const char* path) override {
		if (!FlashFS.rmdir_r(path)) {
			return false;
		}
		return true;
	}


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
		File root = FlashFS.open(path);
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
		return FlashFS.totalBytes();
	}

	virtual size_t storageAvailable() override {
		return (FlashFS.totalBytes() - FlashFS.usedBytes());
	}

};

}

#endif
