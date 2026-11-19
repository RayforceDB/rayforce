/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_FILEIO_H
#define RAY_FILEIO_H

#include "core/platform.h"

/* Cross-platform file I/O (locking, sync, atomic rename) */
#ifdef RAY_OS_WINDOWS
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN   /* keep <dlgs.h>/<winsock.h> macros out */
  #endif
  #include <windows.h>
  typedef HANDLE ray_fd_t;
  #define RAY_FD_INVALID INVALID_HANDLE_VALUE
#else
  typedef int ray_fd_t;
  #define RAY_FD_INVALID (-1)
#endif

#define RAY_OPEN_READ   0x01
#define RAY_OPEN_WRITE  0x02
#define RAY_OPEN_CREATE 0x04

ray_fd_t  ray_file_open(const char* path, int flags);
void     ray_file_close(ray_fd_t fd);
ray_err_t ray_file_lock_ex(ray_fd_t fd);
ray_err_t ray_file_lock_sh(ray_fd_t fd);
/* Exclusive lock without waiting: RAY_ERR_IO when another holder has it. */
ray_err_t ray_file_trylock_ex(ray_fd_t fd);
ray_err_t ray_file_unlock(ray_fd_t fd);
ray_err_t ray_file_sync(ray_fd_t fd);
/* True when RAYFORCE_NO_FSYNC is set to a non-empty value other than "0":
 * every durability flush (fsync, directory fsync, msync(MS_SYNC),
 * FlushFileBuffers) becomes a no-op.  Test-suite knob only -- writes still
 * happen and are visible to readers, they just are not forced to media.
 * Read once per process. */
bool ray_fsync_disabled(void);
#ifndef RAY_OS_WINDOWS
/* fsync(fd) unless ray_fsync_disabled(); returns fsync's result (0 = ok). */
int  ray_fsync_fd(int fd);
#endif
ray_err_t ray_file_sync_dir(const char* path);
/* Flush everything written under a tree with one call, where the host can: the
 * whole filesystem holding it (Linux syncfs), on a local filesystem whose syncfs
 * reports a failed write (Linux 5.8 and later; not FUSE, whose syncfs asks the
 * server for nothing). A table of many small files otherwise spends its time in
 * thousands of fsyncs.
 *
 * Open the tree BEFORE the first write under it: syncfs reports a writeback error
 * the descriptor has not seen, and a descriptor opened after the writes samples
 * the filesystem's error state late, so an error that another syncfs (another
 * process, `sync -f`, a second import) saw first would be lost. A per-file fsync
 * has no such hole.
 *
 *   int h = ray_file_sync_tree_begin(root);      // before the writes
 *   ... write ...
 *   if (!ray_file_sync_tree_end(h)) { sync each file }   // or _abort(h) on failure
 *
 * begin returns a descriptor (>= 0), RAY_SYNC_TREE_NONE (-1) where the caller must
 * sync each file (no such call, an old kernel, a filesystem not known to report
 * errors, a missing tree, ray_file_sync_tree_force_fallback), or RAY_SYNC_TREE_SKIP
 * (-2) under ray_fsync_disabled(), where there is nothing to flush. end flushes
 * and closes: true when everything is flushed (always for SKIP, never for NONE),
 * false when the flush failed, and the caller then syncs each file, which finds
 * the error itself. abort closes without flushing.
 *
 * It waits for ALL the dirty data of the filesystem, other processes' included:
 * on a busy shared volume the last step of an import can take longer than its own
 * data needs. It is not a completeness check (a missing file is not noticed, as
 * the per-file loop noticed it) and cannot be interrupted: the writers report
 * their own errors and cancellation. */
#define RAY_SYNC_TREE_NONE (-1)
#define RAY_SYNC_TREE_SKIP (-2)
int  ray_file_sync_tree_begin(const char* root);
bool ray_file_sync_tree_end(int handle);
void ray_file_sync_tree_abort(int handle);
/* Test-suite knob: while set, ray_file_sync_tree_begin answers NONE, so the
 * per-file fallback runs where syncfs would be used (it is the only path on other
 * hosts and old kernels, and the Linux suite would not exercise it otherwise). */
void ray_file_sync_tree_force_fallback(bool on);
ray_err_t ray_file_rename(const char* old_path, const char* new_path);
/* Publish a new path without replacing any existing destination, including
 * an empty directory.  Uses the host's no-replace rename; where the host or
 * filesystem lacks one, ray_file_rename_new_emulated (never a racy
 * check-then-rename). */
ray_err_t ray_file_rename_new(const char* old_path, const char* new_path);
/* No-replace rename from portable primitives.  A directory: mkdir claims
 * the name atomically, then rename replaces only that empty placeholder
 * (an empty directory is briefly visible at new_path).  A file: link, then
 * unlink.  POSIX only; exposed for tests. */
ray_err_t ray_file_rename_new_emulated(const char* old_path, const char* new_path);
ray_err_t ray_mkdir(const char* path);
ray_err_t ray_mkdir_p(const char* path);  /* like `mkdir -p` */
/* A file in memory (Linux memfd), which no filesystem writes back; like
 * anonymous memory, its pages can still be swapped out where there is swap.
 * Its descriptor, and in path[cap] the path that opens it again, each open
 * with an offset of its own, while the descriptor stays open.
 * RAY_FD_INVALID where the host has none, or that path does not open it. */
ray_fd_t ray_file_open_mem(char* path, size_t cap);

#endif /* RAY_FILEIO_H */
