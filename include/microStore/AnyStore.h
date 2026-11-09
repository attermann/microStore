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

#include "Log.h"
#include "Utility.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace microStore {

/* ---------------- ANY STORE ----------------
 *
 * AnyStore is a type-erased, copyable handle to any key-value store that
 * provides the common microStore store API (FileStore, HeapStore, or a custom
 * store with the same shape). The concrete store is chosen at runtime:
 *
 *     AnyStore store = use_fs ? AnyStore::make<FileStore>()
 *                             : AnyStore::make<HeapStore>();
 *     store.set_max_recs(100);   // before FileStore::init() so boot pruning applies
 *     if (FileStore* f = store.target<FileStore>()) f->init(fs, "/path/");
 *     else if (HeapStore* h = store.target<HeapStore>()) h->init();
 *     TypedStore<std::string, std::string, AnyStore> typed(store);
 *
 * Or configure and init a concrete store first and adopt it:
 *
 *     std::shared_ptr<FileStore> f = std::make_shared<FileStore>();
 *     f->init(fs, "/path/");
 *     AnyStore store(f);
 *
 * Semantics:
 *  - A default-constructed (or reset()) AnyStore is empty: isValid() is false,
 *    put/get/remove/exists return false, size() returns 0, everything else is
 *    a no-op, and begin() == end().
 *  - Copies share the same backend (like FileSystem). The backend is destroyed
 *    when the last handle (or live iterator) referencing it goes away.
 *  - Store-specific setup (init(), compact(), ...) is done on the concrete
 *    store via target<S>(), which returns nullptr if the backend is not an S.
 *    No RTTI is required.
 *  - Policy setters forward to the current backend only; they are not cached
 *    across reassignment.
 *  - Iteration mirrors FileStore: operator-> yields metadata only (key,
 *    timestamp, ttl); operator* also loads the value. Mutating the store while
 *    iterating is undefined behaviour, as with the backends.
 *
 * Requirements on a wrapped store type S:
 *  isValid() const, size() const, clear(), close(), set_ttl_secs(uint32_t),
 *  set_max_recs(uint32_t), dumpInfo(bool),
 *  put(const uint8_t*, uint8_t, const uint8_t*, uint16_t, uint32_t, uint32_t),
 *  get(const uint8_t*, uint8_t, uint8_t*, uint16_t*),
 *  get(const std::vector<uint8_t>&, std::vector<uint8_t>&),
 *  remove(const uint8_t*, uint8_t), exists(const uint8_t*, uint8_t),
 *  begin()/end() returning S::iterator whose operator-> / operator* expose
 *  key, value, timestamp and ttl.
 */

class AnyStore
{
public:

	/* -------- ENTRY -------- */

	struct Entry {
		std::vector<uint8_t> key;
		std::vector<uint8_t> value;
		uint32_t timestamp = 0;
		uint32_t ttl = 0;
	};

private:

	/* -------- TYPE TAG (RTTI-free type identity) -------- */

	typedef const void* TypeId;

	template<typename T>
	struct TypeTag { static const char id; };

	template<typename T>
	static TypeId type_id() { return &TypeTag<T>::id; }

	/* -------- ITERATOR IMPL -------- */

	class IteratorImpl {
	public:
		virtual ~IteratorImpl() {}
		virtual TypeId type() const = 0;
		virtual bool at_end() const = 0;
		virtual void next() = 0;
		virtual const Entry& meta() const = 0;   // key, timestamp, ttl
		virtual const Entry& entry() const = 0;  // key, value, timestamp, ttl
		virtual std::shared_ptr<IteratorImpl> clone() const = 0;
		virtual bool equals(const IteratorImpl& other) const = 0;
	};

	/* -------- STORE IMPL -------- */

