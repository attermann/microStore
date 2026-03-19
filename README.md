# microStore

Advanced header-only KV store for embedded filesystems that follows the log-structured KV architecture used by systems such as Bitcask, which combines an append-only log with an in-memory index for fast lookups.

## Features
- append-only segmented log
- robin-hood hash index
- persistent index file (fast boot)
- write batching
- crash safe commits
- tombstone deletes
- automatic compaction
- filesystem agnostic
- filesystem-agnostic backend
- journaled crash-safe compaction
- index-based compaction walk
- streaming iterator
- automatic age-based eviction (TTL)
- automatic size-based eviction (keep N most recent records)
