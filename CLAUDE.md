# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
pio run -e native                    # Build native (desktop) target
pio run -e heltec_wifi_lora_32_V3   # Build for ESP32 LoRa
pio run -e wiscore_rak4631          # Build for nRF52 LoRa
pio test -e native                  # Run tests on native target
pio test -e native -f test_iterator # Run a specific test suite
```

The native environment produces `.pio/build/native/program`. All environments use `-std=gnu++14`.

## Architecture

microStore is a persistent key-value store for embedded systems, inspired by Bitcask. All logic lives in the `include/microStore/` directory — there are no `.cpp` files.

### Core Design

- **Append-only segmented log**: Records are appended to segment files (up to 64KB each, 8 segments max). Writes go through a 4KB in-memory buffer before flushing.
- **Robin-hood hash index**: In-memory index (`std::unordered_map`) mapping keys → `(segment_id, offset)`. Rebuilt from a persistent index file at boot — no full log scan needed.
- **Tombstone deletes**: Logical deletes write a delete-marker record; compaction reclaims space.
- **Crash-safe compaction**: A journal file tracks compaction state (`COMPACTING` → `COMMIT`). On boot, `recover_if_needed()` checks the journal and either completes a partial compaction or discards it.
- **Index-based compaction walk**: Compaction builds per-segment sorted lists of live offsets from the in-memory index, then seeks directly to live records instead of a full scan.

### Key Structures

| Structure | Magic | Purpose |
|-----------|-------|---------|
| `RecordHeader` | `0xC0DEC0DE` | Per-record metadata: key/value lengths, timestamp, CRC-32 |
| `RecordCommit` | `0xFACEB00C` | Commit marker appended after each successful write |
| `Journal` | `0x4B564A4E` | Crash recovery state for compaction |

### Header Layout

| File | Purpose |
|------|---------|
| `include/microStore/Store.hpp` | KV store engine — the main library logic |
| `include/microStore/FileSystem.hpp` | OOP filesystem abstraction (`FileSystem` / `FileSystemImpl`) |
| `include/microStore/File.hpp` | OOP file abstraction (`File` / `FileImpl`) with integrated CRC-32 |
| `include/microStore/Crc.hpp` | CRC-32 utility |
| `include/microStore/impl/` | Platform-specific `FileSystemImpl` backends |

### Filesystem Abstraction

`Store.hpp` uses `FileSystem` and `File` directly (the OOP layer from `FileSystem.hpp` / `File.hpp`). `FileSystem` wraps a `shared_ptr<FileSystemImpl>`; `Store::init()` takes a `FileSystem` by value. Platform backends implement `FileSystemImpl` and are selected by build flag:

| Build Flag | Backend | Platform |
|-----------|---------|---------|
| `MICROSTORE_USE_POSIXFS` | `PosixFileSystemImpl` | native (Linux/macOS) |
| `MICROSTORE_USE_LITTLEFS` | `LittleFSFileSystemImpl` | ESP32 |
| `MICROSTORE_USE_SPIFFS` | `SPIFFSFileSystemImpl` | ESP32 |
| `MICROSTORE_USE_INTERNALFS` | `InternalFSFileSystemImpl` | nRF52 |
| `MICROSTORE_USE_FLASHFS` | `FlashFSFileSystemImpl` | nRF52 + SPI flash |

`File` accumulates a running CRC-32 on every read/write transparently; call `file.crc()` to retrieve it. To add a new platform, subclass `FileSystemImpl` and `FileImpl`.

### Platform Detection

- `PLATFORM_NATIVE`: uses `std::chrono` for timestamps
- `PLATFORM_ESP32` / `PLATFORM_NRF52`: uses Arduino's `millis()`

### Configuration Macros (top of Store.hpp)

| Macro | Default | Meaning |
|-------|---------|---------|
| `UFSKV_MAX_VALUE` | 1024 | Max value size in bytes |
| `UFSKV_SEGMENT_SIZE` | 65536 | Per-segment file size limit |
| `UFSKV_MAX_SEGMENTS` | 8 | Max number of segments |
| `UFSKV_WRITE_BUFFER` | 4096 | Write buffer size |
| `KV_MAX_KEY_LEN` | 64 | Max key length in bytes |
| `UFSKV_COMPACT_RETRY_MS` | 60000 | Compaction cooldown (ms) |

### Public API (`microStore::Store`)

Include as `#include <microStore/Store.hpp>`. The library is header-only; `src/` is intentionally empty (`srcFilter: [-<*>]` in `library.json`).

- `init(FileSystem fs, const char* prefix)` — initialize with a `FileSystem` instance and file name prefix
- `put(key, ts, data, len)` — write key-value (overloads for `uint8_t*`, `char*`, `std::vector`)
- `get(key, out, len)` — read value
- `remove(key)` — logical delete
- `clear()` — wipe all data
- `dumpInfo(detailed)` — print storage statistics
- Range-based for loop supported via `begin()`/`end()` returning a forward iterator over `Store::Entry` (key, value, timestamp)