	class StoreImpl {
	public:
		virtual ~StoreImpl() {}
		virtual TypeId type() const = 0;
		virtual void* target_ptr() = 0;
		virtual bool isValid() const = 0;
		virtual bool put(const uint8_t* key, uint8_t key_len, const void* data, uint16_t len, uint32_t ttl, uint32_t ts) = 0;
		virtual bool get(const uint8_t* key, uint8_t key_len, void* out, uint16_t* size) = 0;
		virtual bool get(const std::vector<uint8_t>& key, std::vector<uint8_t>& out) = 0;
		virtual bool remove(const uint8_t* key, uint8_t key_len) = 0;
		virtual bool exists(const uint8_t* key, uint8_t key_len) = 0;
		virtual size_t size() const = 0;
		virtual void clear() = 0;
		virtual void close() = 0;
		virtual void set_ttl_secs(uint32_t ttl_s) = 0;
		virtual void set_max_recs(uint32_t max_recs) = 0;
		virtual void dumpInfo(bool detailed) = 0;
		// Returns nullptr when the store is empty (begin == end).
		virtual std::shared_ptr<IteratorImpl> begin(const std::shared_ptr<StoreImpl>& self) = 0;
	};

	/* -------- ITERATOR MODEL -------- */

	template<typename S>
	class IteratorModel : public IteratorImpl {
	public:
		typedef typename S::iterator iter_type;

		IteratorModel(const std::shared_ptr<StoreImpl>& owner, const iter_type& it, const iter_type& end)
			: owner_(owner), it_(it), end_(end) {}

		TypeId type() const override { return type_id<S>(); }
		bool at_end() const override { return it_ == end_; }

		void next() override {
			++it_;
			meta_ok_ = false;
			value_ok_ = false;
		}

		const Entry& meta() const override {
			if (!meta_ok_ && !at_end()) {
				const auto& e = *it_.operator->();  // metadata only (no value load on FileStore)
				cache_.key.assign(e.key.begin(), e.key.end());
				cache_.value.clear();
				cache_.timestamp = e.timestamp;
				cache_.ttl       = e.ttl;
				meta_ok_ = true;
			}
			return cache_;
		}

		const Entry& entry() const override {
			if (!value_ok_ && !at_end()) {
				const auto& e = *it_;  // full dereference (lazy value load on FileStore)
				cache_.key.assign(e.key.begin(), e.key.end());
				cache_.value.assign(e.value.begin(), e.value.end());
				cache_.timestamp = e.timestamp;
				cache_.ttl       = e.ttl;
				meta_ok_ = true;
				value_ok_ = true;
			}
			return cache_;
		}

		std::shared_ptr<IteratorImpl> clone() const override {
			return std::make_shared<IteratorModel>(*this);
		}

		bool equals(const IteratorImpl& other) const override {
			if (other.type() != type()) return false;
			return it_ == static_cast<const IteratorModel&>(other).it_;
		}

	private:
		std::shared_ptr<StoreImpl> owner_;  // keeps the backend alive while iterating
		iter_type it_;
		iter_type end_;
		mutable Entry cache_;
		mutable bool meta_ok_ = false;
		mutable bool value_ok_ = false;
	};

	/* -------- STORE HOLDERS -------- */

	// Holds S by value (single allocation via make/allocate).
	template<typename S>
	struct InlineHolder {
		template<typename... Args>
		explicit InlineHolder(Args&&... args) : store(std::forward<Args>(args)...) {}
		S& get() { return store; }
		const S& get() const { return store; }
		S store;
	};

	// Shares ownership of an externally created S.
	template<typename S>
	struct SharedHolder {
		explicit SharedHolder(const std::shared_ptr<S>& p) : store(p) {}
		S& get() { return *store; }
		const S& get() const { return *store; }
		std::shared_ptr<S> store;
	};

	/* -------- STORE MODEL -------- */

	template<typename S, typename Holder>
	class StoreModel : public StoreImpl {
	public:
		template<typename... Args>
		explicit StoreModel(Args&&... args) : holder_(std::forward<Args>(args)...) {}

		TypeId type() const override { return type_id<S>(); }
		void* target_ptr() override { return &holder_.get(); }

		bool isValid() const override { return holder_.get().isValid(); }

		bool put(const uint8_t* key, uint8_t key_len, const void* data, uint16_t len, uint32_t ttl, uint32_t ts) override {
			return holder_.get().put(key, key_len, static_cast<const uint8_t*>(data), len, ttl, ts);
		}
		bool get(const uint8_t* key, uint8_t key_len, void* out, uint16_t* size) override {
			return holder_.get().get(key, key_len, static_cast<uint8_t*>(out), size);
		}
		bool get(const std::vector<uint8_t>& key, std::vector<uint8_t>& out) override {
			return holder_.get().get(key, out);
		}
		bool remove(const uint8_t* key, uint8_t key_len) override { return holder_.get().remove(key, key_len); }
		bool exists(const uint8_t* key, uint8_t key_len) override { return holder_.get().exists(key, key_len); }

