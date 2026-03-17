#pragma once

/*
KV Table Project — Feature Summary

Purpose: A generic, type-safe key-value table abstraction for embedded systems
with swappable storage backends at compile time.

Core Abstraction
- Table<K, V, Backend, KeyCodec, ValueCodec> — central template that composes storage + serialization
- Backends implement: put, get, erase, and iterator over raw byte pairs
- Codecs implement: encode(T) → vector<uint8_t> and decode(vector<uint8_t>) → T
- Codec<T> — generic raw memcpy + std::string specialization              

Notable Design Choices
- No formal test framework — main.cpp acts as a manual integration test
- Flash write granularity is handled per-platform (nRF52 requires 32-byte alignment)
- microStore external dependency bridges POSIX file calls to embedded filesystems (LittleFS)
- Keys stored as hex-encoded filenames on POSIX (e.g., "nodeA" → kvstore/6e6f646541)                                                           
                                                        
*/

#include <vector>

namespace microStore {

template<typename Key,typename Value,typename Backend, typename KeyCodec,typename ValueCodec>
class Table
{
public:

    struct Entry
    {
        Key key;
        Value value;
    };

    Table(Backend& b):backend(b){}

    bool put(const Key& key,const Value& value)
    {
        auto k=KeyCodec::encode(key);
        auto v=ValueCodec::encode(value);

        return backend.put(k,v);
    }

    bool get(const Key& key,Value& value)
    {
        auto k=KeyCodec::encode(key);

        std::vector<uint8_t> raw;

        if(!backend.get(k, raw)) return false;

        return ValueCodec::decode(raw, value);
    }

    bool remove(const Key& key)
    {
        auto k=KeyCodec::encode(key);
        return backend.remove(k);
    }

    class iterator
    {
    public:

        iterator(typename Backend::iterator it, typename Backend::iterator end)
            : it_(std::move(it)), end_(std::move(end))
        {
            if(it_ != end_) load();
        }

        iterator& operator++()
        {
            ++it_;
            if(it_ != end_) load();
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
