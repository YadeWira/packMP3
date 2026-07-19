# packMP3

packMP3 is a lossless compression program for MPEG audio files —
MP3 (Layer III) and, as of v3.0, MP1/MP2 (Layer I/II). It reconstructs
the exact original file, bit for bit. MP3 is re-encoded with an
adaptive arithmetic coder (typical reduction: **~11-16%**); MP1/MP2 are
handled by the sibling [packMP2](https://github.com/YadeWira/packMP2)
library. Embedded ID3v2 cover art (JPEG or PNG) is losslessly
recompressed too, via the sibling [packJPG](https://github.com/YadeWira/packJPG)
and [packPNG](https://github.com/YadeWira/packPNG) libraries.

**Supported platforms:** Linux x64, Windows 7 SP1+ (x64 and x86).

**📖 [Wiki](https://github.com/YadeWira/packMP3/wiki)** — FAQ, format details, benchmarks, troubleshooting.


## Installation

Download the latest binary from the [Releases](https://github.com/YadeWira/packMP3/releases) page:

| File | Target |
|---|---|
| `packMP3_linux_x64` | Linux 64-bit |
| `packMP3_win_x64.exe` | Windows 10/11 64-bit (also runs on Windows 7 SP1 x64) |
| `packMP3_win_x86.exe` | Windows 7 SP1+ 32-bit |

Windows binaries are statically linked (no MSVC redistributable, no
UCRT/pthread DLL required) — they run on a clean Windows 7 install with
no extra setup.

### Building from source

MP1/MP2 and embedded cover-art support depend on three sibling projects,
[packMP2](https://github.com/YadeWira/packMP2),
[packJPG](https://github.com/YadeWira/packJPG) and
[packPNG](https://github.com/YadeWira/packPNG), vendored as **shallow**
git submodules for header provenance (`source/vendor/packmp2-src`,
`source/vendor/packjpg-src`, `source/vendor/packpng-src` — packJPG's own
repo history is large, so `--depth 1` matters):

```bash
git clone --recurse-submodules --shallow-submodules --depth 1 \
    https://github.com/YadeWira/packMP3
```

The prebuilt static libraries these headers pair with are **not**
vendored (packMP3 doesn't build any of the three from source) — build
`libpackmp2.a`/`libpackJPG.a`/`libpackpng.a` yourself from those repos
(`make lib`, non-LTO for packmp2/packjpg) and copy them into
`source/vendor/packmp2/`, `source/vendor/packjpg/` and
`source/vendor/packpng/` (plus `win64`/`win32` subdirs for the
cross-compile targets) before running `make`. packPNG's own `libpackpng.a`
bundles a fresh copy of packJPG internally, which collides (same C++
class/function names) with both packMP3's own code and the standalone
packJPG copy above — the vendored copy needs an extra `objcopy
--redefine-syms` pass first (map committed at
`source/vendor/packpng/redefine_map*.txt`); see the include comment above
`vendor/packpng-src/source/packpng.h` in `packmp3.cpp` for the exact
symbol-collision rationale if rebuilding this from scratch.


## Usage

```
packMP3 <subcommand> [switches] [filename(s)]
```

### Subcommands

| Subcommand | Description |
|---|---|
| `a` | compress MP3/MP2/MP1 files to `.pm3` (archive) |
| `x` | decompress `.pm3` files back to MP3/MP2/MP1 (extract) |
| `mix` | auto-detect and process both directions (use with caution) |
| `list` | display info about `.pm3` archives (MP3, MP2, or MP1) without decompressing |
| `stats` | show source file info (size, MPEG layer/version, channels, bitrate) without compressing |

packMP3 recognizes file types by content, not extension. MP3 goes to
the `.pm3` format (`"MS"`/`"MK"` magic); MP2 goes to a separate `.pm3`
container (`"M2"` magic) so the two never collide. Files that are
neither a recognized MPEG audio format nor a `.pm3` archive are
silently skipped. Wildcards (`*.mp3`, `*.*`) and drag-and-drop work;
on Windows, wildcard expansion is handled internally because `cmd.exe`
doesn't expand them.

In default mode files are never overwritten — packMP3 appends
underscores to make a fresh name. Pass `-o` to overwrite. Directories
are silently ignored unless `-r` is given.

If `"-"` is used as a filename, input is read from stdin and output is
written to stdout.

### Examples

```
packMP3 a *.mp3                        # compress everything in cwd
packMP3 a -k4 -o -np -od out/ *.mp3    # 4 parallel chunks/file, overwrite, no pause
packMP3 a -r music/                    # recurse into music/
packMP3 x *.pm3                        # decompress
packMP3 mix *.*                        # auto-detect each file
packMP3 list *.pm3                     # show version + size, no decompress
packMP3 - < song.pm3 > song.mp3        # stream
```

### `mix` — mixed mode

Auto-detects each file and compresses or decompresses accordingly.

> **Warning:** running `mix` on a folder that was already compressed
> will decompress the `.pm3` files back, undoing previous work.

### `list` — list `.pm3` info

Displays version, packed size, MPEG format, frame count, channels, rate
and bitrate — without decompressing.

```
$ packMP3 list -np song.pm3
  version  : v2.0
  packed   : 798.8 KB
  chunks   : 4 (intra-file parallel)
  format   : MPEG-1 Layer III
  frames   : 1423
  channels : 2 (joint stereo)
  rate     : 44100 Hz
  bitrate  : 192 kbps (CBR)
```

The `chunks` line only appears for archives made with `-k` > 1 (MP3
archives only — chunking doesn't apply to MP2).

`list`/`stats` also work on MP2 files and their `.pm3` archives (the
sync-scan is header-only, no full decode):

```
$ packMP3 stats -np song.mp2
  size     : 1.20 MB
  format   : MPEG-1 Layer II
  frames   : 3266
  channels : 2 (joint stereo)
  rate     : 48000 Hz
  bitrate  : 128 kbps (CBR)

$ packMP3 list -np song.pm3
  version  : v3.0
  packed   : 929.3 KB
  original : 1.20 MB
  method   : packMP2 (zstd/zpaq) (packMP2 v0.5.0)
  format   : MPEG-1 Layer II
  frames   : 3266
  channels : 2 (joint stereo)
  rate     : 48000 Hz
  bitrate  : 128 kbps (CBR)
```

### Embedded cover art

If an MP3's ID3v2 tag carries a JPEG or PNG cover (the common case for
tagged music files), packMP3 automatically recompresses it losslessly
— via [packJPG](https://github.com/YadeWira/packJPG) for JPEG covers,
[packPNG](https://github.com/YadeWira/packPNG) for PNG covers — instead
of storing it as generic bytes, shrinking files with high-resolution
artwork further, with no extra flag needed. It's self-verifying (the
recompressed image is decompressed and byte-compared before ever being
used) and silently falls back to the ordinary generic encoding for
anything unusual — other image formats (GIF, BMP, etc.), unsynchronised
tags, multiple pictures, or any parsing surprise. `-d` (discard
meta-info) skips this entirely along with the rest of the tag.


## Command-line switches

| Switch | Description |
|---|---|
| `-ver` | verify files after processing (encode → decode → byte-compare) |
| `-v?` | level of verbosity; 0, 1 or 2 (default 0) |
| `-vp` | progress bar mode (overrides `-v?`) |
| `-np` | no pause after processing files |
| `--no-color` | disable ANSI color output (also respected via `NO_COLOR` env var) |
| `-o` | overwrite existing files |
| `-od<path>` | write output files to directory `<path>` (created if needed) |
| `-th<n>` | worker threads for batch processing across files; `0` = auto (forces `-ver`) |
| `-k<n>` | intra-file parallel chunks for speed; default `1` = best ratio, `0` = auto |
| `-r` | recurse into subdirectories |
| `-fs` | preserve source folder structure under `-od` (use with `-r`) |
| `-dry` | dry run: simulate without writing output files |
| `-module` | machine-friendly output: `OK`/`ERROR` + elapsed time |
| `-p` | proceed on warnings |
| `-d` | discard meta-info (ID3 tags) |

Most of these switches — subcommands `a`/`x`/`list`, `-od`/`-r`/`-fs`/`-dry`/`-ver`/`-np`/`-o`/`-module`/`-th<n>`/`-p`/`-d`/`-v<n>` — follow a shared CLI convention coordinated with the sibling lossless-recompressor projects [packJPG](https://github.com/YadeWira/packJPG) and [packPNG](https://github.com/YadeWira/packPNG). Release binaries also share the `<name>_<platform>_<arch>[.exe]` naming pattern across all three.

### `-p` / `-d` / `-ver` — what they trade off

By default packMP3 cancels on warnings to guarantee bit-exact round-trip.

* `-p` accepts non-spec-compliant MP3 quirks and compresses anyway. The
  reconstructed MP3 **may not be byte-equal** to the original (no loss
  of audio data or quality, though).
* `-d` discards meta-info (ID3 tags) for smaller output. Reconstruction
  is no longer byte-equal.
* `-ver` does a full encode → decode → byte-compare per file. Files
  that fail verification are not written.

`-ver` should never be combined with `-p` or `-d` — those flags
intentionally drop byte-equality, so verification will always fail.


## Threading

packMP3 has two orthogonal threading modes:

| Flag | Granularity | Effect |
|---|---|---|
| `-th<n>` | across files | run N files in parallel, each on 1 thread |
| `-k<n>` | within a file | split one file into N independent chunks, each compressed/decompressed on its own thread |
| `-th<n> -k<m>` | both | batch of N files in parallel, each split into M chunks |

### `-k<n>` — intra-file parallel chunking

A `.pm3` archive is normally a single serial arithmetic stream — encode
and decode are inherently sequential. `-k<n>` splits the file at frame
boundaries into `n` independent sub-streams (each with its own model
state), so a **single file** can use multiple cores.

This trades a little compression ratio for speed — each chunk's
adaptive model starts cold instead of carrying statistics from the
whole file:

| `-k` | ratio (corpus avg) | encode | decode |
|---|---|---|---|
| `1` (default) | 88.6% | ~265 ms | ~274 ms |
| `2` | 89.2% | ~142 ms (~1.9×) | ~153 ms (~1.8×) |
| `4` | 89.8% | ~82 ms (~3.2×) | ~85 ms (~3.2×) |

Use `-k1` when ratio matters most (archival, bandwidth-constrained
transfer). Use `-k4`/`-k0` (auto) when speed matters most (interactive
tools, batch pipelines). `-k` values are format-visible — `list` shows
the chunk count — but every value reconstructs the input losslessly.

### `-th<n>` (multi-file batch)

`-th0` auto-detects core count. In batch mode, **verification is forced
on automatically** — every file is encode→decode→compared before the
output is committed.


## Other modes

### `-dry` — dry run

Simulates processing without writing any output. Useful to preview
ratios before committing to a batch.

```
packMP3 a -dry -np *.mp3
```

### `-module` — machine-friendly output

Single-line output: `OK <seconds>` or `ERROR <code> <seconds>`.


## Library / DLL API

packMP3 has a C-linkage library API for embedding into other
applications (archivers, media tools, servers). Same `.pm3` format as
the CLI.

### Building

```bash
cd source
make lib      # -> packMP3lib.a          static lib (Linux)
make so       # -> libpackMP3.so         Unix shared object (Linux/macOS)
make dll      # -> bin/packMP3.dll + bin/libpackMP3.a     Windows x64 (mingw cross-compile)
make dll-x86  # -> bin/packMP332.dll + bin/libpackMP332.a Windows x86
make dll-all  # both dll and dll-x86
```

Pre-built library bundles (matching the sibling projects' packaging) are
also attached to each [release](https://github.com/YadeWira/packMP3/releases):
`packMP3-<ver>-linux-x64-lib.tar.gz`, `packMP3-<ver>-win64-lib.zip`,
`packMP3-<ver>-win32-lib.zip` — each includes the library, headers, a
`.def` file for MSVC (`lib /def:packMP3.def /machine:x64`), and a short
README.

Header: `source/packmp3lib.h` for building the library itself,
`source/packmp3dll.h` for consumers linking against the shared
lib/DLL. Both wrap the `pmplib_*` declarations in `extern "C"`, so
exported symbols have plain, unmangled names.

### Functions

| Function | Purpose |
|---|---|
| `pmplib_convert_stream2stream(msg)` | Convert using the streams bound by `pmplib_init_streams` |
| `pmplib_convert_file2file(in, out, msg)` | Convenience wrapper: file → file |
| `pmplib_convert_stream2mem(**out, *out_size, msg)` | Convenience wrapper: bound input stream → memory buffer |
| `pmplib_init_streams(in_src, in_type, in_size, out_dest, out_type)` | Bind input/output streams (file, memory, or `FILE*`) for the next convert call |
| `pmplib_version_info()`, `pmplib_short_name()` | Version metadata |

`in_type`/`out_type`: `0` = file path, `1` = memory buffer (`in_size` =
buffer length), `2` = `FILE*` stream (e.g. `stdin`/`stdout`).

### Example

```c
#include "packmp3lib.h"
#include <stdio.h>

int main(void) {
    char msg[256] = {0};
    pmplib_init_streams("song.mp3", 0, 0, "song.pm3", 0);
    if (!pmplib_convert_stream2stream(msg)) {
        fprintf(stderr, "failed: %s\n", msg);
        return 1;
    }
    return 0;
}
```

### Current limitation: no thread/batch control in the library

The CLI's `-th`/`-k` threading is not yet exposed through the library
API — `pmplib_convert_*` calls are single-threaded, single-file
operations (no in-library batch function, no setter to enable
intra-file chunking). This was confirmed against the sibling projects'
library APIs, which do expose thread/batch control — closing this gap
is a future decision for packMP3, not a blocker for the current
release.

MP2 support and embedded cover-art recompression (JPEG and PNG covers,
both added during the v3.0 LTS pre-release series) are also CLI-only —
the library only ever handles MP3 (Layer III) `.pm3` archives, same
scope as the threading gap above.

### Windows DLL and `thread_local`

The DLL is built with mingw's default (win32) thread model and has been
verified to run cleanly on a real Windows 10 x64 VM (no crash, correct
output). Because the library currently never spawns threads internally
(see the limitation above), the DLL doesn't exercise the known
mingw/win32 issue where `thread_local` destructors can crash at process
exit for libraries that spawn worker threads across the DLL boundary.
If thread/batch control is added to the library in the future, revisit
this with the POSIX thread model mingw toolchain.


## Known limitations

packMP3 compresses MPEG audio; other file types are silently skipped.

MP3 may stand for three different audio file types: MPEG-1, MPEG-2 and
MPEG-2.5 Audio Layer III. As of v2.0, packMP3 compresses all three
(mono, stereo, joint stereo and dual channel; constant and variable
bitrate).

As of v3.0, MP1 (MPEG Audio Layer I) and MP2 (MPEG Audio Layer II) are
also supported, backed by the
[packMP2](https://github.com/YadeWira/packMP2) library — same `a`/`x`/
`list`/`stats`/`-ver` workflow, separate `.pm3` container (`"M2"`
magic) so neither collides with the MP3 format. Layer I has a
meaningfully different frame structure from Layer II (different
frame-length formula, no SCFSI, different bit-allocation tables), but
packMP2 handles both.

Some rare MP3 encodings are rejected (never damaged) rather than
compressed: free-format bitrate, and frames mixing long and short
blocks within one granule.

packMP3 has low error tolerance — MP3 files might not work with
packMP3 even if they play fine in audio software. `-p` increases error
tolerance and compatibility (see the `-p`/`-d`/`-ver` trade-off above).

Compressed archives are not always compatible across packMP3 major
versions — v2.0 changed the on-disk format, so v1.x `.pmp` files
cannot be decoded by v2.0 and vice versa. You'll get a clean error
message rather than garbage output if you try. v3.0 is an exception:
its format additions (MP2, MP1, embedded cover-art recompression) are
purely additive and version-gated, so v2.0/v2.1 `.pm3` archives still
decode correctly on the current build. Note the archive format has its
own internal version stamp, separate from the displayed "v3.0" product
version — it has ticked forward more than once during this still-ongoing
pre-release series as features were added (each bump gated the same
way), so an older pre-release binary will cleanly reject an archive
written by a newer one with a "newer build" error (rather than
misdecode it), while still reading everything older correctly. It's a
one-way street: older binaries can't read what a newer feature wrote,
but everything older keeps working on newer binaries.

On Windows, dragging too many files at once may show a
missing-privileges error; use the command line instead.


## Version numbering

* **Main version** (`appversion` in `packmp3.cpp`) is a two-digit
  number: `v{main/10}.{main%10}` — e.g. `20` → `v2.0`.
* Compressed archives are only guaranteed compatible within the same
  main version. A change in main version may break the on-disk format
  (v2.0 did).
* An optional sub-version string marks smaller, format-compatible
  changes (bug fixes, speed improvements): `a`, `b`, `c`, …

See `docs/versionnumbering.txt` for the full guideline.


## License

All programs in this package are free software; you can redistribute
them and/or modify them under the terms of the GNU Lesser General
Public License as published by the Free Software Foundation; either
version 3 of the License, or (at your option) any later version.

The package is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser
General Public License for more details at
http://www.gnu.org/copyleft/lgpl.html.

If the LGPL v3 license is not compatible with your software project you
might contact us and ask for a special permission to use the packMP3
algorithm under different conditions. In any case, usage of the
packMP3 algorithm under the LGPL v3 or above is highly advised and
special permissions will only be given where necessary on a case by
case basis. This offer is aimed mainly at closed source freeware
developers seeking to add `.pm3` support to their software projects.

Copyright 2010...2014 by Ratisbon University and Matthias Stirner.
Copyright 2010...2026 by Yade Bravo & Matthias Stirner.


## History

* **v3.0 (LTS, in pre-release)** — MP1/MP2 (MPEG Audio Layer I/II)
  support via the [packMP2](https://github.com/YadeWira/packMP2) library
  (`a`/`x`/`list`/`stats`/`-ver`, separate `"M2"` container); losslessly
  recompresses embedded ID3v2 cover art, JPEG via
  [packJPG](https://github.com/YadeWira/packJPG) and PNG via
  [packPNG](https://github.com/YadeWira/packPNG); format additions are
  backward-compatible with v2.0/v2.1 archives throughout.
* **v2.0** — full MP3 family (MPEG-1/2/2.5 Layer III, all channel
  modes, CBR/VBR), new `.pm3` extension, `-k` intra-file parallel
  chunking, retuned entropy models, link-time optimization and an
  optional profile-guided build (`make pgo`).
* **v1.0g** (2016) — updated contact info, minor bugfix.
* **v1.0f** (2014) — relicensed to LGPL v3.
* **v1.0e** (2014) — source optimizations (cppcheck).
* **v1.0d** (2013) — open-sourced under GPL v3.
* **v1.0c** (2012) — first public version.
* **v1.0** (2012) — first release (non-public, testing only).

Full commit-level history is in the git log.


## Acknowledgements

packMP3 is the result of countless hours of research and development.
It started as **Matthias Stirner**'s master's thesis project for
Ratisbon University, supervised by Prof. Dr. Christian Wolff.

Prof. Dr. Gerhard Seelmann from Hochschule Aalen introduced Matthias to
the field of data compression while studying at HTW Aalen University —
without him, neither packJPG nor packMP3 would exist.

Thanks to Stephan Busch of SqueezeChart.com for many hours of
beta-testing the original packMP3.

Logo and icon designed by Michael Kaufmann.


## Contact

* **Repository:** https://github.com/YadeWira/packMP3
* **Issues:** https://github.com/YadeWira/packMP3/issues
* **Discussion:** https://encode.su/threads/4517-packMP3-Full-MPEG
* **Support:** https://www.patreon.com/YadeWira