		size_t size() const override { return holder_.get().size(); }
		void clear() override { holder_.get().clear(); }
		void close() override { holder_.get().close(); }
		void set_ttl_secs(uint32_t ttl_s) override { holder_.get().set_ttl_secs(ttl_s); }
		void set_max_recs(uint32_t max_recs) override { holder_.get().set_max_recs(max_recs); }
		void dumpInfo(bool detailed) override { holder_.get().dumpInfo(detailed); }

		std::shared_ptr<IteratorImpl> begin(const std::shared_ptr<StoreImpl>& self) override {
			typename S::iterator b = holder_.get().begin();
			typename S::iterator e = holder_.get().end();
			if (b == e) return std::shared_ptr<IteratorImpl>();
			return std::make_shared<IteratorModel<S>>(self, b, e);
		}

	private:
		Holder holder_;
	};

	explicit AnyStore(const std::shared_ptr<StoreImpl>& impl) : _impl(impl) {}

public:

	/* -------- ITERATOR -------- */

	// Forward iterator over all live records of the backend. end() is a null
	// sentinel, so it never allocates; begin() allocates one small iterator
	// object and stepping does not allocate.
	class iterator {
	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type        = Entry;
		using difference_type   = std::ptrdiff_t;
		using pointer           = const Entry*;
		using reference         = const Entry&;

		iterator() {}

		// operator*  — full dereference; loads the value (lazily on FileStore).
		// operator-> — metadata only (key, timestamp, ttl); value is empty unless
		//              operator* was already called for this position.
		reference operator*()  const { return impl_->entry(); }
		pointer   operator->() const { return &impl_->meta(); }

		iterator& operator++() {
			if (impl_) {
				if (impl_.use_count() > 1) impl_ = impl_->clone();  // don't advance shared copies
				impl_->next();
			}
			return *this;
		}

		iterator operator++(int) {
			iterator tmp = *this;
			++(*this);
			return tmp;
		}

		bool operator==(const iterator& o) const {
			if (!impl_ && !o.impl_) return true;
			if (!impl_)   return o.impl_->at_end();
			if (!o.impl_) return impl_->at_end();
			return impl_->equals(*o.impl_);
		}
		bool operator!=(const iterator& o) const { return !(*this == o); }

	private:
		friend class AnyStore;
		explicit iterator(const std::shared_ptr<IteratorImpl>& impl) : impl_(impl) {}
		std::shared_ptr<IteratorImpl> impl_;
	};

	/* -------- CONSTRUCTION -------- */

	// Empty store: isValid() == false.
	AnyStore() {}

	// Adopt an existing store, sharing ownership. A null pointer gives an empty store.
	template<typename S>
	explicit AnyStore(const std::shared_ptr<S>& store) {
		if (store) _impl = std::make_shared<StoreModel<S, SharedHolder<S>>>(store);
	}

	// Construct an S in place (single allocation). Arguments go to S's constructor.
	template<typename S, typename... Args>
	static AnyStore make(Args&&... args) {
		return AnyStore(std::shared_ptr<StoreImpl>(
			std::make_shared<StoreModel<S, InlineHolder<S>>>(std::forward<Args>(args)...)));
	}

	// Like make(), but allocates the store object with the given allocator
	// (e.g. a PSRAM allocator). Arguments go to S's constructor.
	template<typename S, typename Alloc, typename... Args>
	static AnyStore allocate(const Alloc& alloc, Args&&... args) {
		return AnyStore(std::shared_ptr<StoreImpl>(
			std::allocate_shared<StoreModel<S, InlineHolder<S>>>(alloc, std::forward<Args>(args)...)));
	}

	// Pointer to the concrete store if the backend is an S, otherwise nullptr.
	template<typename S>
	S* target() {
		if (!_impl || _impl->type() != type_id<S>()) return nullptr;
		return static_cast<S*>(_impl->target_ptr());
	}

