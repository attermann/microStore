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

#include "File.h"
#include "FileSystem.h"
#include "Utility.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <vector>
#include <unordered_map>

namespace microStore {

/* ---------------- CONFIG ---------------- */

#ifndef USTORE_MAX_VALUE_LEN
#define USTORE_MAX_VALUE_LEN 1024
#endif

#ifndef USTORE_SEGMENT_SIZE
#define USTORE_SEGMENT_SIZE 65536
#endif

#ifndef USTORE_MAX_SEGMENTS
#define USTORE_MAX_SEGMENTS 8
#endif

#ifndef USTORE_WRITE_BUFFER
//#define USTORE_WRITE_BUFFER 4096
#define USTORE_WRITE_BUFFER 512
#endif

#ifndef USTORE_MAX_KEY_LEN
#define USTORE_MAX_KEY_LEN 64
#endif

#ifndef USTORE_MAX_FILENAME_LEN
#define USTORE_MAX_FILENAME_LEN 64
#endif

#ifndef USTORE_COMPACT_RETRY_MS
//#define USTORE_COMPACT_RETRY_MS 5000   // ms between compact() retries after failure
#define USTORE_COMPACT_RETRY_MS 60000   // ms between compact() retries after failure
#endif

#ifndef USTORE_COMPACT_THRESHOLD
#define USTORE_COMPACT_THRESHOLD 25  // % dead records that trigger auto-compact (0 = disabled)
#endif

#ifndef USTORE_DEFAULT_TTL_SECS
#define USTORE_DEFAULT_TTL_SECS 0
#endif

#ifndef USTORE_DEFAULT_MAX_RECS
#define USTORE_DEFAULT_MAX_RECS 0
#endif

/* ---------------- CONSTANTS ---------------- */

static const uint32_t MAGIC_RECORD  = 0xC0DEC0DE;
static const uint32_t MAGIC_COMMIT  = 0xFACEB00C;
static const uint16_t FLAG_DELETE   = 1;
static const uint32_t JOURNAL_MAGIC = 0x4B564A4E;

enum JournalState { JOURNAL_NONE = 0, JOURNAL_COMPACTING = 1, JOURNAL_COMMIT = 2 };

#pragma pack(push,1)
struct Journal {
	uint32_t magic;
	uint32_t state;           // JOURNAL_NONE / COMPACTING / COMMIT
	uint32_t next_seg;        // segments 0..next_seg-1 are committed to compact.tmp
	uint32_t tmp_valid_size;  // byte count of compact.tmp that is durably valid
};
#pragma pack(pop)


/* ---------------- RECORD STRUCTURES ---------------- */

#pragma pack(push,1)

struct RecordHeader
{
	uint32_t magic;       // framing sentinel — MUST be first
	uint8_t  flags;       // record type (normal/delete) — affects how the rest of the header is interpreted
	uint8_t  key_len;     // length of the key
	uint16_t length;      // lenght of the value
	uint32_t timestamp;   // timestamp at record insertion
	uint32_t ttl;         // per-record TTL in seconds; 0 = use global policy
	uint32_t crc;         // integrity check — MUST be last
};

struct RecordCommit
{
	uint32_t magic;
};

#pragma pack(pop)

/* ---------------- STORAGE ENGINE ---------------- */

template<typename Allocator = std::allocator<uint8_t>>
class BasicFileStore
{
public:

	using allocator_type = Allocator;

	BasicFileStore() : BasicFileStore(Allocator{}) {}
	explicit BasicFileStore(const Allocator& alloc)
		: _alloc(alloc), _index(map_alloc_type(_alloc)) { write_buf_pos = 0; }

	~BasicFileStore()
	{
		if (isValid())
			flush_buffer();
	}

	inline allocator_type get_allocator() const { return _alloc; }

	inline bool isValid() const { if (!_filesystem) return false; return true; }
	inline operator bool() const { return isValid(); }

	bool init(FileSystem& filesystem, const char* prefix)
	{
		_filesystem = filesystem;
		strncpy(base_prefix,prefix,sizeof(base_prefix));

		recover_if_needed();

		if (!load_index())
			rebuild_index_from_segments();

		// Enforce max_recs at boot: prune excess entries and rewrite the
		// persistent index so evicted keys don't reappear on the next reboot.
		size_t pruned = prune_index_to_max_recs_();
		if (pruned > 0) {
			char iname[USTORE_MAX_FILENAME_LEN]; index_name(iname);
			_filesystem.remove(iname);
			write_index_bulk();
		}

		open_index_for_append();

		// Resume at the highest segment that exists on flash, not always seg 0.
		// Without this, after reset current_segment stays 0 and new writes
		// overwrite previously-live segments, losing all their records.
		uint32_t resume_seg = 0;
		for (uint32_t i = 0; i < USTORE_MAX_SEGMENTS; i++)
		{
			char name[USTORE_MAX_FILENAME_LEN];
			segment_name(i, name);
			File f = _filesystem.open(name, File::ModeRead);
			if (f) { f.close(); resume_seg = i; }
		}

		open_segment(resume_seg);

		return true;
	}

	void clear()
	{
        if (!isValid()) return;

		char name[USTORE_MAX_FILENAME_LEN];

		if (index_file) index_file.close();

		for(uint32_t i=0;i<USTORE_MAX_SEGMENTS;i++)
		{
			segment_name(i,name);
			_filesystem.remove(name);
		}

		index_name(name);
		_filesystem.remove(name);

		_index.clear();
		_dead_since_compact = 0;
		current_segment=0;
		current_offset=0;
		write_buf_pos=0;

		open_index_for_append();
		open_segment(0);
	}

	/* -------- PUT -------- */

