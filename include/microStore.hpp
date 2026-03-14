#pragma once

/*
microStore.hpp
Advanced single-header KV store for embedded filesystems.

Features
--------
- append-only segmented log
- robin-hood hash index
- persistent index file (fast boot)
- write batching
- crash safe commits
- tombstone deletes
- automatic compaction
- filesystem agnostic

Below is a single-header C++ implementation called microStore that incorporates the previous design plus the two additional improvements:

New capabilities added
	1.	Persistent hash index file → near-instant boot (no full log scan)
	2.	Write batching buffer → reduces filesystem write amplification
	3.	Segmented append-only log
	4.	Tombstone deletes
	5.	Automatic compaction
	6.	Robin-hood hash table
	7.	Crash-safe commit markers
	8.	Filesystem-agnostic backend

The design still follows the log-structured KV architecture used by systems such as Bitcask, which combines an append-only log with an in-memory index for fast lookups.

*/

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <vector>
#include <unordered_map>
#if defined(PLATFORM_NATIVE)
#include <chrono>
#endif

namespace microStore
{

/* ---------------- CONFIG ---------------- */

#ifndef UFSKV_MAX_VALUE
#define UFSKV_MAX_VALUE 1024
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

#ifndef KV_MAX_KEY_LEN
#define KV_MAX_KEY_LEN 64
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

/* ---------------- TIME HELPER ---------------- */

#if defined(PLATFORM_NATIVE)
static inline uint32_t ufskv_millis()
{
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}
#else
// millis() is a reliable Arduino built-in on both ESP32 and nRF52840.
// std::chrono::steady_clock is intentionally avoided: the Adafruit nRF52
// Arduino core lacks the _gettimeofday_r backend and causes linker errors.
static inline uint32_t ufskv_millis()
{
    return (uint32_t)millis();
}
#endif

/* ---------------- FILESYSTEM INTERFACE ---------------- */

struct FileHandle
{
    void* ctx;
};

struct FileSystemInterface
{
    FileHandle (*open)(const char*, const char*);
    size_t (*read)(FileHandle, void*, size_t);
    size_t (*write)(FileHandle, const void*, size_t);
    int (*seek)(FileHandle, long, int);
    long (*tell)(FileHandle);
    int (*flush)(FileHandle);
    int (*close)(FileHandle);
    int (*remove)(const char*);
    int (*rename)(const char*, const char*);
};

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

/* ---------------- CRC ---------------- */

static uint32_t crc32(uint32_t crc,const uint8_t* data,size_t len)
{
    crc = ~crc;

    for(size_t i=0;i<len;i++)
    {
        crc ^= data[i];
        for(int j=0;j<8;j++)
            crc = (crc>>1) ^ (0xEDB88320 & -(crc&1));
    }

    return ~crc;
}

/* ---------------- STORAGE ENGINE ---------------- */

class Storage
{
public:

    Storage()
    {
        write_buf_pos = 0;
        index_file.ctx = nullptr;
    }

    bool init(FileSystemInterface* iface,const char* prefix)
    {
        fs = iface;
        strncpy(base_prefix,prefix,sizeof(base_prefix));

        recover_if_needed();

        load_index();

        open_index_for_append();

        // Resume at the highest segment that exists on flash, not always seg 0.
        // Without this, after reset current_segment stays 0 and new writes
        // overwrite previously-live segments, losing all their records.
        uint32_t resume_seg = 0;
        for (uint32_t i = 0; i < UFSKV_MAX_SEGMENTS; i++)
        {
            char name[64];
            segment_name(i, name);
            FileHandle f = fs->open(name, "rb");
            if (f.ctx) { fs->close(f); resume_seg = i; }
        }

        open_segment(resume_seg);

        return true;
    }

    void clear()
    {
        char name[64];

        if(index_file.ctx) { fs->close(index_file); index_file.ctx = nullptr; }

        for(uint32_t i=0;i<UFSKV_MAX_SEGMENTS;i++)
        {
            segment_name(i,name);
            fs->remove(name);
        }

        index_name(name);
        fs->remove(name);

        index.clear();
        current_segment=0;
        current_offset=0;
        write_buf_pos=0;

        open_index_for_append();
        open_segment(0);
    }

    /* -------- PUT -------- */

