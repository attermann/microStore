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

#include <vector>

namespace microStore {

template<typename Key, typename Value, typename Backend, typename KeyCodec = Codec<Key>, typename ValueCodec = Codec<Value>>
class Table
{
public:

    struct Entry
    {
        Key key;
        Value value;
    };

    Table(Backend& b):backend(b){}

    bool put(const Key& key, const Value& value)
    {
        auto k = KeyCodec::encode(key);
        auto v = ValueCodec::encode(value);

        return backend.put(k, v);
    }

    bool get(const Key& key, Value& value)
    {
        auto k = KeyCodec::encode(key);

        std::vector<uint8_t> raw;

        if (!backend.get(k, raw)) return false;

        return ValueCodec::decode(raw, value);
    }

    bool remove(const Key& key)
    {
        auto k = KeyCodec::encode(key);
        return backend.remove(k);
    }

    bool exists(const Key& key)
    {
        auto k = KeyCodec::encode(key);
        return backend.exists(k);
    }

    size_t size()
    {
        return backend.size();
    }

    class iterator
    {
    public:

        iterator(typename Backend::iterator it, typename Backend::iterator end)
            : it_(std::move(it)), end_(std::move(end))
        {
            if (it_ != end_) load();
        }

        iterator& operator++()
        {
            ++it_;
            if (it_ != end_) load();
            return *this;
        }

        bool operator!=(const iterator& other) const
        {
            return it_ != other.it_;
        }

        Entry operator*()
        {
            return current_;
        }

    private:

        typename Backend::iterator it_;
        typename Backend::iterator end_;
        Entry current_;

        void load()
        {
            KeyCodec::decode(it_->key, current_.key);
            ValueCodec::decode(it_->value, current_.value);
        }
    };

    iterator begin()
    {
        return iterator(backend.begin(), backend.end());
    }

    iterator end()
    {
        return iterator(backend.end(), backend.end());
    }

private:

    Backend& backend;
};

}