	template<typename S>
	const S* target() const {
		if (!_impl || _impl->type() != type_id<S>()) return nullptr;
		return static_cast<const S*>(_impl->target_ptr());
	}

	// Release the backend; the store becomes empty.
	void reset() { _impl.reset(); }

	// True if a backend is attached (even if that backend is not yet valid).
	bool hasStore() const { return (bool)_impl; }

	inline bool isValid() const { return _impl && _impl->isValid(); }
	inline operator bool() const { return isValid(); }

	/* -------- PUT -------- */

	bool put(const uint8_t* key, uint8_t key_len, const void* data, uint16_t len, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		if (!_impl) {
			USTORE_LOG("[ustore] put: AnyStore is empty\n");
			return false;
		}
		return _impl->put(key, key_len, data, len, ttl, ts);
	}

	inline bool put(const char* key, const void* data, uint16_t len, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put((const uint8_t*)key, (uint8_t)strlen(key), data, len, ttl, ts);
	}

	inline bool put(const std::vector<uint8_t>& key, const void* data, uint16_t len, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put(key.data(), (uint8_t)key.size(), data, len, ttl, ts);
	}

	inline bool put(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put(key.data(), (uint8_t)key.size(), data.data(), (uint16_t)data.size(), ttl, ts);
	}

	/* -------- GET -------- */

	bool get(const uint8_t* key, uint8_t key_len, void* out, uint16_t* size)
	{
		if (!_impl) {
			USTORE_LOG("[ustore] get: AnyStore is empty\n");
			return false;
		}
		return _impl->get(key, key_len, out, size);
	}

	inline bool get(const char* key, void* out, uint16_t* size)
	{
		return get((const uint8_t*)key, (uint8_t)strlen(key), out, size);
	}

	inline bool get(const std::vector<uint8_t>& key, void* out, uint16_t* size)
	{
		return get(key.data(), (uint8_t)key.size(), out, size);
	}

	bool get(const std::vector<uint8_t>& key, std::vector<uint8_t>& out)
	{
		if (!_impl) {
			USTORE_LOG("[ustore] get: AnyStore is empty\n");
			return false;
		}
		return _impl->get(key, out);
	}

	/* -------- REMOVE -------- */

	bool remove(const uint8_t* key, uint8_t key_len)
	{
		if (!_impl) return false;
		return _impl->remove(key, key_len);
	}

	inline bool remove(const char* key)
	{
		return remove((const uint8_t*)key, (uint8_t)strlen(key));
	}

	inline bool remove(const std::vector<uint8_t>& key)
	{
		return remove(key.data(), (uint8_t)key.size());
	}

	/* -------- EXISTS -------- */

	bool exists(const uint8_t* key, uint8_t key_len)
	{
		if (!_impl) return false;
		return _impl->exists(key, key_len);
	}

	inline bool exists(const char* key)
	{
		return exists((const uint8_t*)key, (uint8_t)strlen(key));
	}

	inline bool exists(const std::vector<uint8_t>& key)
	{
		return exists(key.data(), (uint8_t)key.size());
	}

	/* -------- SIZE / CLEAR / CLOSE -------- */

	inline size_t size() const { return _impl ? _impl->size() : 0; }

	void clear() { if (_impl) _impl->clear(); }

	void close() { if (_impl) _impl->close(); }

	/* -------- POLICY -------- */

	inline void set_ttl_secs(uint32_t ttl_s) { if (_impl) _impl->set_ttl_secs(ttl_s); }

	inline void set_max_recs(uint32_t max_recs) { if (_impl) _impl->set_max_recs(max_recs); }

	/* -------- DUMP INFO -------- */

	void dumpInfo(bool detailed = true)
	{
		if (!_impl) {
			printf("AnyStore: (empty)\n");
			return;
		}
		_impl->dumpInfo(detailed);
	}

	/* -------- BEGIN / END -------- */

	iterator begin() {
		if (!_impl) return iterator();
		return iterator(_impl->begin(_impl));
	}

	iterator end() { return iterator(); }

private:

	std::shared_ptr<StoreImpl> _impl;
};

template<typename T>
const char AnyStore::TypeTag<T>::id = 0;

} // namespace microStore
