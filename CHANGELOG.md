# Changelog

## [Unreleased]

### Added

- Added `AnyStore`, a type-erased wrapper that selects any store backend (`FileStore`, `HeapStore`, or custom) at runtime and works with `TypedStore`.
- Added one path model shared by every filesystem adapter: a path is always relative to the adapter's root (`"/a"`, `"./a"` and `"a"` are the same file), and the base path says where that root is. See "Paths and base paths" in the user guide. Path resolution never allocates; its buffers are sized by the new `USTORE_MAX_PATH_LEN` (default 128) and `USTORE_MAX_BASEPATH_LEN` (default 32) macros.

### Changed

- **Behavior change (native `PosixFileSystem` / `StdioFileSystem` / `UniversalFileSystem`):** with the default (empty) base path, absolute paths now resolve under the current directory instead of the host's root, the same way relative paths already did. For example, `"/tmp/x"` now means `./tmp/x`. To use host-absolute paths, construct the adapter with `"/"` as the base path, e.g. `PosixFileSystem("/")`. Relative paths such as `"./kv/"` resolve exactly as before.
- **Behavior change (native `PosixFileSystem` / `StdioFileSystem`):** a non-empty base path is now honored. It was previously ignored, so every path now resolves under it.

### Fixed

- Fixed relative paths (`"./x"`) failing on the ESP32 `LittleFSFileSystem`, `SPIFFSFileSystem` and `SDFileSystem` adapters, whose Arduino FS layer only accepts paths that start with `/`. This also stops their `init()` write check (on `"./__init_test__"`) from failing, which reformatted the filesystem on every `init()` call when `reformatOnFail` was set (the default).
- Fixed the POSIX and stdio adapters ignoring their base path, which also left relative paths unresolvable on ESP32 with a non-empty mount point (#13).
- Fixed adapters keeping the caller's base-path pointer, which dangled if the string was temporary, and fixed the LittleFS mount point dangling after the adapter was destroyed, because the Arduino FS layer keeps that pointer (#13).
- Fixed `FileStore` compaction running with the active segment still open, which LittleFS and FatFS refuse to delete, and fixed the store being left without a writable segment (or with a stray out-of-range segment file) after a failed compaction (#6, #11).
- Fixed compaction recovery paths that could lose data: a power loss after the compacted segment was moved into place but before the journal was cleared deleted all data on the next boot; a failed rename while finalizing deleted all data and still reported success; recovery from a compaction interrupted mid-way kept a stale index, and a later compaction attempt could truncate the only copy of the interrupted compaction's records. A compaction interrupted by a write failure is now recovered immediately rather than at the next boot.

## [0.1.7] - 2026-06-27

### Added

- Added support for `FileStore` directories.
- Added support for treating trailing `/` paths as directory prefixes during `FileStore::init()`.
- Added propagation of subdirectory names in `listDirectory()`.
- Added initial `microStore` user guide documentation.
- Added `USTORE_ENABLE_LOG` preprocessor option for configurable logging.
- Added optional `basepath` constructor support to `LittleFSFileSystem` and `StdioFileSystem`.
- Added `close()` methods to `FileStore` and `HeapStore`.

### Changed

- Refactored file paths to use `./` relative naming for broader filesystem compatibility.
- Made `size()` methods `const`.
- Changed `TypedStore::iterator::operator*()` to return `Entry&` rather than by value.
- Disabled `main()` unless `LIBRARY_TEST` is defined.

### Improved

- Improved logging behavior when stores are accessed before initialization.
- Improved consistency across filesystem adapters.
- Expanded and improved test coverage.

### Fixed

- Fixed POSIX directory existence checks.
- Fixed stdio directory existence checks.
- Fixed range-based iteration behavior in `TypedStore`.
- Fixed directory handling during filesystem enumeration.
- Various minor compatibility and stability fixes.
