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

#ifndef UFSKV_SEGMENT_SIZE
//#define UFSKV_SEGMENT_SIZE (1024 * 1024)
#define UFSKV_SEGMENT_SIZE (64 * 1024)
#endif

#ifndef UFSKV_MAX_SEGMENTS
#define UFSKV_MAX_SEGMENTS 8
#endif

#ifndef UFSKV_WRITE_BUFFER
#define UFSKV_WRITE_BUFFER 4096
#endif

#ifndef USTORE_MAX_KEY_LEN
#define USTORE_MAX_KEY_LEN 64
#endif

#ifndef USTORE_MAX_FILENAME_LEN
#define USTORE_MAX_FILENAME_LEN 64
#endif

#ifndef UFSKV_COMPACT_RETRY_MS
//#define UFSKV_COMPACT_RETRY_MS 5000   // ms between compact() retries after failure
#define UFSKV_COMPACT_RETRY_MS 60000   // ms between compact() retries after failure
#endif

/* ---------------- CONSTANTS ---------------- */

static const uint32_t MAGIC_RECORD  = 0xC0DEC0DE;
static const uint32_t MAGIC_COMMIT  = 0xFACEB00C;
static const uint16_t FLAG_DELETE   = 1;
static const uint32_t JOURNAL_MAGIC = 0x4B564A4E;

enum JournalState { JOURNAL_NONE = 0, JOURNAL_COMPACTING = 1, JOURNAL_COMMIT = 2 };