	bool put(const uint8_t* key, uint8_t key_len, const uint8_t* data, uint16_t len, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
        if (!isValid()) return false;

		if (key_len > USTORE_MAX_KEY_LEN) {
			printf("[ustore] put: failed due to excessive key length: %u\n", key_len);
			return false;
		}
		if (len > USTORE_MAX_VALUE_LEN) {
			printf("[ustore] put: failed due to excessive data length: %u\n", len);
			return false;
		}

		// CBA Don't fail to put just because rotation fails
		//if (!rotate_segment_if_needed(len))
		//    return false;
		rotate_segment_if_needed(len);

		RecordHeader hdr;

		hdr.magic = MAGIC_RECORD;
		hdr.key_len = key_len;
		hdr.timestamp = ts;
		hdr.ttl = ttl;
		hdr.length = len;
		hdr.flags = 0;

		hdr.crc = crc32(0,(uint8_t*)&hdr,sizeof(hdr)-4);
		hdr.crc = crc32(hdr.crc,key,key_len);
		hdr.crc = crc32(hdr.crc,(uint8_t*)data,len);

		uint32_t offset = current_offset;

		append_buffer((uint8_t*)&hdr, sizeof(hdr));
		append_buffer(key, key_len);
		append_buffer(data, len);

		RecordCommit c;
		c.magic = MAGIC_COMMIT;

		append_buffer((uint8_t*)&c, sizeof(c));

		// ensure data is on flash before committing index entry
		// if flush fails then fail put without writing index entry
		if (!flush_buffer()) {
			return false;
		}

		// If an existing record was updated then increment _dead_since_compact
		if (index_find(key, key_len)) _dead_since_compact++;

		index_insert(key, key_len, current_segment, offset, ts, ttl);

		// Enforce max_recs: evict the oldest record(s) from the in-memory index
		// when a new key pushes the count over the limit. Orphaned disk records
		// will be reclaimed at the next compaction.
		if (policy_max_recs > 0 && _index.size() > policy_max_recs)
			prune_index_to_max_recs_();

		persist_index_entry(key, key_len, current_segment, offset, ts, ttl);

		current_offset += sizeof(hdr)+key_len+len+sizeof(c);

		compact_if_threshold();

printf("[ustore] put: wrote key %s with data length %u\n", bin_str(key, key_len), len);
//printf("[ustore] put: %s\n", bin_str((uint8_t*)data, len));
		return true;
	}

	inline bool put(const char* key, const uint8_t* data, uint16_t len, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put((const uint8_t*)key, (uint8_t)strlen(key), data, len, ttl, ts);
	}

	inline bool put(const char* key, const char* data, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put((const uint8_t*)key, (uint8_t)strlen(key), (const uint8_t*)data, (uint16_t)strlen(data), ttl, ts);
	}

	inline bool put(const std::vector<uint8_t>& key, const uint8_t* data, uint16_t len, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put(key.data(), (uint8_t)key.size(), data, len, ttl, ts);
	}

	inline bool put(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data, uint32_t ttl = 0, uint32_t ts = microStore::time())
	{
		return put(key.data(), (uint8_t)key.size(), data.data(), (uint16_t)data.size(), ttl, ts);
	}

	/* -------- GET -------- */

	bool get(const uint8_t* key, uint8_t key_len, uint8_t* out, uint16_t* size)
	{
        if (!isValid()) return false;

//printf("[ustore] get: fetching key %s with data size %u\n", bin_str(key, key_len), *size);
		if (key_len > USTORE_MAX_KEY_LEN) {
			printf("[ustore] get: failed due to excessive key length: %u\n", key_len);
			return false;
		}

		flush_buffer();

		IndexValue* e = index_find(key, key_len);
		if (!e) {
			printf("[ustore] get: key not found in index\n");
			return false;
		}

		if (is_ttl_expired_(e->timestamp, e->ttl)) {
			index_remove(key, key_len);
			printf("[ustore] get: key expired by TTL\n");
			return false;
		}

		char name[USTORE_MAX_FILENAME_LEN];
		segment_name(e->segment,name);

		File f = _filesystem.open(name, File::ModeRead);
		if (!f) return false;

		f.seek(e->offset, SeekModeSet);

		RecordHeader hdr;

		if (f.read(&hdr, sizeof(hdr)) != sizeof(hdr)) {
			printf("[ustore] get: header read failed\n");
			f.close();
			return false;
		}

		if (hdr.magic != MAGIC_RECORD ||
			hdr.key_len > USTORE_MAX_KEY_LEN ||
			hdr.length > USTORE_MAX_VALUE_LEN)
		{
			printf("[ustore] get: found corrupted record\n");
			f.close();
			return false;
		}

		// CBA If this is only a size request then skip reading value
		if (out != nullptr) {
			f.seek((long)(e->offset+sizeof(hdr)+hdr.key_len), SeekModeSet);
			size_t read = std::min(hdr.length, *size);
			if (f.read(out, read) != read) {
				printf("[ustore] get: value read failed\n");
				f.close();
				return false;
			}
		}

		*size = hdr.length;

		f.close();

printf("[ustore] get: returning key %s with data length %u\n", bin_str(key, key_len), *size);
//printf("[ustore] get: %s\n", bin_str((uint8_t*)out, *size));
		return true;
	}

	inline bool get(const char* key, uint8_t* out, uint16_t* size)
	{
		return get((const uint8_t*)key, (uint8_t)strlen(key), out, size);
	}

	inline bool get(const std::vector<uint8_t>& key, uint8_t* out, uint16_t* size)
	{
		return get(key.data(), (uint8_t)key.size(), out, size);
	}

	inline bool get(const std::vector<uint8_t>& key, std::vector<uint8_t>& out)
	{
		out.resize(USTORE_MAX_VALUE_LEN);
		uint16_t size = USTORE_MAX_VALUE_LEN;
		if (!get(key.data(), (uint8_t)key.size(), out.data(), &size)) {
			return false;
		}
		out.resize(size);
		return true;
	}