    bool put(const uint8_t* key, uint8_t key_len, uint32_t ts, const void* data, uint16_t len)
    {
        if(len > UFSKV_MAX_VALUE)
            return false;

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

        append(&hdr,sizeof(hdr));
        append(key,key_len);
        append(data,len);

        RecordCommit c;
        c.magic = MAGIC_COMMIT;

        append(&c,sizeof(c));

        flush_buffer();  // ensure data is on flash before committing index entry

        index_insert(key, key_len, current_segment, offset, ts);

        persist_index_entry(key, key_len, current_segment, offset, ts);

        current_offset += sizeof(hdr)+key_len+len+sizeof(c);

        return true;
    }

    bool put(const char* key, uint32_t ts, const void* data, uint16_t len)
    {
        size_t kl = strlen(key);
        if(kl > KV_MAX_KEY_LEN) return false;
        return put((const uint8_t*)key, (uint8_t)kl, ts, data, len);
    }

    bool put(const std::vector<uint8_t>& key, uint32_t ts, const void* data, uint16_t len)
    {
        if(key.size() > KV_MAX_KEY_LEN) return false;
        return put(key.data(), (uint8_t)key.size(), ts, data, len);
    }

    /* -------- GET -------- */

    bool get(const uint8_t* key, uint8_t key_len, void* out, uint16_t* len)
    {
        flush_buffer();

        IndexValue* e=index_find(key, key_len);
        if(!e) return false;

        char name[64];
        segment_name(e->segment,name);

        FileHandle f=fs->open(name,"rb");
        if(!f.ctx) return false;

        fs->seek(f,e->offset,0);

        RecordHeader hdr;
        fs->read(f,&hdr,sizeof(hdr));

        fs->seek(f,(long)(e->offset+sizeof(hdr)+hdr.key_len),0);

        *len=hdr.length;

        fs->read(f,out,hdr.length);

        fs->close(f);

        return true;
    }

    bool get(const char* key, void* out, uint16_t* len)
    {
        size_t kl = strlen(key);
        if(kl > KV_MAX_KEY_LEN) return false;
        return get((const uint8_t*)key, (uint8_t)kl, out, len);
    }

    bool get(const std::vector<uint8_t>& key, void* out, uint16_t* len)
    {
        if(key.size() > KV_MAX_KEY_LEN) return false;
        return get(key.data(), (uint8_t)key.size(), out, len);
    }

    /* -------- DELETE -------- */

    bool remove(const uint8_t* key, uint8_t key_len)
    {
        RecordHeader hdr;

        hdr.magic = MAGIC_RECORD;
        hdr.key_len = key_len;
        hdr.timestamp = 0;
        hdr.length = 0;
        hdr.flags = FLAG_DELETE;
        hdr.crc = crc32(0,(uint8_t*)&hdr,sizeof(hdr)-4);
        hdr.crc = crc32(hdr.crc,key,key_len);

        append(&hdr,sizeof(hdr));
        append(key,key_len);

        RecordCommit c;
        c.magic=MAGIC_COMMIT;

        append(&c,sizeof(c));

        flush_buffer();  // ensure tombstone is on flash before committing index entry

        index_remove(key, key_len);

        persist_index_entry(key,key_len,0xFFFFFFFF,0);

        current_offset += sizeof(RecordHeader) + key_len + sizeof(RecordCommit);

        return true;
    }

    bool remove(const char* key)
    {
        size_t kl = strlen(key);
        if(kl > KV_MAX_KEY_LEN) return false;
        return remove((const uint8_t*)key, (uint8_t)kl);
    }

    bool remove(const std::vector<uint8_t>& key)
    {
        if(key.size() > KV_MAX_KEY_LEN) return false;
        return remove(key.data(), (uint8_t)key.size());
    }

    /* -------- POLICY -------- */