#pragma pack(push,1)
struct Journal { uint32_t magic; uint32_t state; };
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
		: alloc_(alloc), index(map_alloc_type(alloc_)) { write_buf_pos = 0; }

	allocator_type get_allocator() const { return alloc_; }

	bool init(FileSystem& filesystem, const char* prefix)
	{
		fs = filesystem;
		strncpy(base_prefix,prefix,sizeof(base_prefix));

		recover_if_needed();

		if (!load_index())
			rebuild_index_from_segments();

		open_index_for_append();

		// Resume at the highest segment that exists on flash, not always seg 0.
		// Without this, after reset current_segment stays 0 and new writes
		// overwrite previously-live segments, losing all their records.
		uint32_t resume_seg = 0;
		for (uint32_t i = 0; i < UFSKV_MAX_SEGMENTS; i++)
		{
			char name[USTORE_MAX_FILENAME_LEN];
			segment_name(i, name);
			File f = fs.open(name, File::ModeRead);
			if (f) { f.close(); resume_seg = i; }
		}

		open_segment(resume_seg);

		return true;
	}

	void clear()
	{
		char name[USTORE_MAX_FILENAME_LEN];

		if (index_file) index_file.close();

		for(uint32_t i=0;i<UFSKV_MAX_SEGMENTS;i++)
		{
			segment_name(i,name);
			fs.remove(name);
		}

		index_name(name);
		fs.remove(name);

		index.clear();
		current_segment=0;
		current_offset=0;
		write_buf_pos=0;

		open_index_for_append();
		open_segment(0);
	}

	/* -------- PUT -------- */

	bool put(const uint8_t* key, uint8_t key_len, const uint8_t* data, uint16_t len, uint32_t ts = ustore_time())
	{
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

		index_insert(key, key_len, current_segment, offset, ts);

		persist_index_entry(key, key_len, current_segment, offset, ts);

		current_offset += sizeof(hdr)+key_len+len+sizeof(c);

printf("[ustore] Successfully put key %s with data length %u\n", bin_str(key, key_len), len);
//printf("[ustore] put: %s\n", bin_str((uint8_t*)data, len));
		return true;
	}

	inline bool put(const char* key, const uint8_t* data, uint16_t len, uint32_t ts = ustore_time())
	{
		return put((const uint8_t*)key, (uint8_t)strlen(key), data, len, ts);
	}

	inline bool put(const char* key, const char* data, uint32_t ts = ustore_time())
	{
		return put((const uint8_t*)key, (uint8_t)strlen(key), (const uint8_t*)data, (uint16_t)strlen(data), ts);
	}

	inline bool put(const std::vector<uint8_t>& key, const uint8_t* data, uint16_t len, uint32_t ts = ustore_time())
	{
		return put(key.data(), (uint8_t)key.size(), data, len, ts);
	}

	inline bool put(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data, uint32_t ts = ustore_time())
	{
		return put(key.data(), (uint8_t)key.size(), data.data(), (uint16_t)data.size(), ts);
	}

	/* -------- GET -------- */

	bool get(const uint8_t* key, uint8_t key_len, uint8_t* out, uint16_t* size)
	{
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

		char name[USTORE_MAX_FILENAME_LEN];
		segment_name(e->segment,name);

		File f = fs.open(name, File::ModeRead);
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

		index_remove(key, key_len);

		persist_index_entry(key, key_len, 0xFFFFFFFF, 0);

		current_offset += sizeof(RecordHeader) + key_len + sizeof(RecordCommit);

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
		if(key_len > USTORE_MAX_KEY_LEN) return false;

		IndexValue* e = index_find(key, key_len);
		if (!e) return false;
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
		return index.size();
	}

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
		flush_buffer();

		uint32_t total_file_size = 0;
		uint32_t total_entries = 0;
		uint32_t total_tomb_entries = 0;
		uint32_t total_tomb_bytes = 0;
		uint32_t total_dead_entries = 0;
		uint32_t total_dead_bytes = 0;

			for(uint32_t seg = 0; seg < UFSKV_MAX_SEGMENTS; seg++)
		{
			char name[USTORE_MAX_FILENAME_LEN];
			segment_name(seg, name);

			File f = fs.open(name, File::ModeRead);
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

				while(true)
				{
					uint32_t offset = (uint32_t)f.tell();

					RecordHeader hdr;
					if(f.read(&hdr, sizeof(hdr)) != sizeof(hdr))
						break;

					if(hdr.magic != MAGIC_RECORD)
						break;

					if(hdr.key_len > USTORE_MAX_KEY_LEN)
						break;

					uint8_t key[USTORE_MAX_KEY_LEN];
					if(f.read(key, hdr.key_len) != hdr.key_len)
						break;

					f.seek((long)(offset + sizeof(hdr) + hdr.key_len + hdr.length), SeekModeSet);

					RecordCommit c;
					if(f.read(&c, sizeof(c)) != sizeof(c))
						break;

					if(c.magic != MAGIC_COMMIT)
						break;

					uint32_t record_size = sizeof(RecordHeader) + hdr.key_len + hdr.length + sizeof(RecordCommit);

					if(hdr.flags & FLAG_DELETE)
					{
						tomb_entries++;
						tomb_bytes += record_size;
					}
					else
					{
						IndexValue* iv = index_find(key, hdr.key_len);
						bool live = (iv && iv->segment == seg && iv->offset == offset);
						if(live)
							entries++;
						else
						{
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

	// Forward iterator over all live key-value records.
	// Walks the in-memory index and reads one record's value from disk per step.
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
		iterator() : store_(nullptr) {}

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
		friend class BasicFileStore<Allocator>;

		using idx_iter = typename IndexMap::iterator;

		iterator(BasicFileStore* store, idx_iter pos, idx_iter end)
			: store_(store), pos_(pos), end_(end)
		{
			load();
		}

		void load() {
			if (pos_ == end_) return;
			const IndexValue& iv = pos_->second;
			current_.key       = pos_->first;
			current_.timestamp = iv.timestamp;

			char name[USTORE_MAX_FILENAME_LEN];
			store_->segment_name(iv.segment, name);

			File f = store_->fs.open(name, File::ModeRead);
			if (!f) { current_.value.clear(); return; }

			f.seek((long)iv.offset, SeekModeSet);
			RecordHeader hdr;
			f.read(&hdr, sizeof(hdr));
			f.seek((long)(iv.offset + sizeof(hdr) + hdr.key_len), SeekModeSet);
			current_.value.resize(hdr.length);
			if (hdr.length > 0)
				f.read(current_.value.data(), hdr.length);
			f.close();
		}

		BasicFileStore* store_;
		idx_iter pos_;
		idx_iter end_;
		Entry current_;
	};

	/* -------- BEGIN / END -------- */

	iterator begin() {
		flush_buffer();   // ensure pending writes are visible before iterating
		return iterator(this, index.begin(), index.end());
	}

	iterator end() {
		return iterator(this, index.end(), index.end());
	}

private:

	/* -------- BUFFERED WRITES -------- */

	void append_buffer(const uint8_t* data, size_t len)
	{
		const uint8_t* p=(const uint8_t*)data;

		while(len)
		{
			size_t space = UFSKV_WRITE_BUFFER - write_buf_pos;
			size_t n = (len < space) ? len : space;

			memcpy(write_buf+write_buf_pos, p, n);

			write_buf_pos += n;
			p += n;
			len -= n;

			if(write_buf_pos == UFSKV_WRITE_BUFFER)
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
		return false;
	}

	/* -------- INDEX -------- */

	IndexValue* index_find(const uint8_t* key, uint8_t key_len)
	{
		auto it = index.find(make_key(key, key_len));
		return (it != index.end()) ? &it->second : nullptr;
	}

	void index_insert(const uint8_t* key, uint8_t key_len, uint32_t seg, uint32_t off, uint32_t ts = 0)
	{
		IndexValue& iv = index[make_key(key, key_len)];
		iv.segment   = seg;
		iv.offset    = off;
		iv.timestamp = ts;
	}

	void index_remove(const uint8_t* key, uint8_t key_len)
	{
		index.erase(make_key(key, key_len));
	}

	/* -------- INDEX FILE -------- */

	bool persist_index_entry(const uint8_t* key, uint8_t key_len, uint32_t seg, uint32_t off, uint32_t ts = 0)
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
		index_file.flush();  // explicit fsync — guarantees entry reaches flash

		return true;
	}

	bool write_index_bulk()
	{
		char name[USTORE_MAX_FILENAME_LEN];
		index_name(name);

		File f = fs.open(name, File::ModeAppend);
		if (!f) return false;

		for (auto& kv : index)
		{
			uint8_t  key_len = (uint8_t)kv.first.size();
			uint32_t seg     = kv.second.segment;
			uint32_t off     = kv.second.offset;
			uint32_t ts      = kv.second.timestamp;

			f.write(&key_len, 1);
			f.write(kv.first.data(), key_len);
			f.write(&seg, 4);
			f.write(&off, 4);
			f.write(&ts, 4);
		}

		f.flush();
		f.close();
		return true;
	}

	bool load_index()
	{
		char name[USTORE_MAX_FILENAME_LEN];
		index_name(name);

		File f = fs.open(name, File::ModeRead);
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

			// seg==0xFFFFFFFF is a deletion sentinel written by remove()
			if(seg==0xFFFFFFFF)
				index_remove(key, key_len);
			else
				index_insert(key, key_len, seg, off, ts);
		}

		f.close();

		return true;
	}

	/* -------- INDEX FILE OPEN HELPER -------- */

	void open_index_for_append()
	{
		char name[USTORE_MAX_FILENAME_LEN];
		index_name(name);
		index_file = fs.open(name, File::ModeAppend);
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
		active_file = fs.open(name, File::ModeReadAppend);
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
		if (current_offset + write_size + sizeof(RecordHeader) + sizeof(RecordCommit) < UFSKV_SEGMENT_SIZE)
			return true;

printf("[ustore] Rotating segment...\n");
		flush_buffer();

		if (active_file) active_file.close();

		current_segment++;

		if (current_segment >= UFSKV_MAX_SEGMENTS) {
			if (compact_in_cooldown &&
				(ustore_millis() - compact_cooldown_start_ms) < UFSKV_COMPACT_RETRY_MS)
			{
				printf("[ustore] Compact skipped: cooldown active\n");
				current_segment = UFSKV_MAX_SEGMENTS - 1;
				open_segment(current_segment);
				return false;
			}
			if (!compact()) {
				compact_in_cooldown       = true;
				compact_cooldown_start_ms = ustore_millis();
				current_segment = UFSKV_MAX_SEGMENTS - 1;
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

	void write_journal(uint32_t state)
	{
		char name[USTORE_MAX_FILENAME_LEN]; journal_name(name);
		File f = fs.open(name, File::ModeWrite);
		if (!f) return;
		Journal j; j.magic = JOURNAL_MAGIC; j.state = state;
		f.write(&j, sizeof(j));
		f.flush();
		f.close();
	}

	void clear_journal()
	{
		char name[USTORE_MAX_FILENAME_LEN]; journal_name(name);
		fs.remove(name);
	}

	void recover_if_needed()
	{
		char name[USTORE_MAX_FILENAME_LEN]; journal_name(name);
		File f = fs.open(name, File::ModeRead);
		if (!f) return;

		Journal j;
		size_t n = f.read(&j, sizeof(j));
		f.close();

		if (n != sizeof(j) || j.magic != JOURNAL_MAGIC) {
			fs.remove(name);
			return;
		}

		if (j.state == JOURNAL_COMMIT) {
			// Tmp file is complete — finish the swap.
			finalize_compaction();
		} else {
			// COMPACTING: tmp may be partial — just discard it.
			char tmp[USTORE_MAX_FILENAME_LEN]; snprintf(tmp, sizeof(tmp), "%s_compact.tmp", base_prefix);
			fs.remove(tmp);
		}

		fs.remove(name);  // clear journal
	}

	/* -------- FINALIZE COMPACTION -------- */

	void finalize_compaction()
	{
		char tmp_name[USTORE_MAX_FILENAME_LEN]; snprintf(tmp_name, sizeof(tmp_name), "%s_compact.tmp", base_prefix);
		char seg0[USTORE_MAX_FILENAME_LEN];     segment_name(0, seg0);

		// Remove all existing segments, then rename tmp → seg0.
		for (uint32_t i = 0; i < UFSKV_MAX_SEGMENTS; i++) {
			char sname[USTORE_MAX_FILENAME_LEN]; segment_name(i, sname);
			fs.remove(sname);
		}
		if (!fs.rename(tmp_name, seg0)) {
			fs.remove(tmp_name);
			return;
		}

		// Rebuild in-memory index by scanning seg0.
		index.clear();
		File f = fs.open(seg0, File::ModeRead);
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
					index_insert(key_buf, hdr.key_len, 0, scan_offset, hdr.timestamp);
				scan_offset += sizeof(hdr) + hdr.key_len + hdr.length + sizeof(c);
			}
			f.close();
		}

		// Rebuild persistent index from the now-correct in-memory index.
		// Use write_index_bulk() — single flush for all entries — not persist_index_entry()
		// which flushes after every entry and would cause N × fsync delays on flash.
		char iname[USTORE_MAX_FILENAME_LEN]; index_name(iname);
		if (index_file) index_file.close();
		fs.remove(iname);
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
		index.clear();

		for (uint32_t seg = 0; seg < UFSKV_MAX_SEGMENTS; seg++)
		{
			char sname[USTORE_MAX_FILENAME_LEN];
			segment_name(seg, sname);
			File f = fs.open(sname, File::ModeRead);
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
					index_insert(key_buf, hdr.key_len, seg, scan_offset, hdr.timestamp);
				scan_offset += sizeof(hdr) + hdr.key_len + hdr.length + sizeof(c);
			}
			f.close();
		}

		// Persist the rebuilt index so the next boot uses the fast path.
		char iname[USTORE_MAX_FILENAME_LEN]; index_name(iname);
		if (index_file) index_file.close();
		fs.remove(iname);
		write_index_bulk();
	}

	/* -------- COMPACTION -------- */

	bool compact()
	{
printf("[ustore] Compacting storage...\n");

		// --- Phase 1: write COMPACTING journal ---
		write_journal(JOURNAL_COMPACTING);

		char tmp_name[USTORE_MAX_FILENAME_LEN]; snprintf(tmp_name, sizeof(tmp_name), "%s_compact.tmp", base_prefix);
printf("[ustore] Opening tmp file: %s\n", tmp_name);
		File outf = fs.open(tmp_name, File::ModeWrite);
		if (!outf) { clear_journal(); return false; }

		// --- Phase 2: build per-segment sorted lists of live offsets ---
		struct LiveRec { uint32_t offset; KeyType key; };
		using LiveRecVec = std::vector<LiveRec, rebind_alloc<LiveRec>>;
		rebind_alloc<LiveRec> lr_alloc(alloc_);
		LiveRecVec per_seg[UFSKV_MAX_SEGMENTS];
		for (auto& v : per_seg) { v = LiveRecVec(lr_alloc); }

		for (auto& kv : index) {
			uint32_t seg = kv.second.segment;
			if (seg < UFSKV_MAX_SEGMENTS) {
				LiveRec lr; lr.offset = kv.second.offset; lr.key = kv.first;
				per_seg[seg].push_back(lr);
			}
		}
		for (uint32_t s = 0; s < UFSKV_MAX_SEGMENTS; s++) {
			std::sort(per_seg[s].begin(), per_seg[s].end(),
					[](const LiveRec& a, const LiveRec& b){ return a.offset < b.offset; });
		}

		// --- Phase 3: one open/close per segment, read live records in offset order ---
		// Static to avoid placing 1 KB on the stack — compact() is not re-entrant.
		uint8_t key_buf[USTORE_MAX_KEY_LEN];
		static uint8_t val_buf[USTORE_MAX_VALUE_LEN];
		bool write_ok = true;

		for (uint32_t s = 0; s < UFSKV_MAX_SEGMENTS && write_ok; s++) {
			if (per_seg[s].empty()) continue;

			char src_name[USTORE_MAX_FILENAME_LEN]; segment_name(s, src_name);
printf("[ustore] Opening src file: %s\n", src_name);
			File src = fs.open(src_name, File::ModeRead);
			if (!src) continue;

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

		outf.flush();
printf("[ustore] Closing tmp file: %s\n", tmp_name);
		outf.close();

		if (!write_ok) {
			printf("[ustore] Compact aborted: storage full, segments preserved\n");
			fs.remove(tmp_name);
			clear_journal();   // journal is still COMPACTING — safe to discard
			return false;
		}

		// --- Phase 4: commit journal → safe to finalize ---
		write_journal(JOURNAL_COMMIT);

		finalize_compaction();   // rename + index rebuild

		clear_journal();

		return true;
	}

private:

	FileSystem fs;

	char base_prefix[32];

	File active_file;
	File index_file;

	uint32_t current_segment = 0;
	uint32_t current_offset = 0;

	bool compact_in_cooldown = false;
	uint32_t compact_cooldown_start_ms = 0;

	uint32_t policy_ttl_s_  = 0; // 0 = TTL disabled (seconds)
	uint32_t policy_max_recs_ = 0; // 0 = max-records disabled

	uint8_t write_buf[UFSKV_WRITE_BUFFER];
	size_t write_buf_pos;

	Allocator alloc_;
	IndexMap index;
};


using FileStore = BasicFileStore<>;

}
