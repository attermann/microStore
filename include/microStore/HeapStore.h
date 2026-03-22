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

#include "Utility.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <vector>
#include <map>

namespace microStore {

/* ---------------- CONFIG (shared with FileStore.h) ---------------- */

#ifndef USTORE_MAX_VALUE_LEN
#define USTORE_MAX_VALUE_LEN 1024
#endif

#ifndef USTORE_MAX_KEY_LEN
#define USTORE_MAX_KEY_LEN 64
#endif

/* ---------------- HEAP STORE ENGINE ---------------- */

template<typename Allocator = std::allocator<uint8_t>>
class BasicHeapStore
{
public:

    using allocator_type = Allocator;

    BasicHeapStore() : BasicHeapStore(Allocator{}) {}
    explicit BasicHeapStore(const Allocator& alloc)
        : alloc_(alloc), data_(map_alloc_type(alloc_)) {}

    allocator_type get_allocator() const { return alloc_; }

    bool init() { return true; }

    /* -------- PUT -------- */

    bool put(const uint8_t* key, uint8_t key_len, const void* data, uint16_t len, uint32_t ts = ustore_time())
    {
        if (key_len > USTORE_MAX_KEY_LEN) {
            printf("[heapstore] put failed due to excessive key length: %u\n", key_len);
            return false;
        }
        if (len > USTORE_MAX_VALUE_LEN) {
            printf("[heapstore] put failed due to excessive data length: %u\n", len);
            return false;
        }

        KeyType k = make_key(key, key_len);

        if (policy_ttl_s_ > 0) {
            uint32_t now = ustore_time();
            for (auto it = data_.begin(); it != data_.end(); ) {
                if (now - it->second.timestamp >= policy_ttl_s_)
                    it = data_.erase(it);
                else
                    ++it;
            }
        }

        if (policy_max_recs_ > 0 && data_.count(k) == 0) {
            while (data_.size() >= policy_max_recs_)
                data_.erase(data_.begin());
        }

        HeapEntry& e = data_[k];
        e.value.assign(static_cast<const uint8_t*>(data),
                       static_cast<const uint8_t*>(data) + len);
        e.timestamp = ts;

        return true;
    }

    inline bool put(const char* key, const void* data, uint16_t len, uint32_t ts = ustore_time())
    {
        return put((const uint8_t*)key, (uint8_t)strlen(key), data, len, ts);
    }

    inline bool put(const std::vector<uint8_t>& key, const void* data, uint16_t len, uint32_t ts = ustore_time())
    {
        return put(key.data(), (uint8_t)key.size(), data, len, ts);
    }

    inline bool put(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data, uint32_t ts = ustore_time())
    {
        return put(key.data(), (uint8_t)key.size(), data.data(), (uint16_t)data.size(), ts);
    }

    /* -------- GET -------- */

    bool get(const uint8_t* key, uint8_t key_len, void* out, uint16_t* size)
    {
        if (key_len > USTORE_MAX_KEY_LEN) {
            printf("[heapstore] get failed due to excessive key length: %u\n", key_len);
            return false;
        }

        auto it = data_.find(make_key(key, key_len));
        if (it == data_.end()) return false;

        const std::vector<uint8_t>& v = it->second.value;

        if (out != nullptr) {
            uint16_t n = (uint16_t)std::min((size_t)*size, v.size());
            memcpy(out, v.data(), n);
        }

        *size = (uint16_t)v.size();
        return true;
    }

    inline bool get(const char* key, void* out, uint16_t* size)
    {
        return get((const uint8_t*)key, (uint8_t)strlen(key), out, size);
    }

    inline bool get(const std::vector<uint8_t>& key, void* out, uint16_t* size)
    {
        return get(key.data(), (uint8_t)key.size(), out, size);
    }

    inline bool get(const std::vector<uint8_t>& key, std::vector<uint8_t>& out)
    {
        out.resize(USTORE_MAX_VALUE_LEN);
        uint16_t size = USTORE_MAX_VALUE_LEN;
        if (!get(key.data(), (uint8_t)key.size(), out.data(), &size))
            return false;
        out.resize(size);
        return true;
    }

