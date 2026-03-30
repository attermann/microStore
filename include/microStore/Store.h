/*
 * Copyright (c) 2026 Chad Attermann
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#pragma once

#include <memory>
#include <vector>
#include <cassert>
#include <stdint.h>

namespace microStore {

/* ---------------- STORE IMPLEMENTATION BASE CLASS ---------------- */

template<typename Allocator = std::allocator<uint8_t>>
class StoreImpl {

public:
	using allocator_type = Allocator;

	/* -------- ENTRY -------- */

	struct Entry {
		std::vector<uint8_t> key;
		std::vector<uint8_t> value;
		uint32_t timestamp;
		uint32_t ttl;
	};

protected:
	StoreImpl() {}
	explicit StoreImpl(const Allocator& alloc) : _alloc(alloc) {}

public:
	virtual ~StoreImpl() {}

protected:
	// Core CRUD operations
	virtual bool isValid() const = 0;
	virtual bool init() { return true; }  // default no-op implementation

	virtual bool put(const std::vector<uint8_t>& key, const std::vector<uint8_t>& value, uint32_t ttl = 0, uint32_t ts = 0) = 0;
	virtual bool get(const std::vector<uint8_t>& key, std::vector<uint8_t>& out) = 0;
	virtual bool remove(const std::vector<uint8_t>& key) = 0;
	virtual bool exists(const std::vector<uint8_t>& key) = 0;
	virtual size_t size() const = 0;
	virtual void clear() = 0;
	virtual void dumpInfo(bool detailed) = 0;

	// Policy methods with default implementations
	virtual void set_ttl_secs(uint32_t ttl_s) { policy_ttl_secs = ttl_s; }
	virtual void set_max_recs(uint32_t max_recs) { policy_max_recs = max_recs; }

	// Type-erased iterator support
	virtual void* begin_impl() = 0;
	virtual void* end_impl() = 0;
	virtual void iterator_increment(void* handle) = 0;
	virtual bool iterator_equal(void* a, void* b) = 0;
	virtual Entry iterator_deref(void* handle) = 0;
	virtual void iterator_destroy(void* handle) = 0;

protected:
	Allocator _alloc;
	uint32_t policy_ttl_secs = 0;
	uint32_t policy_max_recs = 0;

template<typename A> friend class Store;
};

/* ---------------- STORE WRAPPER CLASS ---------------- */

template<typename Allocator = std::allocator<uint8_t>>
class Store {

public:
	using allocator_type = Allocator;
	using Entry = typename StoreImpl<Allocator>::Entry;

public:
	Store() {}
	Store(const Store& obj) : _impl(obj._impl) {}
	Store(StoreImpl<Allocator>* impl) : _impl(impl) {}
	virtual ~Store() {}

	inline Store& operator = (const Store& obj) { _impl = obj._impl; return *this; }
	inline Store& operator = (StoreImpl<Allocator>* impl) { _impl.reset(impl); return *this; }
	inline bool operator < (const Store& obj) const { return _impl.get() < obj._impl.get(); }
	inline bool operator > (const Store& obj) const { return _impl.get() > obj._impl.get(); }
	inline bool operator == (const Store& obj) const { return _impl.get() == obj._impl.get(); }
	inline bool operator != (const Store& obj) const { return _impl.get() != obj._impl.get(); }
	inline StoreImpl<Allocator>* get() { return _impl.get(); }
	inline void clear_impl() { _impl.reset(); }
	inline bool isValid() const { if (_impl.get() == nullptr) return false; return _impl->isValid(); }
	inline operator bool() const { return isValid(); }

public:
	// Core CRUD operations
	inline bool init() { assert(_impl); return _impl->init(); }

	inline bool put(const std::vector<uint8_t>& key, const std::vector<uint8_t>& value, uint32_t ttl = 0, uint32_t ts = 0) {
		assert(_impl); return _impl->put(key, value, ttl, ts);
	}

	inline bool get(const std::vector<uint8_t>& key, std::vector<uint8_t>& out) {
		assert(_impl); return _impl->get(key, out);
	}

	inline bool remove(const std::vector<uint8_t>& key) {
		assert(_impl); return _impl->remove(key);
	}

	inline bool exists(const std::vector<uint8_t>& key) {
		assert(_impl); return _impl->exists(key);
	}

	inline size_t size() const {
		assert(_impl); return _impl->size();
	}

	inline void clear() {
		assert(_impl); _impl->clear();
	}

	inline void dumpInfo(bool detailed = true) {
		assert(_impl); _impl->dumpInfo(detailed);
	}

	// Policy methods
	inline void set_ttl_secs(uint32_t ttl_s) {
		assert(_impl); _impl->set_ttl_secs(ttl_s);
	}

	inline void set_max_recs(uint32_t max_recs) {
		assert(_impl); _impl->set_max_recs(max_recs);
	}

	/* -------- ITERATOR -------- */

	// RAII wrapper for type-erased iterator handles
	class iterator {
	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type        = Entry;
		using difference_type   = std::ptrdiff_t;
		using pointer           = const Entry*;
		using reference         = const Entry&;

		// Default constructor — produces a singular (non-dereferenceable) iterator
		iterator() : store_(nullptr), handle_(nullptr) {}

		// Destructor — releases the type-erased handle
		~iterator() {
			if (store_ && handle_)
				store_->_impl->iterator_destroy(handle_);
		}

		// Copy constructor
		iterator(const iterator& other) : store_(other.store_), handle_(nullptr), current_(other.current_) {
			// Type-erased handles cannot be copied, so we cannot support copy construction
			// after the handle has been created. This is a limitation of the type erasure.
			// For now, we'll just set handle to nullptr and let the comparison operators work.
			// This is safe because we only use iterators in range-based for loops.
		}

		// Copy assignment
		iterator& operator=(const iterator& other) {
			if (this != &other) {
				if (store_ && handle_)
					store_->_impl->iterator_destroy(handle_);
				store_ = other.store_;
				handle_ = nullptr;
				current_ = other.current_;
			}
			return *this;
		}

		reference operator*()  const { return current_; }
		pointer   operator->() const { return &current_; }

		iterator& operator++() {
			store_->_impl->iterator_increment(handle_);
			current_ = store_->_impl->iterator_deref(handle_);
			return *this;
		}

		iterator operator++(int) {
			iterator tmp = *this;
			++(*this);
			return tmp;
		}

		bool operator==(const iterator& o) const {
			if (store_ != o.store_) return false;
			if (!store_) return true;  // both are default-constructed
			if (!handle_ && !o.handle_) return true;  // both are end iterators
			if (!handle_ || !o.handle_) return false;  // one is end, other is not
			return store_->_impl->iterator_equal(handle_, o.handle_);
		}

		bool operator!=(const iterator& o) const { return !(*this == o); }

	private:
		friend class Store<Allocator>;

		iterator(Store* store, void* handle)
			: store_(store), handle_(handle)
		{
			if (handle_)
				current_ = store_->_impl->iterator_deref(handle_);
		}

		Store*  store_;
		void*   handle_;
		Entry   current_;
	};

	/* -------- BEGIN / END -------- */

	iterator begin() {
		assert(_impl);
		return iterator(this, _impl->begin_impl());
	}

	iterator end() {
		assert(_impl);
		return iterator(this, _impl->end_impl());
	}

protected:
	std::shared_ptr<StoreImpl<Allocator>> _impl;
};

} // namespace microStore
