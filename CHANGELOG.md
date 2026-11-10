# Changelog

## [Unreleased]

### Added

- Added `AnyStore`, a type-erased wrapper that selects any store backend (`FileStore`, `HeapStore`, or custom) at runtime and works with `TypedStore`.

### Fixed

- Fixed `FileStore::init()` deleting records whose timestamp lies ahead of the current clock. On devices without a real-time clock the clock restarts from the last persisted offset at boot, so records written after that point were lost on every restart; their timestamps are now clamped to the current time instead.
- Fixed `FileStore` compaction running with the active segment still open, which LittleFS and FatFS refuse to delete, and fixed the store being left without a writable segment (or with a stray out-of-range segment file) after a failed compaction (#6, #11).

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
