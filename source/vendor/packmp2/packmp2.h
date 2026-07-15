/* packMP2.h — public C API for MP2 lossless compression library.
   Link with -lpackmp2 (or vendor src/lib/ *.c + vendor/zstd/ *.c).
   For slim build (zstd only, no zpaq): compile with -DPACKMP2_SLIM.

   VENDORED HEADER — pinned copy from https://github.com/YadeWira/packMP2
   (local snapshot as of commit e91bb6a). The prebuilt libpackmp2.a this
   header pairs with is a local build artifact, not committed (see .gitignore
   *.a rule) -- run `make lib` in the packMP2 repo and copy libpackmp2.a into
   this directory to build packMP3 from source. Once both repos are stable
   and pushed, this should become a git submodule instead of a pinned copy.

   Thread-safety: functions are reentrant for independent inputs but the
   underlying unpack/pack engine uses shared global buffers (UM2_ARRAY,
   SKIPPED_DATA). Concurrent calls to compress() or decompress() will
   race. For multi-threaded use, serialize access or use separate processes.
   Planned fix (v0.6): per-call heap allocation of frame/skip buffers.

   Error reporting: every function takes a char msg[256] buffer that receives
   a human-readable diagnostic on failure (empty string on success).

   Copyright (C) 2026 packMP2 contributors. GPLv3. */
#ifndef PACKMP2_H
#define PACKMP2_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PACKMP2_VERSION_MAJOR 0
#define PACKMP2_VERSION_MINOR 5
#define PACKMP2_VERSION_PATCH 0

/* ---- compression methods ---- */
#define PACKMP2_METHOD_ZSTD  1   /* zstd + trained dict (fast, ~90%)   */
#define PACKMP2_METHOD_ZPAQ  2   /* zpaq context-mixing (best, ~81%)   */
#define PACKMP2_METHOD_AUTO  0   /* auto-select based on file size      */

/* ---- compression levels (controls method complexity, higher = better ratio) ----
   zstd: level maps directly to zstd level clamped 1..6 (dict does heavy lifting).
   zpaq: 1=LZ77-fast  2=LZ77-longer  3=BWT+mix  4=BWT+ISSE+mix  5=full-CM. */
#define PACKMP2_LEVEL_STORE   0   /* store verbatim (no compression)              */
#define PACKMP2_LEVEL_FAST    1   /* zstd level 1  /  zpaq LZ77-fast              */
#define PACKMP2_LEVEL_DEFAULT 3   /* zstd level 1  /  zpaq BWT+mix   (best speed) */
#define PACKMP2_LEVEL_GOOD    4   /* zstd level 3  /  zpaq BWT+ISSE+mix (balance) */
#define PACKMP2_LEVEL_BEST    5   /* zstd level 6  /  zpaq full CM      (max)     */

/* ---- never-expand policy ---- */
#define PACKMP2_NEVER_EXPAND 1   /* if compressed >= original, store verbatim */
#define PACKMP2_ALLOW_EXPAND 0   /* always compress, even if it grows         */

/* ---- options struct (extensible, no ABI break on future fields) ---- */
typedef struct {
    int method;          /* PACKMP2_METHOD_ZSTD / _ZPAQ / _AUTO        */
    int level;           /* 0..5 (interpreted per method)               */
    int never_expand;    /* PACKMP2_NEVER_EXPAND (default) / ALLOW      */
    int reserved[8];     /* must be zero (future: threads, dict, etc.)  */
} packmp2_opts;

/* ---- default options ---- */
#define packmp2_opts_default() { PACKMP2_METHOD_AUTO, PACKMP2_LEVEL_DEFAULT, \
                                 PACKMP2_NEVER_EXPAND, {0,0,0,0,0,0,0,0} }

/* ---- API ---- */

/* Compress mp2 data into TCAM2 format.
   in/in_len   : raw MP2 bytes (MPEG Audio Layer II)
   out/out_len : malloc'd compressed output (caller frees with free())
   opts        : compression options (see packmp2_opts)
   msg[256]    : error diagnostic buffer (empty on success)
   Returns 0 on success, non-zero on failure. */
int packmp2_compress(const unsigned char *in,  size_t  in_len,
                           unsigned char **out, size_t *out_len,
                     const packmp2_opts *opts, char msg[256]);

/* Decompress TCAM2 format back to MP2.
   in/in_len   : compressed data (from packmp2_compress)
   out/out_len : malloc'd MP2 output (caller frees with free())
   msg[256]    : error diagnostic buffer (empty on success)
   Returns 0 on success, non-zero on failure. */
int packmp2_decompress(const unsigned char *in,  size_t  in_len,
                             unsigned char **out, size_t *out_len,
                       char msg[256]);

/* Peek at TCAM2 header without full decompression.
   Returns original MP2 size, or 0 if invalid header.
   msg[256] receives diagnostic on error. */
size_t packmp2_query_original_size(const unsigned char *in, size_t in_len,
                                   char msg[256]);

/* Version string (e.g. "0.5.0"). Always available. */
const char *packmp2_version(void);

#ifdef __cplusplus
}
#endif
#endif /* PACKMP2_H */
