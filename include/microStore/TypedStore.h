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

#include "Codec.h"
#include "Log.h"

#include <vector>

namespace microStore {

template<typename Key, typename Value, typename Store, typename KeyCodec = Codec<Key>, typename ValueCodec = Codec<Value>>
class TypedStore
{
public:

    struct Entry
    {
        Key key;
        Value value;
    };

    TypedStore(Store& b):store(b){}

	inline bool isValid() const { return store.isValid(); }
	inline operator bool() const { return isValid(); }

    bool put(const Key& key, const Value& value, uint32_t ttl = 0)
    {
        if (!isValid()) {
			USTORE_LOG("[ustore] put: store is invalid\n");
			return false;
		}
        auto k = KeyCodec::encode(key);
        auto v = ValueCodec::encode(value);
        return store.put(k, v, ttl);
    }

    bool get(const Key& key, Value& value)
    {
        if (!isValid()) {
			USTORE_LOG("[ustore] get: store is invalid\n");
			return false;
		}
        auto k = KeyCodec::encode(key);
        std::vector<uint8_t> raw;
        if (!store.get(k, raw)) return false;
        return ValueCodec::decode(raw, value);
    }

    bool remove(const Key& key)
    {
        if (!isValid()) {
			USTORE_LOG("[ustore] remove: store is invalid\n");
			return false;
		}
        auto k = KeyCodec::encode(key);
        return store.remove(k);
    }

    bool exists(const Key& key)
    {
        if (!isValid()) {
			USTORE_LOG("[ustore] exists: store is invalid\n");
			return false;
		}
        auto k = KeyCodec::encode(key);
        return store.exists(k);
    }

    size_t size() const
    {
        if (!isValid()) {
			USTORE_LOG("[ustore] size: store is invalid\n");
			return 0;
		}
        return store.size();
    }

    class iterator
    {
    public:

        // Decoding is deferred to operator*, so advancing costs nothing.
        //
        // The store underneath is lazy on purpose: BasicFileStore reads a
        // record only when it is dereferenced, and its operator-> is
        // documented as metadata-only for exactly that reason. Loading in the
        // constructor and in operator++ took that back — every step read the
        // record off the filesystem whether the caller wanted it or not, so a
        // traversal paid for entries it never looked at and reaching position
        // N always cost N reads.
        iterator(typename Store::iterator it, typename Store::iterator end)
            : it_(std::move(it)), end_(std::move(end)), loaded_(false)
        {
        }

        iterator& operator++()
        {
            ++it_;
            loaded_ = false;
            return *this;
        }

        bool operator!=(const iterator& other) const
        {
            return it_ != other.it_;
        }

        Entry& operator*()
        {
            if (!loaded_) load();
            return current_;
        }

    private:

        typename Store::iterator it_;
        typename Store::iterator end_;
        Entry current_;
        bool loaded_;

        void load()
        {
            loaded_ = true;
            // The end iterator is not dereferenceable. The constructor used to
            // say so by not loading; the guard moves here now that the load
            // happens on demand.
            if (it_ == end_) return;
            const auto& raw = *it_;  // operator* triggers the store's lazy value load
            KeyCodec::decode(raw.key, current_.key);
            ValueCodec::decode(raw.value, current_.value);
        }
    };

    iterator begin()
    {
        return iterator(store.begin(), store.end());
    }

    iterator end()
    {
        return iterator(store.end(), store.end());
    }

private:

    Store& store;
};

template<typename Key, typename Store>
using TypedKeyStore = TypedStore<Key, Empty, Store>;
}
