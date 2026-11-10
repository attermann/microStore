# microStore Pull Request Policy

This document explains what microStore will and won't accept, so you can
tell before you start whether a change is likely to be merged. It
complements [CONTRIBUTING.md](../CONTRIBUTING.md), which covers *how* to
contribute.

Every pull request is reviewed against these rules. The maintainer uses
automated tooling to help prepare reviews, but every merge or close decision
is made by the maintainer personally.

---

## 1. What microStore is

A **header-only C++ key-value store library** for embedded filesystems,
built on the log-structured architecture used by Bitcask: an append-only,
segmented log plus an in-memory hash index. It runs on microcontrollers
(the ESP32 and nRF52 families) and on native platforms (Linux, macOS), and
it's used as the storage layer by other projects, such as
[microReticulum](https://github.com/attermann/microReticulum).

The library targets **platforms and filesystems, not boards**. It is a
library, not an application: nothing in it should be specific to a
particular board, module or product. Board definitions and environments in
this repo exist only to build and test the library.

## 2. Project goals (what a PR should move toward)

1. **Data safety.** Commits are crash-safe, compaction is journaled and
   recoverable, and corruption is detected (CRC-32). A power loss at any
   point must never leave a store that can't be opened.
2. **Stable on-disk format.** Stores written by one release open with the
   next. Record, commit, index and journal layouts and their magic numbers
   don't change without a migration or compatibility path.
3. **Fitting constrained MCUs.** RAM, flash and filesystem space are
   scarce. Code size, heap use, fragmentation, stack depth and **flash
   wear** all matter.
4. **Filesystem-agnostic.** The store engine talks only to the
   `FileSystem`/`File` abstraction. Filesystem- and platform-specific code
   lives in `include/microStore/Adapters/`, selected by `USTORE_USE_*`
   build flags.
5. **Opt-in features.** Optional behavior (logging, adapters, extra
   features) is gated behind `USTORE_*` build flags or runtime settings so
   it costs nothing when unused.
6. **Header-only, no dependencies.** The library stays header-only and
   depends only on the C++ standard library and the platform framework.
   It builds as C++14 (`gnu++14`) on every supported target.
7. **One consistent API across stores.** `FileStore`, `HeapStore`,
   `AnyStore` and `TypedStore` keep a common API shape and stay
   interchangeable.

## 3. In scope

- Bug fixes in the store engine, index, compaction, recovery, iterators,
  eviction (TTL and size), codecs or filesystem adapters.
- Robustness fixes for power loss, full filesystems, corruption and
  recovery.
- Memory, flash, flash-wear and performance improvements that don't hurt
  readability or compatibility.
- New filesystem adapters that implement `FileSystemImpl`/`FileImpl` and
  aren't tied to a particular board.
- New `Codec<T>` specializations for common types.
- Tests (unit tests in `test/`).
- Documentation fixes and improvements (README, `docs/`).
- PlatformIO build and CI fixes.

## 4. Out of scope (likely to be closed or redirected)

- **Application features** (a database server, a query language, a
  networking or sync layer, a UI) → a separate project built on microStore.
- **Board- or variant-specific code**: pin maps, variant files, conditional
  code for a particular board or module. Code like this belongs in the
  application that uses the library.
- **Platform- or filesystem-specific code in the store engine**, where it
  belongs in an adapter or where a portable approach exists, and
  **changes that work on only one platform** in shared code.
- **On-disk format changes without a compatibility path** (see 5.4).
- **New third-party dependencies**, or anything that would make the
  library no longer header-only, unless discussed in an issue first.
- **Large refactors, renames or reformatting** not tied to a stated
  problem, or not discussed first.
- **Weakening crash safety or integrity checks** for speed or size, unless
  it's opt-in and off by default.

## 5. Requirements for merging

A PR that's missing any of these will be sent back for changes before it
can be merged.

1. **CI passes:** the native build and unit tests (`native`) and the
   embedded builds for ESP32 and nRF52 (`ttgo-lora32-v21`,
   `heltec_wifi_lora_32_V3`, `wiscore_rak4631`). PRs from first-time
   contributors may need the maintainer to approve CI before it runs; "not
   yet run" isn't a failure, but it isn't a pass either.
2. **One logical concern per PR.** Unrelated fixes, formatting and
   refactors go in separate PRs.
3. **Problem stated first.** The description says what's broken or
   missing, who is affected and how to reproduce, before the solution.
4. **On-disk compatibility preserved.** Changes to record, commit, index or
   journal layouts, magic numbers or file naming either stay readable by
   the previous release or include a migration path, are discussed in an
   issue first, and have a CHANGELOG entry.
5. **Crash safety preserved.** Changes to write, commit, compaction,
   recovery or index persistence explain what happens on a power loss at
   each step, and include or update tests for it where practical.
6. **Platforms tested are listed.** Changes to shared code (the store
   engine, `File`/`FileSystem`, codecs, iterators, timestamps, eviction)
   list which platforms were tested, how, and which weren't. Adapter
   changes are tested on that adapter's platform.
7. **No breaking API changes** without prior discussion and a CHANGELOG
   entry. Changes to one store's API shape are reflected in the others
   (2.7).
8. **Docs updated** for user-visible behavior, `USTORE_*` build flags and
   configuration macros, APIs or workflows (README, `docs/`, CHANGELOG as
   appropriate).
9. **Still header-only, C++14, no new dependencies.** No `.cpp` files in
   the library, nothing beyond C++14, and no new third-party libraries
   unless agreed in an issue first.
10. **Platform-specific code is unavoidable and isolated.** It goes in an
    adapter or behind a `PLATFORM_*`/`USTORE_*` guard, is confined to as
    small an area as possible, and the PR explains why it's needed.
    Board- or variant-specific code isn't accepted (section 4).
11. **License:** contributions are accepted under the project's license
    (see [LICENSE](../LICENSE)). Code copied from elsewhere must be
    license-compatible and credited.

## 6. Preferences

These won't block a PR on their own, but they make it faster to review and
more likely to be merged.

- Follows the existing coding style and naming.
- Tests added or updated for behavior changes.
- Heap allocation in hot paths avoided; long-lived containers respect the
  custom allocator support.
- Writes and erases kept to a minimum (flash wear).
- Logging goes through the existing `USTORE_ENABLE_LOG` logging and isn't
  noisy by default.
- Small enough to review in one sitting (roughly < 500 changed lines
  excluding tests and docs; bigger needs a good reason).
- Configuration macro defaults left unchanged unless that's the point of
  the PR.

## 7. What happens after you submit

Each pull request ends up in one of four places:

| Outcome | What it means for you |
|---|---|
| **Merged** | It's in scope and meets the requirements in section 5. |
| **Changes requested** | The idea is welcome, but specific things need fixing first (failing CI, missing tests or docs, an untested platform, a format change without a compatibility path, too many concerns in one PR). You'll get a list. |
| **Discussion needed** | It raises a design, scope, format or compatibility question that has to be settled before code review. Expect the conversation to move to an issue. |
| **Closed** | It's out of scope (section 4), duplicates existing work, or conflicts with the project's goals. Where possible you'll be pointed to a better home for it. |

The maintainer has limited time, so reviews may take a while. A PR that
follows this document and the template is the fastest to get through.
If you're unsure whether something fits, open an issue first; that's
always welcome and often saves rework.
