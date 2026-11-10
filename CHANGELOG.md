# Changelog

## [Unreleased]

### Added

- Added `AnyStore`, a type-erased wrapper that selects any store backend (`FileStore`, `HeapStore`, or custom) at runtime and works with `TypedStore`.

### Fixed

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
