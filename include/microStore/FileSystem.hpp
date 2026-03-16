#pragma once

#include "File.hpp"

#include <list>
#include <memory>
#include <cassert>
#include <functional>
#include <stdint.h>

namespace microStore {

	class FileSystemImpl {

	public:
		struct Callbacks {
			using DirectoryListing = std::function<void(const char*)>;
		};

	protected:
		FileSystemImpl() {}
	public:
		virtual ~FileSystemImpl() {}

	protected:
		virtual bool init() { return true; }
		virtual void loop() {}

		// Factory
		virtual File open(const char* path, File::MODE mode, const bool create = false) = 0;

		//POSIX
		virtual bool exists(const char* path) = 0;
		virtual bool remove(const char* path) = 0;
		virtual bool rename(const char* from_path, const char* to_path) = 0;
		virtual bool mkdir(const char* path) = 0;
		virtual bool rmdir(const char* path) = 0;

		// Helper
		//virtual size_t readFile(const char* path, Bytes& data) = 0;
		//virtual size_t writeFile(const char* path, const Bytes& data) = 0;
		virtual bool isDirectory(const char* path) = 0;
		virtual std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) = 0;
		virtual size_t storageSize() = 0;
		virtual size_t storageAvailable() = 0;

	friend class FileSystem;
	};

	class FileSystem {

	public:
		using Callbacks = FileSystemImpl::Callbacks;

	public:
		FileSystem() {}
		FileSystem(const FileSystem& obj) : _impl(obj._impl) {}
		FileSystem(FileSystemImpl* impl) : _impl(impl) {}
		virtual ~FileSystem() {}

		inline FileSystem& operator = (const FileSystem& obj) { _impl = obj._impl; return *this; }
		inline FileSystem& operator = (FileSystemImpl* impl) { _impl.reset(impl); return *this; }
		inline operator bool() const { return _impl.get() != nullptr; }
		inline bool operator < (const FileSystem& obj) const { return _impl.get() < obj._impl.get(); }
		inline bool operator > (const FileSystem& obj) const { return _impl.get() > obj._impl.get(); }
		inline bool operator == (const FileSystem& obj) const { return _impl.get() == obj._impl.get(); }
		inline bool operator != (const FileSystem& obj) const { return _impl.get() != obj._impl.get(); }
		inline FileSystemImpl* get() { return _impl.get(); }
		inline void clear() { _impl.reset(); }

	public:
		inline bool init() { assert(_impl); return _impl->init(); }
		inline void loop() { assert(_impl); _impl->loop(); }

		// Factory
		inline File open(const char* path, File::MODE mode, const bool create = false) { return _impl->open(path, mode, create); }
		//File open(const char *path, const char *mode = "r", const bool create = false);

		// POSIX
		inline bool exists(const char* path) { assert(_impl); return _impl->exists(path); }
		inline bool remove(const char* path) { assert(_impl); return _impl->remove(path); }
		inline bool rename(const char* from_path, const char* to_path) { assert(_impl); return _impl->rename(from_path, to_path); }
		inline bool mkdir(const char* path) { assert(_impl); return _impl->mkdir(path); }
		inline bool rmdir(const char* path) { assert(_impl); return _impl->rmdir(path); }

		// Helper
		//inline size_t readFile(const char* path, Bytes& data) { assert(_impl); return _impl->readFile(path, data); }
		//inline size_t writeFile(const char* path, const Bytes& data) { assert(_impl); return _impl->writeFile(path, data); }
		inline bool isDirectory(const char* path) { assert(_impl); return _impl->isDirectory(path); }
		inline std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) { assert(_impl); return _impl->listDirectory(path, callback); }
		inline size_t storageSize() { assert(_impl); return _impl->storageSize(); }
		inline size_t storageAvailable() { assert(_impl); return _impl->storageAvailable(); }

		//NEW File open(const char *path, const char *mode = FILE_READ, const bool create = false);
		//NEW bool exists(const char *path);
		//NEW bool remove(const char *path);
		//NEW bool rename(const char *pathFrom, const char *pathTo);
		//NEW bool mkdir(const char *path);
		//NEW bool rmdir(const char *path);

	private:
		std::list<std::string> _empty;

		// getters/setters
	protected:
	public:

#ifndef NDEBUG
		inline std::string debugString() const {
			std::string dump;
			dump = "FileSystem object, this: " + std::to_string((uintptr_t)this) + ", data: " + std::to_string((uintptr_t)_impl.get());
			return dump;
		}
#endif

	protected:
		std::shared_ptr<FileSystemImpl> _impl;
	};

}