	/* -------- REMOVE -------- */

	bool remove(const uint8_t* key, uint8_t key_len)
	{
        if (!isValid()) return false;

		if(key_len > USTORE_MAX_KEY_LEN) return false;

		RecordHeader hdr;
		hdr.magic = MAGIC_RECORD;
		hdr.key_len = key_len;
		hdr.timestamp = 0;
		hdr.length = 0;
		hdr.flags = FLAG_DELETE;
		hdr.crc = crc32(0, (uint8_t*)&hdr, sizeof(hdr)-4);
		hdr.crc = crc32(hdr.crc, key, key_len);

		append_buffer((uint8_t*)&hdr, sizeof(hdr));
		append_buffer(key, key_len);

		RecordCommit c;
		c.magic=MAGIC_COMMIT;

		append_buffer((uint8_t*)&c, sizeof(c));

		flush_buffer();  // ensure tombstone is on flash before committing index entry

		// If an existing record was removed then increment _dead_since_compact
		if (index_find(key, key_len)) _dead_since_compact++;

		index_remove(key, key_len);

		persist_index_entry(key, key_len, 0xFFFFFFFF, 0);

		current_offset += sizeof(RecordHeader) + key_len + sizeof(RecordCommit);

		compact_if_threshold();

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
        if (!isValid()) return false;
		if(key_len > USTORE_MAX_KEY_LEN) return false;
		IndexValue* e = index_find(key, key_len);
		if (!e) return false;
		if (is_ttl_expired_(e->timestamp, e->ttl)) { index_remove(key, key_len); return false; }
		return true;
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

	inline size_t size()
	{
        if (!isValid()) return 0;
		return _index.size();
	}

	/* -------- POLICY -------- */

	inline void set_ttl_secs(uint32_t ttl_s)
	{
		policy_ttl_secs    = ttl_s;
	}

	inline void set_max_recs(uint32_t max_recs)
	{
		policy_max_recs = max_recs;
	}

	/* -------- DUMP INFO -------- */

	void dumpInfo(bool detailed = true)
	{
		flush_buffer();

		uint32_t total_file_size = 0;
		uint32_t total_entries = 0;
		uint32_t total_tomb_entries = 0;
		uint32_t total_tomb_bytes = 0;
		uint32_t total_dead_entries = 0;
		uint32_t total_dead_bytes = 0;

		for(uint32_t seg = 0; seg < USTORE_MAX_SEGMENTS; seg++) {
			char name[USTORE_MAX_FILENAME_LEN];
			segment_name(seg, name);

			File f = _filesystem.open(name, File::ModeRead);
			if (!f) break;  // segments are contiguous; first missing one ends the scan

			uint32_t file_size = 0;
			uint32_t entries = 0;
			uint32_t tomb_entries = 0;
			uint32_t tomb_bytes = 0;
			uint32_t dead_entries = 0;
			uint32_t dead_bytes = 0;

			{
				f.seek(0, SeekModeEnd);
				file_size = (uint32_t)f.tell();
				f.seek(0, SeekModeSet);

				while(true) {
					uint32_t offset = (uint32_t)f.tell();

					RecordHeader hdr;
					if(f.read(&hdr, sizeof(hdr)) != sizeof(hdr)) break;

					if(hdr.magic != MAGIC_RECORD) break;

					if(hdr.key_len > USTORE_MAX_KEY_LEN) break;

					uint8_t key[USTORE_MAX_KEY_LEN];
					if(f.read(key, hdr.key_len) != hdr.key_len) break;

					f.seek((long)(offset + sizeof(hdr) + hdr.key_len + hdr.length), SeekModeSet);

					RecordCommit c;
					if(f.read(&c, sizeof(c)) != sizeof(c)) break;

					if(c.magic != MAGIC_COMMIT) break;

					uint32_t record_size = sizeof(RecordHeader) + hdr.key_len + hdr.length + sizeof(RecordCommit);

					if (hdr.flags & FLAG_DELETE) {
						tomb_entries++;
						tomb_bytes += record_size;
					}
					else {
						IndexValue* iv = index_find(key, hdr.key_len);
						bool live = (iv && iv->segment == seg && iv->offset == offset);
						if(live) {
							entries++;
						}
						else {
							dead_entries++;
							dead_bytes += record_size;
						}
					}
				}

				f.close();
			}

			total_file_size += file_size;
			total_entries += entries;
			total_tomb_entries += tomb_entries;
			total_tomb_bytes += tomb_bytes;
			total_dead_entries += dead_entries;
			total_dead_bytes += dead_bytes;

			if (detailed) {
				bool active = (seg == current_segment);
				printf("%s%s:\n", name, active ? " (ACTIVE)" : "");
				printf("  Bytes       : %8u\n", (unsigned)file_size);
				printf("  Entries     : %8u\n", (unsigned)entries);
				printf("  Tomb bytes  : %8u\n", (unsigned)tomb_bytes);
				printf("  Tomb entries: %8u\n", (unsigned)tomb_entries);
				printf("  Dead bytes  : %8u\n", (unsigned)dead_bytes);
				printf("  Dead entries: %8u\n", (unsigned)dead_entries);
			}
		}

		printf("TOTAL:\n");
		printf("  Bytes       : %8u\n", (unsigned)total_file_size);
		printf("  Entries     : %8u\n", (unsigned)total_entries);
		printf("  Tomb bytes  : %8u\n", (unsigned)total_tomb_bytes);
		printf("  Tomb entries: %8u\n", (unsigned)total_tomb_entries);
		printf("  Dead bytes  : %8u\n", (unsigned)total_dead_bytes);
		printf("  Dead entries: %8u\n", (unsigned)total_dead_entries);
	}

private:

	/* -------- INDEX TYPES (hoisted so iterator can reference them) -------- */

	struct VectorHash {
		template<typename Alloc>
		size_t operator()(const std::vector<uint8_t, Alloc>& v) const {
			uint32_t h = 2166136261u;
			for(uint8_t b : v) { h ^= b; h *= 16777619u; }
			return h;
		}
	};

	struct IndexValue {
		uint32_t segment;
		uint32_t offset;
		uint32_t timestamp;
		uint32_t ttl;
	};

	template<typename T>
	using rebind_alloc = typename std::allocator_traits<Allocator>::template rebind_alloc<T>;

	using byte_alloc_type = rebind_alloc<uint8_t>;
	using KeyType         = std::vector<uint8_t, byte_alloc_type>;
	using MapPairType     = std::pair<const KeyType, IndexValue>;
	using map_alloc_type  = rebind_alloc<MapPairType>;
	using IndexMap = std::unordered_map<
		KeyType, IndexValue, VectorHash, std::equal_to<KeyType>, map_alloc_type>;

	KeyType make_key(const uint8_t* key, uint8_t key_len) const {
		return KeyType(key, key + key_len, byte_alloc_type(_alloc));
	}

public:

	/* -------- ENTRY -------- */

	struct Entry {
		std::vector<uint8_t> key;
		std::vector<uint8_t> value;
		uint32_t timestamp;
		uint32_t ttl;
	};

	/* -------- ITERATOR -------- */

	// Forward iterator over all live key-value records.
	// Walks the in-memory index; key and timestamp are available with zero disk
	// I/O. The value field is lazy-loaded on first dereference (*it or it->)
	// and cached until operator++ advances to the next record.
	// WARNING: Mutating the store during iteration (put/remove/clear/compact) is
	// undefined behaviour — unordered_map iterator invalidation rules apply.
	class iterator {
	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type        = Entry;
		using difference_type   = std::ptrdiff_t;
		using pointer           = const Entry*;
		using reference         = const Entry&;

		// Default constructor — produces a singular (non-dereferenceable) iterator.
		iterator() : store_(nullptr), value_loaded_(false) {}

		// operator*  — full dereference; loads the value from disk on first call
		//              for this position. Use this when you need entry.value.
		// operator-> — metadata-only; returns key and timestamp (already in memory)
		//              without touching disk. entry.value will be empty unless
		//              operator* was already called for this position.
		reference operator*()  const { if (!value_loaded_) load_value(); return current_; }
		pointer   operator->() const { return &current_; }

		iterator& operator++() {
			++pos_;
			load_meta();
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
		friend class BasicFileStore<Allocator>;

		using idx_iter = typename IndexMap::iterator;

		iterator(BasicFileStore* store, idx_iter pos, idx_iter end)
			: store_(store), pos_(pos), end_(end), value_loaded_(false)
		{
			load_meta();
		}

		// Populate key and timestamp from the in-memory index. Zero disk I/O.
		// Resets value_loaded_ so the next dereference re-reads from disk.
		void load_meta() {
			if (pos_ == end_) return;
			iv_                = pos_->second;
			current_.key.assign(pos_->first.begin(), pos_->first.end());
			current_.timestamp = iv_.timestamp;
			current_.ttl       = iv_.ttl;
			current_.value.clear();
			value_loaded_      = false;
		}

		// Read the value from disk into current_.value. Called lazily by
		// operator* / operator->. Marked const so it can mutate mutable members.
		void load_value() const {
			char name[USTORE_MAX_FILENAME_LEN];
			store_->segment_name(iv_.segment, name);

			File f = store_->_filesystem.open(name, File::ModeRead);
			value_loaded_ = true;
			if (!f) { current_.value.clear(); return; }

			f.seek((long)iv_.offset, SeekModeSet);
			RecordHeader hdr;
			f.read(&hdr, sizeof(hdr));
			f.seek((long)(iv_.offset + sizeof(hdr) + hdr.key_len), SeekModeSet);
			current_.value.resize(hdr.length);
			if (hdr.length > 0)
				f.read(current_.value.data(), hdr.length);
			f.close();
		}

		BasicFileStore*      store_;
		idx_iter             pos_;
		idx_iter             end_;
		IndexValue           iv_;
		mutable bool         value_loaded_;
		mutable Entry        current_;
	};

	/* -------- BEGIN / END -------- */

	iterator begin() {
		flush_buffer();   // ensure pending writes are visible before iterating
		return iterator(this, _index.begin(), _index.end());
	}

	iterator end() {
		return iterator(this, _index.end(), _index.end());
	}

private:

	/* -------- BUFFERED WRITES -------- */

	void append_buffer(const uint8_t* data, size_t len)
	{
		const uint8_t* p=(const uint8_t*)data;

		while(len)
		{
			size_t space = USTORE_WRITE_BUFFER - write_buf_pos;
			size_t n = (len < space) ? len : space;

			memcpy(write_buf+write_buf_pos, p, n);

			write_buf_pos += n;
			p += n;
			len -= n;

			if(write_buf_pos == USTORE_WRITE_BUFFER)
				flush_buffer();
		}
	}

	bool flush_buffer()
	{
		if (!active_file) {
			printf("[ustore] ERROR: Active file is not valid, failed to flush buffer\n");
			// CBA Must reset buffer pos to avoid an infinite flushing loop
			write_buf_pos = 0;
			return false;
		}
		active_file.write(write_buf, write_buf_pos);
		active_file.flush();
		write_buf_pos = 0;
		return true;
	}

	/* -------- INDEX -------- */

	IndexValue* index_find(const uint8_t* key, uint8_t key_len)
	{
		auto it = _index.find(make_key(key, key_len));
		return (it != _index.end()) ? &it->second : nullptr;
	}

	void index_insert(const uint8_t* key, uint8_t key_len, uint32_t seg, uint32_t off, uint32_t ts = 0, uint32_t ttl = 0)
	{
		IndexValue& iv = _index[make_key(key, key_len)];
		iv.segment   = seg;
		iv.offset    = off;
		iv.timestamp = ts;
		iv.ttl       = ttl;
	}

	void index_remove(const uint8_t* key, uint8_t key_len)
	{
		_index.erase(make_key(key, key_len));
	}

	/* -------- POLICY HELPERS -------- */

	bool is_ttl_expired_(uint32_t ts, uint32_t record_ttl) const
	{
		uint32_t effective_ttl = (record_ttl > 0) ? record_ttl : policy_ttl_secs;
		return effective_ttl > 0 && microStore::time() > ts && (microStore::time() - ts) >= effective_ttl;
	}

	// Evict the oldest records (by timestamp) until _index.size() <= policy_max_recs.
	// Returns the number of entries evicted.
	// When to_evict == 1 (the common put() path), a simple O(n) scan is used to
	// avoid heap allocation.  When to_evict > 1 (init / compact), a partial sort
	// is used to evict all excess entries in a single pass.
	size_t prune_index_to_max_recs_()
	{
		if (policy_max_recs == 0 || _index.size() <= policy_max_recs) return 0;

		size_t to_evict = _index.size() - policy_max_recs;

		if (to_evict == 1) {
			// Fast path: single linear scan for the oldest entry.
			auto oldest = _index.begin();
			for (auto it = _index.begin(); it != _index.end(); ++it)
				if (it->second.timestamp < oldest->second.timestamp) oldest = it;
			_index.erase(oldest);
		} else {
			// Bulk path: collect (timestamp, key) pairs, partial-sort, then erase.
			using KTSPair = std::pair<uint32_t, KeyType>;
			using KTSAlloc = rebind_alloc<KTSPair>;
			KTSAlloc kts_alloc(_alloc);
			std::vector<KTSPair, KTSAlloc> candidates(kts_alloc);
			candidates.reserve(_index.size());
			for (auto& kv : _index)
				candidates.push_back(std::make_pair(kv.second.timestamp, kv.first));
			std::partial_sort(candidates.begin(), candidates.begin() + (long)to_evict, candidates.end(),
				[](const KTSPair& a, const KTSPair& b){ return a.first < b.first; });
			for (size_t i = 0; i < to_evict; i++) {
				_index.erase(candidates[i].second);
			}
		}
		// Record(s) evicted so increment _dead_since_compact
		_dead_since_compact += to_evict;
printf("[ustore] Evicted %lu records to policy_max_recs\n", to_evict);

		return to_evict;
	}

	/* -------- INDEX FILE -------- */

	bool persist_index_entry(const uint8_t* key, uint8_t key_len, uint32_t seg, uint32_t off, uint32_t ts = 0, uint32_t ttl = 0)
	{
		if (!index_file) {
			printf("[ustore] ERROR: Index file is not valid\n");
			return false;
		}

		index_file.write(&key_len, 1);
		index_file.write(key, key_len);
		index_file.write(&seg, 4);
		index_file.write(&off, 4);
		index_file.write(&ts, 4);
		index_file.write(&ttl, 4);
		index_file.flush();  // explicit fsync — guarantees entry reaches flash

		return true;
	}

	bool write_index_bulk()
	{
		char name[USTORE_MAX_FILENAME_LEN];
		index_name(name);

		File f = _filesystem.open(name, File::ModeAppend);
		if (!f) return false;

		for (auto& kv : _index)
		{
			uint8_t  key_len = (uint8_t)kv.first.size();
			uint32_t seg     = kv.second.segment;
			uint32_t off     = kv.second.offset;
			uint32_t ts      = kv.second.timestamp;

			uint32_t ttl     = kv.second.ttl;

			f.write(&key_len, 1);
			f.write(kv.first.data(), key_len);
			f.write(&seg, 4);
			f.write(&off, 4);
			f.write(&ts, 4);
			f.write(&ttl, 4);
		}

		f.flush();
		f.close();
		return true;
	}

	bool load_index()
	{
		char name[USTORE_MAX_FILENAME_LEN];
		index_name(name);

		File f = _filesystem.open(name, File::ModeRead);
		if (!f) return false;

		while(true)
		{
			uint8_t key_len;
			uint8_t key[USTORE_MAX_KEY_LEN];
			uint32_t seg;
			uint32_t off;

			if(f.read(&key_len, 1) != 1) break;
			if(key_len > USTORE_MAX_KEY_LEN) break;
			if(f.read(key, key_len) != key_len) break;
			if(f.read(&seg, 4) != 4) break;
			if(f.read(&off, 4) != 4) break;
			uint32_t ts = 0;
			if(f.read(&ts, 4) != 4) break;
			uint32_t ttl = 0;
			if(f.read(&ttl, 4) != 4) break;

			// seg==0xFFFFFFFF is a deletion sentinel written by remove()
			if(seg==0xFFFFFFFF)
				index_remove(key, key_len);
			else
				index_insert(key, key_len, seg, off, ts, ttl);
		}

		f.close();

		return true;
	}

	/* -------- INDEX FILE OPEN HELPER -------- */

	void open_index_for_append()
	{
		char name[USTORE_MAX_FILENAME_LEN];
		index_name(name);
		index_file = _filesystem.open(name, File::ModeAppend);
	}

	const char* bin_str(const uint8_t* key, size_t len)
	{
		static char str[USTORE_MAX_VALUE_LEN*2+1];
		int n = 0;
		for (int i = 0; i < len && i < USTORE_MAX_VALUE_LEN; ++i) {
			//if (key[i] > 31 && key[i] < 127) {
			//	str[n++] = key[i];
			//}
			//else {
				snprintf(str+n, sizeof(str)-n, "%02x", key[i]);
				n += 2;
			//}
		}
		str[n] = 0;
		return str;
	}

	/* -------- SEGMENTS -------- */

	void segment_name(uint32_t id,char* out)
	{
		snprintf(out, USTORE_MAX_FILENAME_LEN, "%s_%u.dat", base_prefix,id);
	}

	void index_name(char* out)
	{
		snprintf(out, USTORE_MAX_FILENAME_LEN, "%s_index.dat", base_prefix);
	}

	void journal_name(char* out)
	{
		snprintf(out, USTORE_MAX_FILENAME_LEN, "%s_journal.dat", base_prefix);
	}

	bool open_segment(uint32_t id)
	{
		if (active_file) active_file.close();

		char name[USTORE_MAX_FILENAME_LEN];
		segment_name(id,name);

printf("[ustore] Opening active file: %s\n", name);
		active_file = _filesystem.open(name, File::ModeReadAppend);
		if (!active_file) {
			printf("[ustore] ERROR: Failed to open active file: %s\n", name);
			return false;
		}
		current_segment=id;
		active_file.seek(0,SeekModeEnd);
		current_offset=active_file.tell();
		return true;
	}

	bool rotate_segment_if_needed(uint32_t write_size)
	{
		if (current_offset + write_size + sizeof(RecordHeader) + sizeof(RecordCommit) < USTORE_SEGMENT_SIZE)
			return true;

printf("[ustore] Rotating segment...\n");
		flush_buffer();

		if (active_file) active_file.close();

		current_segment++;

		if (current_segment >= USTORE_MAX_SEGMENTS) {
			if (compact_in_cooldown &&
				(microStore::millis() - compact_cooldown_start_ms) < USTORE_COMPACT_RETRY_MS)
			{
				printf("[ustore] Compact skipped: cooldown active\n");
				current_segment = USTORE_MAX_SEGMENTS - 1;
				open_segment(current_segment);
				return false;
			}
			if (!compact()) {
				compact_in_cooldown       = true;
				compact_cooldown_start_ms = microStore::millis();
				current_segment = USTORE_MAX_SEGMENTS - 1;
				open_segment(current_segment);
				return false;
			}
			compact_in_cooldown = false;
			current_segment = 1;  // seg0 = compacted data; fresh writes start at seg1
		}

		open_segment(current_segment);
		return true;
	}

	/* -------- JOURNAL HELPERS -------- */

	void write_journal(uint32_t state, uint32_t next_seg = 0, uint32_t tmp_valid_size = 0)
	{
		char name[USTORE_MAX_FILENAME_LEN]; journal_name(name);
		File f = _filesystem.open(name, File::ModeWrite);
		if (!f) return;
		Journal j;
		j.magic          = JOURNAL_MAGIC;
		j.state          = state;
		j.next_seg       = next_seg;
		j.tmp_valid_size = tmp_valid_size;
		f.write(&j, sizeof(j));
		f.flush();
		f.close();
	}

	void clear_journal()
	{
		char name[USTORE_MAX_FILENAME_LEN]; journal_name(name);
		_filesystem.remove(name);
	}

	void compact_if_threshold()
	{
#if USTORE_COMPACT_THRESHOLD > 0
		uint32_t total = (uint32_t)_index.size() + _dead_since_compact;
		if (total > 0 && _dead_since_compact * 100 / total >= USTORE_COMPACT_THRESHOLD) {
printf("[ustore] Compaction triggered by deleted threshold\n");
			if (compact()) {
				// After threshold-triggered compaction, seg0 holds the compacted data.
				// Open seg1 for new writes, mirroring what rotate_segment_if_needed() does
				// after a rotation-triggered compaction.
				current_segment = 1;
				open_segment(current_segment);
			}
		}
#endif
	}

	void recover_if_needed()
	{
		char name[USTORE_MAX_FILENAME_LEN]; journal_name(name);
		File f = _filesystem.open(name, File::ModeRead);
		if (!f) return;

		Journal j;
		size_t n = f.read(&j, sizeof(j));
		f.close();

		if (n != sizeof(j) || j.magic != JOURNAL_MAGIC) {
			_filesystem.remove(name);
			return;
		}

		char tmp_name[USTORE_MAX_FILENAME_LEN]; snprintf(tmp_name, sizeof(tmp_name), "%s_compact.tmp", base_prefix);

		if (j.state == JOURNAL_COMMIT) {
			// Tmp file is complete — finish the swap.
			finalize_compaction();
		} else if (j.next_seg == 0) {
			// COMPACTING, no source segments deleted yet — tmp may be partial, discard it.
			// All original segments are intact; normal boot proceeds.
			_filesystem.remove(tmp_name);
		} else {
			// COMPACTING with next_seg > 0: source segments 0..next_seg-1 have been deleted.
			// compact.tmp holds their live records up to byte tmp_valid_size (bytes beyond
			// that are either extra valid records from a flushed-but-not-journaled segment,
			// or a partial write stopped by the crash — both are handled harmlessly by the
			// segment scan in rebuild_index_from_segments()).
			//
			// Recovery strategy: rename compact.tmp → segment_0.dat so that the normal
			// boot index rebuild picks it up alongside the surviving segments next_seg..7.
			// The next compaction cycle will consolidate everything correctly.
			char seg0[USTORE_MAX_FILENAME_LEN]; segment_name(0, seg0);
			if (!_filesystem.rename(tmp_name, seg0)) {
				// Rename failed (e.g. filesystem error). Best effort: leave compact.tmp
				// in place; the next boot will retry this same recovery path.
				return;  // keep journal so next boot retries
			}
		}

		_filesystem.remove(name);  // clear journal
	}

	/* -------- FINALIZE COMPACTION -------- */

	void finalize_compaction()
	{
		char tmp_name[USTORE_MAX_FILENAME_LEN]; snprintf(tmp_name, sizeof(tmp_name), "%s_compact.tmp", base_prefix);
		char seg0[USTORE_MAX_FILENAME_LEN];     segment_name(0, seg0);

		// Remove all existing segments, then rename tmp → seg0.
		for (uint32_t i = 0; i < USTORE_MAX_SEGMENTS; i++) {
			char sname[USTORE_MAX_FILENAME_LEN]; segment_name(i, sname);
			_filesystem.remove(sname);
		}
		if (!_filesystem.rename(tmp_name, seg0)) {
			_filesystem.remove(tmp_name);
			return;
		}

		// Rebuild in-memory index by scanning seg0.
		_index.clear();
		File f = _filesystem.open(seg0, File::ModeRead);
		if (f) {
			uint32_t scan_offset = 0;
			uint8_t key_buf[USTORE_MAX_KEY_LEN];
			while (true) {
				RecordHeader hdr;
				if (f.read(&hdr, sizeof(hdr)) != sizeof(hdr)) break;
				if (hdr.magic != MAGIC_RECORD || hdr.key_len == 0 ||
					hdr.key_len > USTORE_MAX_KEY_LEN || hdr.length > USTORE_MAX_VALUE_LEN) break;
				if (f.read(key_buf, hdr.key_len) != hdr.key_len) break;
				f.seek((long)(scan_offset + sizeof(hdr) + hdr.key_len + hdr.length), SeekModeSet);
				RecordCommit c;
				if (f.read(&c, sizeof(c)) != sizeof(c)) break;
				if (c.magic != MAGIC_COMMIT) break;
				if (!(hdr.flags & FLAG_DELETE))
					index_insert(key_buf, hdr.key_len, 0, scan_offset, hdr.timestamp, hdr.ttl);
				scan_offset += sizeof(hdr) + hdr.key_len + hdr.length + sizeof(c);
			}
			f.close();
		}

		// Rebuild persistent index from the now-correct in-memory index.
		// Use write_index_bulk() — single flush for all entries — not persist_index_entry()
		// which flushes after every entry and would cause N × fsync delays on flash.
		char iname[USTORE_MAX_FILENAME_LEN]; index_name(iname);
		if (index_file) index_file.close();
		_filesystem.remove(iname);
		write_index_bulk();
		open_index_for_append();

		current_segment = 0;
		// current_offset is set by open_segment() which the caller will invoke next.
	}

	/* -------- INDEX REBUILD FROM LOG -------- */

	// Called when the index file is missing. Scans all segment files in write
	// order to reconstruct the in-memory index, then persists it so future
	// boots are fast.  Uses the same record-walk logic as finalize_compaction().
	void rebuild_index_from_segments()
	{
		printf("[ustore] Index missing — rebuilding from segment files...\n");
		_index.clear();

		for (uint32_t seg = 0; seg < USTORE_MAX_SEGMENTS; seg++)
		{
			char sname[USTORE_MAX_FILENAME_LEN];
			segment_name(seg, sname);
			File f = _filesystem.open(sname, File::ModeRead);
			if (!f) continue;

			uint32_t scan_offset = 0;
			uint8_t  key_buf[USTORE_MAX_KEY_LEN];
			while (true)
			{
				RecordHeader hdr;
				if (f.read(&hdr, sizeof(hdr)) != sizeof(hdr)) break;
				if (hdr.magic != MAGIC_RECORD || hdr.key_len == 0 ||
					hdr.key_len > USTORE_MAX_KEY_LEN || hdr.length > USTORE_MAX_VALUE_LEN) break;
				if (f.read(key_buf, hdr.key_len) != hdr.key_len) break;
				f.seek((long)(scan_offset + sizeof(hdr) + hdr.key_len + hdr.length), SeekModeSet);
				RecordCommit c;
				if (f.read(&c, sizeof(c)) != sizeof(c)) break;
				if (c.magic != MAGIC_COMMIT) break;
				if (hdr.flags & FLAG_DELETE)
					index_remove(key_buf, hdr.key_len);
				else
					index_insert(key_buf, hdr.key_len, seg, scan_offset, hdr.timestamp, hdr.ttl);
				scan_offset += sizeof(hdr) + hdr.key_len + hdr.length + sizeof(c);
			}
			f.close();
		}

		// Persist the rebuilt index so the next boot uses the fast path.
		char iname[USTORE_MAX_FILENAME_LEN]; index_name(iname);
		if (index_file) index_file.close();
		_filesystem.remove(iname);
		write_index_bulk();
	}

	/* -------- COMPACTION -------- */

public:
	bool compact()
	{
printf("[ustore] Compacting storage...\n");

		// --- Phase 1: write COMPACTING journal (next_seg=0: no source segments deleted yet) ---
		write_journal(JOURNAL_COMPACTING, 0, 0);

		char tmp_name[USTORE_MAX_FILENAME_LEN]; snprintf(tmp_name, sizeof(tmp_name), "%s_compact.tmp", base_prefix);
printf("[ustore] Opening tmp file: %s\n", tmp_name);
		File outf = _filesystem.open(tmp_name, File::ModeWrite);
		if (!outf) { clear_journal(); return false; }

		// --- Phase 2: build per-segment sorted lists of live offsets ---

		// Apply policies before building the live-record lists so that expired
		// and excess records are excluded from the compaction output.
		prune_index_to_max_recs_();

		struct LiveRec {
			uint32_t offset;
			KeyType key;
		};
		using LiveRecVec = std::vector<LiveRec, rebind_alloc<LiveRec>>;
		rebind_alloc<LiveRec> lr_alloc(_alloc);
		LiveRecVec per_seg[USTORE_MAX_SEGMENTS];
		for (auto& v : per_seg) {
			v = LiveRecVec(lr_alloc);
		}

		for (auto& kv : _index) {
printf("[ustore] Calling is_ttl_expired_ with ttl: %u, ts: %u, now: %u\n", kv.second.ttl, kv.second.timestamp, microStore::time());
			if (is_ttl_expired_(kv.second.timestamp, kv.second.ttl)) continue;
			uint32_t seg = kv.second.segment;
			if (seg < USTORE_MAX_SEGMENTS) {
				LiveRec lr;
				lr.offset = kv.second.offset;
				lr.key = kv.first;
printf("[ustore] Pushing segment: %u, offset: %u\n", seg, lr.offset);
				per_seg[seg].push_back(lr);
			}
		}
		for (uint32_t s = 0; s < USTORE_MAX_SEGMENTS; s++) {
			std::sort(per_seg[s].begin(), per_seg[s].end(), [](const LiveRec& a, const LiveRec& b) {
				return a.offset < b.offset;
			});
		}

		// --- Phase 3: process segments one at a time ---
		// For each segment s:
		//   1. Copy live records from s to compact.tmp.
		//   2. Flush compact.tmp to disk.
		//   3. Write journal (next_seg=s+1, tmp_valid_size=current byte count).
		//   4. Delete source segment s.
		// Steps 3 and 4 MUST be in this order: the journal is updated BEFORE the source
		// segment is deleted. If power fails between step 3 and step 4, the next boot's
		// recover_if_needed() sees next_seg=s+1 and renames compact.tmp to segment 0, then
		// finalize_compaction() deletes the still-present segment s — no data is lost.
		//
		// Static to avoid placing 1 KB on the stack — compact() is not re-entrant.
		uint8_t key_buf[USTORE_MAX_KEY_LEN];
		static uint8_t val_buf[USTORE_MAX_VALUE_LEN];
		bool write_ok = true;
		uint32_t committed_segs = 0;  // number of source segments committed to compact.tmp

		for (uint32_t s = 0; s < USTORE_MAX_SEGMENTS; s++) {
			char src_name[USTORE_MAX_FILENAME_LEN]; segment_name(s, src_name);
printf("[ustore] Processing segment: %u, size: %lu\n", s, per_seg[s].size());

			if (!per_seg[s].empty()) {
printf("[ustore] Opening src file: %s\n", src_name);
				File src = _filesystem.open(src_name, File::ModeRead);
				if (src) {
					for (size_t i = 0; i < per_seg[s].size(); i++) {
						LiveRec& lr = per_seg[s][i];
						src.seek((long)lr.offset, SeekModeSet);
						RecordHeader hdr;
						if (src.read(&hdr, sizeof(hdr)) != sizeof(hdr)) continue;
						if (hdr.magic != MAGIC_RECORD || hdr.key_len > USTORE_MAX_KEY_LEN ||
							hdr.length > USTORE_MAX_VALUE_LEN) continue;
						if (src.read(key_buf, hdr.key_len) != hdr.key_len) continue;
						if (hdr.length > 0 && src.read(val_buf, hdr.length) != hdr.length) continue;
						RecordCommit c; c.magic = MAGIC_COMMIT;
						uint32_t expected = sizeof(hdr) + hdr.key_len + hdr.length + sizeof(c);
						size_t written = 0;
						written += outf.write(&hdr,    sizeof(hdr));
						written += outf.write(key_buf, hdr.key_len);
						if (hdr.length > 0) written += outf.write(val_buf, hdr.length);
						written += outf.write(&c, sizeof(c));
						if (written != expected) { write_ok = false; break; }
					}
printf("[ustore] Closing src file: %s\n", src_name);
					src.close();
				}
			}

			if (!write_ok) break;

			// Flush compact.tmp, journal BEFORE deleting the source segment (see comment above).
			outf.flush();
			uint32_t valid_size = (uint32_t)outf.tell();
			write_journal(JOURNAL_COMPACTING, s + 1, valid_size);
			_filesystem.remove(src_name);  // no-op if segment had no records / did not exist
			committed_segs++;
		}

		outf.flush();
printf("[ustore] Closing tmp file: %s\n", tmp_name);
		outf.close();

		if (!write_ok) {
			if (committed_segs == 0) {
				// No source segments were deleted — safe to discard compact.tmp entirely.
				printf("[ustore] Compact aborted: storage full, all segments preserved\n");
				_filesystem.remove(tmp_name);
				clear_journal();
			} else {
				// Some source segments were already deleted; compact.tmp holds their records.
				// Leave compact.tmp and the journal in place. recover_if_needed() on the
				// next boot will rename compact.tmp to segment 0 and recover cleanly.
				printf("[ustore] Compact aborted mid-way after %u segments: recovery on next boot\n",
				       committed_segs);
			}
			return false;
		}

		// --- Phase 4: commit journal → safe to finalize ---
		write_journal(JOURNAL_COMMIT);

		finalize_compaction();   // rename + index rebuild

		clear_journal();

		_dead_since_compact = 0;

		return true;
	}

private:

	FileSystem _filesystem;

	char base_prefix[32];

	File active_file;
	File index_file;

	uint32_t current_segment = 0;
	uint32_t current_offset = 0;

	bool compact_in_cooldown = false;
	uint32_t compact_cooldown_start_ms = 0;
	uint32_t _dead_since_compact = 0;

	uint32_t policy_ttl_secs = USTORE_DEFAULT_TTL_SECS; // 0 = TTL disabled (seconds)
	uint32_t policy_max_recs = USTORE_DEFAULT_MAX_RECS; // 0 = max-records disabled

	uint8_t write_buf[USTORE_WRITE_BUFFER];
	size_t write_buf_pos;

	Allocator _alloc;
	IndexMap _index;
};


using FileStore = BasicFileStore<>;

}
