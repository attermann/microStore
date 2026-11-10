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

/*
 * Path resolution shared by all FileSystem adapters.
 *
 * Path model (identical on every adapter and platform):
 *   - Every adapter has a root. A path is always relative to that root, so
 *     "/a/b", "./a/b" and "a/b" all name the same file: <root>/a/b.
 *   - An adapter's base path says where the root is. An empty base path
 *     selects the backend's default root (the current directory on a host
 *     OS, the mounted filesystem's root on an MCU).
 *   - ".." is passed through untouched; this is not a sandbox.
 *
 * Everything here works on caller-supplied buffers: no heap allocation.
 */

#include <stddef.h>
#include <string.h>

// Max length (including terminator) of a fully resolved path, base included.
// Each adapter call places one buffer of this size on the stack (two for rename).
#ifndef USTORE_MAX_PATH_LEN
#define USTORE_MAX_PATH_LEN 128
#endif

// Max length (including terminator) of an adapter base path / mount point.
#ifndef USTORE_MAX_BASEPATH_LEN
#define USTORE_MAX_BASEPATH_LEN 32
#endif

namespace microStore { namespace Path {

// Skip everything that only says "the root": any run of "/" and "./"
// components, and a lone trailing ".". Returns the root-relative remainder.
inline const char* skip_root(const char* p) {
	for (;;) {
		if (p[0] == '/') { ++p; continue; }
		if (p[0] == '.' && p[1] == '/') { p += 2; continue; }
		if (p[0] == '.' && p[1] == '\0') { ++p; continue; }
		return p;
	}
}

// Append "/" + rel to buf at pos, collapsing runs of "/" in rel. A trailing
// "/" in rel is kept (FileStore uses it to mark a directory prefix).
// Returns false if the result (plus terminator) does not fit in n.
inline bool append_rel(char* buf, size_t n, size_t pos, const char* rel) {
	if (pos + 1 >= n) return false;
	buf[pos++] = '/';
	for (const char* s = rel; *s; ++s) {
		if (*s == '/' && buf[pos - 1] == '/') continue;
		if (pos + 1 >= n) return false;
		buf[pos++] = *s;
	}
	buf[pos] = '\0';
	return true;
}

// True if p is already "/" followed by a remainder with no root-only prefix
// and no repeated "/", so it can be used without copying.
inline bool is_canonical(const char* p) {
	if (p[0] != '/' || skip_root(p) != p + 1) return false;
	return strstr(p, "//") == nullptr;
}

// Root-relative path as "/a/b". Returns p itself when already canonical,
// otherwise writes into buf. Null or empty means the root ("/").
// Returns nullptr if buf is too small.
inline const char* normalize(const char* p, char* buf, size_t n) {
	if (p == nullptr || p[0] == '\0') return "/";
	if (is_canonical(p)) return p;
	if (!append_rel(buf, n, 0, skip_root(p))) return nullptr;
	return buf;
}

// base + normalized p, written into buf. A trailing "/" on base is ignored,
// so a base of "/" yields "/a/b". Returns nullptr if buf is too small.
inline const char* join(const char* base, const char* p, char* buf, size_t n) {
	size_t bl = base ? strlen(base) : 0;
	while (bl > 0 && base[bl - 1] == '/') --bl;
	if (bl >= n) return nullptr;
	if (bl > 0) memcpy(buf, base, bl);
	if (!append_rel(buf, n, bl, (p == nullptr) ? "" : skip_root(p))) return nullptr;
	return buf;
}

// Copy a caller's base path into an adapter-owned buffer, so the adapter never
// keeps the caller's pointer. Trailing "/" are dropped, except that "/" itself
// is kept (it means the host root, which differs from an empty base).
// Null copies as empty. Returns false (leaving dst empty) if src does not fit.
inline bool copy_base(char* dst, size_t n, const char* src) {
	dst[0] = '\0';
	if (src == nullptr) return true;
	size_t len = strlen(src);
	while (len > 1 && src[len - 1] == '/') --len;
	if (len >= n) return false;
	memcpy(dst, src, len);
	dst[len] = '\0';
	return true;
}

// Mount point for a base path, held in storage that lives for the whole
// program. Arduino's FS layer keeps the raw pointer passed to begin(), so the
// string must outlive any adapter. One buffer is shared by every adapter that
// mounts the (singleton) LittleFS. The first call fixes the mount point; a later
// call with a different base keeps the original, since the filesystem is
// already mounted there, and reports the conflict through *conflict.
// "/" maps to "" (ESP-IDF's fallback mount, i.e. the whole filesystem).
inline const char* persistent_mount(const char* base, bool* conflict = nullptr) {
	static char mount[USTORE_MAX_BASEPATH_LEN];
	static bool set = false;
	const char* want = (base == nullptr || strcmp(base, "/") == 0) ? "" : base;
	if (!set) {
		if (!copy_base(mount, sizeof(mount), want)) return nullptr;
		set = true;
	}
	if (conflict) *conflict = (strcmp(mount, want) != 0);
	return mount;
}

} }

// Adapter helper: resolve `in` through the adapter's resolve() into a stack
// buffer named after `var`, and return `fail` from the enclosing function if it
// does not fit. A too-long path fails the operation rather than being
// truncated, so it can never touch a different file.
#define USTORE_RESOLVE_PATH(var, in, fail) \
	char var##_buf[USTORE_MAX_PATH_LEN]; \
	const char* var = resolve((in), var##_buf, sizeof(var##_buf)); \
	if (var == nullptr) return fail