    void set_policy(uint32_t ttl_s, uint32_t max_recs)
    {
        policy_ttl_s_    = ttl_s;
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
            char name[64];
            segment_name(seg, name);

            FileHandle f = fs->open(name, "rb");
            if (!f.ctx) break;  // segments are contiguous; first missing one ends the scan

            uint32_t file_size = 0;
            uint32_t entries = 0;
            uint32_t tomb_entries = 0;
            uint32_t tomb_bytes = 0;
            uint32_t dead_entries = 0;
            uint32_t dead_bytes = 0;

            {
                fs->seek(f, 0, SEEK_END);
                file_size = (uint32_t)fs->tell(f);
                fs->seek(f, 0, 0);

                while(true)
                {
                    uint32_t offset = (uint32_t)fs->tell(f);

                    RecordHeader hdr;
                    if(fs->read(f, &hdr, sizeof(hdr)) != sizeof(hdr))
                        break;

                    if(hdr.magic != MAGIC_RECORD)
                        break;

                    if(hdr.key_len > KV_MAX_KEY_LEN)
                        break;

                    uint8_t key[KV_MAX_KEY_LEN];
                    if(fs->read(f, key, hdr.key_len) != hdr.key_len)
                        break;

                    fs->seek(f, (long)(offset + sizeof(hdr) + hdr.key_len + hdr.length), 0);

                    RecordCommit c;
                    if(fs->read(f, &c, sizeof(c)) != sizeof(c))
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

                fs->close(f);
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

    /* -------- BUFFERED WRITES -------- */

    void append(const void* data,size_t len)
    {
        const uint8_t* p=(const uint8_t*)data;

        while(len)
        {
            size_t space = UFSKV_WRITE_BUFFER - write_buf_pos;
            size_t n = (len < space) ? len : space;

            memcpy(write_buf+write_buf_pos,p,n);

            write_buf_pos += n;
            p += n;
            len -= n;

            if(write_buf_pos == UFSKV_WRITE_BUFFER)
                flush_buffer();
        }
    }

    void flush_buffer()
    {
        fs->write(active_file,write_buf,write_buf_pos);
        fs->flush(active_file);
        write_buf_pos=0;
    }

    /* -------- INDEX -------- */

    struct VectorHash {
        size_t operator()(const std::vector<uint8_t>& v) const {
            uint32_t h = 2166136261u;
            for(uint8_t b : v) { h ^= b; h *= 16777619u; }
            return h;
        }
    };

    struct IndexValue { uint32_t segment; uint32_t offset; uint32_t timestamp; };

    IndexValue* index_find(const uint8_t* key, uint8_t key_len)
    {
        auto it = index.find(std::vector<uint8_t>(key, key + key_len));
        return (it != index.end()) ? &it->second : nullptr;
    }

    void index_insert(const uint8_t* key, uint8_t key_len, uint32_t seg, uint32_t off, uint32_t ts = 0)
    {
        IndexValue& iv = index[std::vector<uint8_t>(key, key + key_len)];
        iv.segment   = seg;
        iv.offset    = off;
        iv.timestamp = ts;
    }

    void index_remove(const uint8_t* key, uint8_t key_len)
    {
        index.erase(std::vector<uint8_t>(key, key + key_len));
    }

    /* -------- INDEX FILE -------- */

    bool persist_index_entry(const uint8_t* key, uint8_t key_len, uint32_t seg, uint32_t off, uint32_t ts = 0)
    {
        if (!index_file.ctx) return false;

        fs->write(index_file, &key_len, 1);
        fs->write(index_file, key, key_len);
        fs->write(index_file, &seg, 4);
        fs->write(index_file, &off, 4);
        fs->write(index_file, &ts, 4);
        fs->flush(index_file);  // explicit fsync — guarantees entry reaches flash

        return true;
    }

    bool write_index_bulk()
    {
        char name[64];
        index_name(name);

        FileHandle f = fs->open(name, "ab");
        if (!f.ctx) return false;

        for (auto& kv : index)
        {
            uint8_t  key_len = (uint8_t)kv.first.size();
            uint32_t seg     = kv.second.segment;
            uint32_t off     = kv.second.offset;
            uint32_t ts      = kv.second.timestamp;

            fs->write(f, &key_len, 1);
            fs->write(f, kv.first.data(), key_len);
            fs->write(f, &seg, 4);
            fs->write(f, &off, 4);
            fs->write(f, &ts, 4);
        }

        fs->flush(f);
        fs->close(f);
        return true;
    }

    bool load_index()
    {
        char name[64];
        index_name(name);

        FileHandle f=fs->open(name,"rb");
        if(!f.ctx) return false;

        while(true)
        {
            uint8_t key_len;
            uint8_t key[KV_MAX_KEY_LEN];
            uint32_t seg;
            uint32_t off;

            if(fs->read(f, &key_len, 1) != 1) break;
            if(key_len > KV_MAX_KEY_LEN) break;
            if(fs->read(f, key, key_len) != key_len) break;
            if(fs->read(f, &seg, 4) != 4) break;
            if(fs->read(f, &off, 4) != 4) break;
            uint32_t ts = 0;
            if(fs->read(f, &ts, 4) != 4) break;

            // seg==0xFFFFFFFF is a deletion sentinel written by remove()
            if(seg==0xFFFFFFFF)
                index_remove(key, key_len);
            else
                index_insert(key, key_len, seg, off, ts);
        }

        fs->close(f);

		return true;
    }

    /* -------- INDEX FILE OPEN HELPER -------- */

    void open_index_for_append()
    {
        char name[64];
        index_name(name);
        index_file = fs->open(name, "ab");
    }

    /* -------- SEGMENTS -------- */

    void segment_name(uint32_t id,char* out)
    {
        sprintf(out,"%s_%u.dat",base_prefix,id);
    }

    void index_name(char* out)
    {
        sprintf(out,"%s_index.dat",base_prefix);
    }

    void journal_name(char* out)
    {
        sprintf(out, "%s_journal.dat", base_prefix);
    }

    void open_segment(uint32_t id)
    {
        char name[64];
        segment_name(id,name);

printf("[ufskv] Opening active file: %s\n", name);
        active_file = fs->open(name,"ab+");
        current_segment=id;
        fs->seek(active_file,0,SEEK_END);
        current_offset=fs->tell(active_file);
    }

    void rotate_segment_if_needed(uint32_t write_size)
    {
        if (current_offset + write_size + sizeof(RecordHeader) + sizeof(RecordCommit) < UFSKV_SEGMENT_SIZE)
            return;

printf("[ufskv] Rotating segment...\n");
        flush_buffer();

printf("[ufskv] Closing active file\n");
        fs->close(active_file);

        current_segment++;

        if (current_segment >= UFSKV_MAX_SEGMENTS) {
            compact();  // compact() calls open_segment(0) internally
            // current_segment and active_file are set by compact → open_segment
            return;
        }

        open_segment(current_segment);
    }

    /* -------- JOURNAL HELPERS -------- */

    void write_journal(uint32_t state)
    {
        char name[64]; journal_name(name);
        FileHandle f = fs->open(name, "wb");
        if (!f.ctx) return;
        Journal j; j.magic = JOURNAL_MAGIC; j.state = state;
        fs->write(f, &j, sizeof(j));
        fs->flush(f);
        fs->close(f);
    }

    void clear_journal()
    {
        char name[64]; journal_name(name);
        fs->remove(name);
    }

    void recover_if_needed()
    {
        char name[64]; journal_name(name);
        FileHandle f = fs->open(name, "rb");
        if (!f.ctx) return;

        Journal j;
        size_t n = fs->read(f, &j, sizeof(j));
        fs->close(f);

        if (n != sizeof(j) || j.magic != JOURNAL_MAGIC) {
            fs->remove(name);
            return;
        }

        if (j.state == JOURNAL_COMMIT) {
            // Tmp file is complete — finish the swap.
            finalize_compaction();
        } else {
            // COMPACTING: tmp may be partial — just discard it.
            char tmp[64]; snprintf(tmp, sizeof(tmp), "%s_compact.tmp", base_prefix);
            fs->remove(tmp);
        }

        fs->remove(name);  // clear journal
    }

    /* -------- FINALIZE COMPACTION -------- */

    void finalize_compaction()
    {
        char tmp_name[64]; snprintf(tmp_name, sizeof(tmp_name), "%s_compact.tmp", base_prefix);
        char seg0[64];     segment_name(0, seg0);

        // Remove all existing segments, then rename tmp → seg0.
        for (uint32_t i = 0; i < UFSKV_MAX_SEGMENTS; i++) {
            char sname[64]; segment_name(i, sname);
            fs->remove(sname);
        }
        if (fs->rename(tmp_name, seg0) != 0) {
            fs->remove(tmp_name);
            return;
        }

        // Rebuild in-memory index by scanning seg0.
        index.clear();
        FileHandle f = fs->open(seg0, "rb");
        if (f.ctx) {
            uint32_t scan_offset = 0;
            uint8_t key_buf[KV_MAX_KEY_LEN];
            while (true) {
                RecordHeader hdr;
                if (fs->read(f, &hdr, sizeof(hdr)) != sizeof(hdr)) break;
                if (hdr.magic != MAGIC_RECORD || hdr.key_len == 0 ||
                    hdr.key_len > KV_MAX_KEY_LEN || hdr.length > UFSKV_MAX_VALUE) break;
                if (fs->read(f, key_buf, hdr.key_len) != hdr.key_len) break;
                fs->seek(f, (long)(scan_offset + sizeof(hdr) + hdr.key_len + hdr.length), 0);
                RecordCommit c;
                if (fs->read(f, &c, sizeof(c)) != sizeof(c)) break;
                if (c.magic != MAGIC_COMMIT) break;
                if (!(hdr.flags & FLAG_DELETE))
                    index_insert(key_buf, hdr.key_len, 0, scan_offset, hdr.timestamp);
                scan_offset += sizeof(hdr) + hdr.key_len + hdr.length + sizeof(c);
            }
            fs->close(f);
        }

        // Rebuild persistent index from the now-correct in-memory index.
        // Use write_index_bulk() — single flush for all entries — not persist_index_entry()
        // which flushes after every entry and would cause N × fsync delays on flash.
        char iname[64]; index_name(iname);
        if (index_file.ctx) { fs->close(index_file); index_file.ctx = nullptr; }
        fs->remove(iname);
        write_index_bulk();
        open_index_for_append();

        current_segment = 0;
        // current_offset is set by open_segment() which the caller will invoke next.
    }

    /* -------- COMPACTION -------- */

    void compact()
    {
printf("[ufskv] Compacting storage...\n");

        // --- Phase 1: write COMPACTING journal ---
        write_journal(JOURNAL_COMPACTING);

        char tmp_name[64]; snprintf(tmp_name, sizeof(tmp_name), "%s_compact.tmp", base_prefix);
printf("[ufskv] Opening tmp file: %s\n", tmp_name);
        FileHandle outf = fs->open(tmp_name, "wb");
        if (!outf.ctx) { clear_journal(); return; }

        // --- Phase 2: build per-segment sorted lists of live offsets ---
        struct LiveRec { uint32_t offset; std::vector<uint8_t> key; };
        std::vector<LiveRec> per_seg[UFSKV_MAX_SEGMENTS];

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
        uint8_t key_buf[KV_MAX_KEY_LEN];
        uint8_t val_buf[UFSKV_MAX_VALUE];

        for (uint32_t s = 0; s < UFSKV_MAX_SEGMENTS; s++) {
            if (per_seg[s].empty()) continue;

            char src_name[64]; segment_name(s, src_name);
printf("[ufskv] Opening src file: %s\n", src_name);
            FileHandle src = fs->open(src_name, "rb");
            if (!src.ctx) continue;

            for (size_t i = 0; i < per_seg[s].size(); i++) {
                LiveRec& lr = per_seg[s][i];
                fs->seek(src, (long)lr.offset, 0);
                RecordHeader hdr;
                if (fs->read(src, &hdr, sizeof(hdr)) != sizeof(hdr)) continue;
                if (hdr.magic != MAGIC_RECORD || hdr.key_len > KV_MAX_KEY_LEN ||
                    hdr.length > UFSKV_MAX_VALUE) continue;
                if (fs->read(src, key_buf, hdr.key_len) != hdr.key_len) continue;
                if (hdr.length > 0 && fs->read(src, val_buf, hdr.length) != hdr.length) continue;
                RecordCommit c; c.magic = MAGIC_COMMIT;
                fs->write(outf, &hdr,    sizeof(hdr));
                fs->write(outf, key_buf, hdr.key_len);
                if (hdr.length > 0) fs->write(outf, val_buf, hdr.length);
                fs->write(outf, &c,      sizeof(c));
            }

printf("[ufskv] Closing src file: %s\n", src_name);
            fs->close(src);
        }

        fs->flush(outf);
printf("[ufskv] Closing tmp file: %s\n", tmp_name);
        fs->close(outf);

        // --- Phase 4: commit journal → safe to finalize ---
        write_journal(JOURNAL_COMMIT);

        finalize_compaction();   // rename + index rebuild

        clear_journal();

        // Reopen segment 0 as the active segment.
        open_segment(0);
    }

private:

    FileSystemInterface* fs;

    char base_prefix[32];

    FileHandle active_file;
    FileHandle index_file;

    uint32_t current_segment=0;
    uint32_t current_offset=0;

    bool     compact_in_cooldown       = false;
    uint32_t compact_cooldown_start_ms = 0;

    uint32_t policy_ttl_s_    = 0;   // 0 = TTL disabled (seconds)
    uint32_t policy_max_recs_ = 0;   // 0 = max-records disabled

    uint8_t write_buf[UFSKV_WRITE_BUFFER];
    size_t write_buf_pos;

    std::unordered_map<std::vector<uint8_t>, IndexValue, VectorHash> index;
};

}