    /* -------- REMOVE -------- */

    bool remove(const uint8_t* key, uint8_t key_len)
    {
        if (key_len > USTORE_MAX_KEY_LEN) return false;
        data_.erase(make_key(key, key_len));
        return true;
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
        if (key_len > USTORE_MAX_KEY_LEN) return false;
        return data_.count(make_key(key, key_len)) > 0;
    }

    inline bool exists(const char* key)
    {
        return exists((const uint8_t*)key, (uint8_t)strlen(key));
    }

    inline bool exists(const std::vector<uint8_t>& key)
    {
        return exists(key.data(), (uint8_t)key.size());
    }

    /* -------- SIZE -------- */

    inline size_t size() const { return data_.size(); }

    /* -------- CLEAR -------- */

    void clear() { data_.clear(); }

    /* -------- POLICY -------- */

	inline void set_ttl_secs(uint32_t ttl_s)
	{
		policy_ttl_s_    = ttl_s;
	}

	inline void set_max_recs(uint32_t max_recs)
	{
		policy_max_recs_ = max_recs;
	}

    /* -------- DUMP INFO -------- */

    void dumpInfo(bool detailed = true)
    {
        size_t total_bytes = 0;
        for (auto& kv : data_)
            total_bytes += kv.first.size() + kv.second.value.size();

        printf("HeapStore:\n");
        printf("  Entries     : %8u\n", (unsigned)data_.size());
        printf("  Bytes (key+value): %8u\n", (unsigned)total_bytes);
        (void)detailed;
    }

private:

    template<typename T>
    using rebind_alloc = typename std::allocator_traits<Allocator>::template rebind_alloc<T>;

    using byte_alloc_type = rebind_alloc<uint8_t>;
    using KeyType         = std::vector<uint8_t, byte_alloc_type>;

    struct HeapEntry {
        std::vector<uint8_t> value;
        uint32_t timestamp = 0;
    };

    using MapPairType    = std::pair<const KeyType, HeapEntry>;
    using map_alloc_type = rebind_alloc<MapPairType>;
    using DataMap        = std::map<KeyType, HeapEntry, std::less<KeyType>, map_alloc_type>;

    KeyType make_key(const uint8_t* key, uint8_t key_len) const {
        return KeyType(key, key + key_len, byte_alloc_type(alloc_));
    }

public:

    /* -------- ENTRY -------- */

    struct Entry {
        std::vector<uint8_t> key;
        std::vector<uint8_t> value;
        uint32_t timestamp;
    };

    /* -------- ITERATOR -------- */

    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = Entry;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const Entry*;
        using reference         = const Entry&;

        iterator() {}

        reference operator*()  const { return current_; }
        pointer   operator->() const { return &current_; }

        iterator& operator++() {
            ++pos_;
            load();
            return *this;
        }

        iterator operator++(int) {
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const iterator& o) const { return pos_ == o.pos_; }
        bool operator!=(const iterator& o) const { return pos_ != o.pos_; }

    private:
        friend class BasicHeapStore<Allocator>;

        using map_iter = typename DataMap::iterator;

        iterator(map_iter pos, map_iter end)
            : pos_(pos), end_(end)
        {
            load();
        }

        void load() {
            if (pos_ == end_) return;
            current_.key.assign(pos_->first.begin(), pos_->first.end());
            current_.value     = pos_->second.value;
            current_.timestamp = pos_->second.timestamp;
        }

        map_iter pos_;
        map_iter end_;
        Entry current_;
    };

    /* -------- BEGIN / END -------- */

    iterator begin() { return iterator(data_.begin(), data_.end()); }
    iterator end()   { return iterator(data_.end(),   data_.end()); }

private:

    Allocator alloc_;
    DataMap data_;

    uint32_t policy_ttl_s_    = 0;
    uint32_t policy_max_recs_ = 0;
};

using HeapStore = BasicHeapStore<>;

} // namespace microStore
