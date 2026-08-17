# packMP3 Makefile
# Originally by Matthias Stirner, 01/2016
# Updated 2026: clang support, cross-compilation, C++17, fixed Linux flags

TARGET   = packMP3

# Compiler: override with make CXX=clang++ or make CXX=g++
CXX     ?= g++
RC       = windres -O coff

# Base flags
CXXFLAGS  = -I. -std=c++17 -O3 -flto=auto -Wall -Wextra -pedantic \
             -funroll-loops -ffast-math -fomit-frame-pointer
LDFLAGS   = -flto=auto

CSRC     = $(wildcard *.c)
CXXSRC   = $(wildcard *.cpp)
DEPS     = $(wildcard *.h) Makefile

# One object directory per build variant. The CLI, `dev`, `lib` and `so`
# compile the SAME sources with mutually incompatible flags (-O0 -g /
# -DBUILD_LIB / -DBUILD_DLL -fvisibility=hidden -fPIC). make only compares
# .o-vs-source timestamps -- it has no idea which flags an object was built
# with -- so sharing one set of .o files means a stale object from another
# variant is silently reused. That bit twice in practice: `make so` after
# `make lib` failed outright with a TPOFF32 relocation error (non-PIC
# objects reused for a shared library), and `make dev` after `make` silently
# relinked the -O3 objects, producing a "debug" build with no debug in it.
# Separate directories make every combination correct without `make clean`
# in between.
OBJNAMES = $(patsubst %.c,%.o,$(CSRC)) $(patsubst %.cpp,%.o,$(CXXSRC))
CLIOBJ   = $(addprefix .obj/cli/,$(OBJNAMES))
DEVOBJ   = $(addprefix .obj/dev/,$(OBJNAMES))
LIBOBJ   = $(addprefix .obj/lib/,$(OBJNAMES))
SOOBJ    = $(addprefix .obj/so/,$(OBJNAMES))

# Cross-compilers.
#
# The -posix suffix is mandatory, not a preference. Debian ships two mingw
# g++ drivers per target: the default one uses the win32 thread model, the
# -posix one uses winpthreads. They are NOT link-compatible in the presence
# of C++ threading: std::mutex, std::thread and std::call_once resolve to
# different runtimes.
#
# The vendored libpackJPG.a is built by packJPG with the -posix driver (see
# its own Makefile) and carries undefined pthread_* references. Linking it
# into a win32-thread-model binary produced a hard hang: compression of an
# embedded JPEG cover succeeded, the self-verify decompression right after
# it blocked forever with the process at ~0.5s of CPU. That shipped in
# v3.0c -- any MP3 with a JPEG cover hung the Windows build. Linux was
# never affected.
#
# Verified by isolating it: a 25-line program linking only this .a hangs on
# the decode when built with the default driver and round-trips byte-exact
# when built with -posix, same .a, nothing else changed. packJPG reproduced
# it independently with its own harness and numbers.
#
# There is no noisy version of this failure. The mismatched link succeeds:
# exit 0, a complete .exe, not one warning. A deadlock at runtime is the
# only symptom there is, which is why this comment is long.
#
# Note the direction of the requirement. It is not "the sibling libraries
# must be built posix" -- packJPG's already was. It is that *every consumer
# of them compiles its own objects with -posix too*, this project included,
# because a posix .a absorbed into a win32-model host is indistinguishable
# from a correct build at link time. Nothing the library author can put in
# the .a, its build or its API detects it downstream.
#
# Check with `x86_64-w64-mingw32-g++-posix -v 2>&1 | grep "Thread model"`;
# it must say posix. If a vendored sibling library ever switches thread
# models, the mismatch is what to look for first: `nm --undefined-only` on
# it should show pthread_* references and no __gthr_win32_* ones.
WIN64       = x86_64-w64-mingw32-g++-posix
WIN32       = i686-w64-mingw32-g++-posix
WINDRES64   = x86_64-w64-mingw32-windres
WINDRES32   = i686-w64-mingw32-windres
CROSS_FLAGS = -I. -std=c++17 -O3 -flto=auto -Wall -funroll-loops -ffast-math -fomit-frame-pointer -pthread
CROSS_LD    = -flto=auto -static -static-libgcc -static-libstdc++ -pthread

# ---- Platform detection ----
ifeq ($(OS),Windows_NT)
  LDFLAGS  += -static -static-libgcc -static-libstdc++ -lpthread -L libwinpthread-1.dll
  RES       = icons.res
  UPX      := upx --best --lzma $(TARGET).exe 2>/dev/null || true
else
  CXXFLAGS += -DUNIX -pthread
  LDFLAGS  += -pthread
  # -static is opt-in on Linux (pass STATIC=1)
  ifeq ($(STATIC),1)
    LDFLAGS += -static -static-libgcc -static-libstdc++
  endif
  RC  =
  RES =
  UPX =
endif

# Layer I/II backend: packMP2 library (sibling project). Header comes from
# the vendor/packmp2-src/ git submodule (see .gitmodules); the prebuilt
# static lib is a separate, gitignored build artifact under vendor/packmp2/
# (run `make lib` in the packMP2 repo and copy libpackmp2.a in). -lstdc++
# is needed since packmp2's zpaq backend is C++.
LDFLAGS += -Lvendor/packmp2 -lpackmp2 -lstdc++

# Embedded ID3v2 cover-art (APIC) recompression: packJPG library (sibling
# project). Header comes from the vendor/packjpg-src/ git submodule; the
# prebuilt static lib under vendor/packjpg/ is a separate build artifact
# (non-LTO build + objcopy --redefine-syms -- see the include comment in
# packmp3.cpp for the ODR-collision rename rationale, and
# vendor/packjpg/redefine_map*.txt for the actual symbol map).
LDFLAGS += -Lvendor/packjpg -lpackJPG

# Embedded ID3v2 cover-art (APIC) recompression, PNG covers: packPNG library
# (sibling project). Header comes from the vendor/packpng-src/ git submodule;
# the prebuilt static lib under vendor/packpng/ is a separate build artifact.
# packPNG's own `make lib` produces a *fat* archive that bundles its own copy
# of packJPG (it uses packJPG internally). Up to packPNG v2.0c that embedded
# copy collided with the packJPG we vendor separately and needed its own
# objcopy --redefine-syms pass here; from v2.0d packPNG isolates those symbols
# in its own build (ppng_pjg_ prefix), so no rename is needed on our side --
# see the include comment in packmp3.cpp. -lz -llzma -lzstd are packPNG's
# remaining external deps (the fat archive bundles kanzi+preflate but not
# these system compression libs).
LDFLAGS += -Lvendor/packpng -lpackpng -lz -llzma -lzstd

# Windows-targeted packmp2/packjpg/packpng static libs (separate mingw-built
# .a's, since BUILD_LIB isn't set for the full-CLI win-x64/win-x86
# cross-compile -- unlike dll-x64/dll-x86, which never reference these
# symbols under BUILD_DLL and so don't need these).
PACKMP2_WIN64 = vendor/packmp2/win64
PACKMP2_WIN32 = vendor/packmp2/win32
PACKJPG_WIN64 = vendor/packjpg/win64
PACKJPG_WIN32 = vendor/packjpg/win32
PACKPNG_WIN64 = vendor/packpng/win64
PACKPNG_WIN32 = vendor/packpng/win32
# packPNG's Windows static libs need the same mingw-built zlib/lzma/zstd
# used by its own build (see PPNG's vendor/mingw-deps{,-x86}/lib) -- these
# are NOT bundled into the .a, unlike kanzi/preflate.
PACKPNG_DEPS_WIN64 = vendor/packpng/win64/deps
PACKPNG_DEPS_WIN32 = vendor/packpng/win32/deps

# ---- Compiler-specific flags ----
ifeq ($(findstring clang,$(CXX)),clang)
  CXXFLAGS += -Wno-unknown-warning-option
else
  CXXFLAGS += -fsched-spec-load
endif

# ---- Rules ----
.obj/cli/%.o: %.cpp $(DEPS)
	@mkdir -p $(@D)
	$(CXX) -c -o $@ $< $(CXXFLAGS)

.obj/dev/%.o: %.cpp $(DEPS)
	@mkdir -p $(@D)
	$(CXX) -c -o $@ $< $(CXXFLAGS) -DDEV_BUILD -O0 -g

.obj/lib/%.o: %.cpp $(DEPS)
	@mkdir -p $(@D)
	$(CXX) -c -o $@ $< $(CXXFLAGS) -DBUILD_LIB

# -fPIC is required: the codec's thread_local globals need the
# position-independent (global-dynamic) TLS model to be usable from a
# shared object; without it, ld fails with a TPOFF32 relocation error.
.obj/so/%.o: %.cpp $(DEPS)
	@mkdir -p $(@D)
	$(CXX) -c -o $@ $< $(CXXFLAGS) -DBUILD_DLL -fvisibility=hidden -fPIC

%.res: %.rc
	@-$(RC) $< $@

$(TARGET): $(CLIOBJ) $(RES)
	$(CXX) -o $@ $^ -s $(LDFLAGS)
	$(UPX)

.PHONY: all dev lib so dll dll-x64 dll-x86 dll-all clean linux-x64 win-x64 win-x86 all-platforms pgo deb test

# Regression suite: reversibility AND proof that each codec actually ran.
# Generates its own corpus on first use. See tests/regression.sh for why the
# second check exists -- a round-trip alone cannot see a dead codec.
test: $(TARGET)
	@bash ../tests/make_testdata.sh >/dev/null
	@bash ../tests/regression.sh

all: $(TARGET)

# Developer build: debug symbols, assertions. Two deliberate differences
# from before:
#  - linked WITHOUT -s. The release rule strips the binary, which used to
#    throw away the very -g symbols this target exists to produce.
#  - written to $(TARGET)-dev, not $(TARGET). Sharing one output name meant
#    `make` after `make dev` saw an up-to-date file and did not relink, so
#    you silently kept running the debug binary (and vice versa).
dev: $(TARGET)-dev

$(TARGET)-dev: $(DEVOBJ) $(RES)
	$(CXX) -o $@ $^ $(LDFLAGS)
	@echo "[OK] $(TARGET)-dev (unstripped, -O0 -g, DEV_BUILD)"

# Profile-guided optimization: instrument, train on the benchmark corpus
# (generated if absent), then rebuild using the collected profile. Format-safe:
# identical output, only faster. Needs lame+ffmpeg for the corpus.
PGODIR := $(CURDIR)/.pgo
pgo:
	@echo "[PGO] 1/3 instrumented build"
	$(CXX) $(CXXFLAGS) -fprofile-generate -fprofile-dir=$(PGODIR) -o $(TARGET) $(CXXSRC) $(LDFLAGS) -fprofile-generate
	@test -n "$$(ls ../corpus/*.mp3 2>/dev/null)" || bash ../scripts/make_corpus.sh ../corpus
	@echo "[PGO] 2/3 training on corpus (compress + decompress)"
	@rm -rf $(PGODIR)/run && mkdir -p $(PGODIR)/run/d
	@for f in ../corpus/*.mp3; do ./$(TARGET) a -np -o -od$(PGODIR)/run "$$f" >/dev/null 2>&1; done
	@for f in $(PGODIR)/run/*.pm3; do ./$(TARGET) x -np -o -od$(PGODIR)/run/d "$$f" >/dev/null 2>&1; done
	@echo "[PGO] 3/3 optimized rebuild"
	$(CXX) $(CXXFLAGS) -fprofile-use -fprofile-dir=$(PGODIR) -fprofile-correction -Wno-coverage-mismatch -o $(TARGET) $(CXXSRC) $(LDFLAGS)
	@echo "[OK] PGO build complete"

# Static library
lib: $(LIBOBJ)
	ar rcs $(TARGET)lib.a $(LIBOBJ)

# Shared library (Linux/macOS: lib<name>.so, no import lib needed on ELF).
# See the .obj/so/%.o rule above for why -fPIC is mandatory here.
so: $(SOOBJ)
	$(CXX) -shared -o lib$(TARGET).so $(SOOBJ) $(LDFLAGS)

# Windows DLL (cross-compile with mingw): packMP3.dll + import lib for linking.
# dll = 64-bit (most common ask); dll-x86 = 32-bit; dll-all = both.
dll: dll-x64
dll-all: dll-x64 dll-x86

dll-x64: bin
	$(WIN64) -I. -std=c++17 -O3 -flto=auto -Wall -funroll-loops -ffast-math \
	  -fomit-frame-pointer -DUNIX -DBUILD_DLL -pthread -shared \
	  -o bin/$(TARGET).dll $(CXXSRC) -s -flto=auto \
	  -static -static-libgcc -static-libstdc++ -pthread \
	  -Wl,--out-implib,bin/lib$(TARGET).a
	@echo "[OK] bin/$(TARGET).dll + bin/lib$(TARGET).a"

dll-x86: bin
	$(WIN32) -I. -std=c++17 -O3 -flto=auto -Wall -funroll-loops -ffast-math \
	  -fomit-frame-pointer -DUNIX -DBUILD_DLL -pthread -shared \
	  -o bin/$(TARGET)32.dll $(CXXSRC) -s -flto=auto \
	  -static -static-libgcc -static-libstdc++ -pthread \
	  -Wl,--out-implib,bin/lib$(TARGET)32.a
	@echo "[OK] bin/$(TARGET)32.dll + bin/lib$(TARGET)32.a"

clean:
	@echo "Cleaning..."
	@-rm -rf .obj *.o *.a *.so *.gcda $(PGODIR) $(TARGET) $(TARGET)-dev $(TARGET).exe $(TARGET).dll \
	         bin/$(TARGET)_linux_x64 \
	         bin/$(TARGET)_linux_x64_native \
	         bin/$(TARGET)_win_x64.exe \
	         bin/$(TARGET)_win_x86.exe \
	         bin/$(TARGET).dll \
	         bin/lib$(TARGET).a \
	         bin/$(TARGET)32.dll \
	         bin/lib$(TARGET)32.a \
	         bin/packmp3_*_amd64.deb

# ---- Cross-compilation targets -----------------------------------------------
#
# Requires mingw-w64:
#   Ubuntu/Debian : sudo apt install mingw-w64
#   Arch          : sudo pacman -S mingw-w64-gcc
#   macOS         : brew install mingw-w64
#
# Usage:
#   make linux-x64      -> bin/packMP3_linux_x64
#   make win-x64        -> bin/packMP3_win_x64.exe
#   make win-x86        -> bin/packMP3_win_x86.exe
#   make all-platforms  -> all three

bin:
	mkdir -p bin

linux-x64: bin
	$(CXX) $(CXXFLAGS) -o bin/$(TARGET)_linux_x64 $(CXXSRC) -s $(LDFLAGS)
	@echo "[OK] bin/$(TARGET)_linux_x64"

linux-x64-native: bin
	$(CXX) $(CXXFLAGS) -march=native -o bin/$(TARGET)_linux_x64_native $(CXXSRC) -s $(LDFLAGS)
	@echo "[OK] bin/$(TARGET)_linux_x64_native (native, do not distribute)"

# Debian package (manual for now -- run this and `gh api .../dispatches`
# by hand per release, same as the rest of the release process; see
# ../packaging/debian/control for the version/deps/description to keep in
# sync with each release). Needs dpkg-deb + fakeroot.
DEBSTAGE = /tmp/packmp3_deb_build
deb: linux-x64
	rm -rf $(DEBSTAGE)
	mkdir -p $(DEBSTAGE)/DEBIAN $(DEBSTAGE)/usr/bin
	cp bin/$(TARGET)_linux_x64 $(DEBSTAGE)/usr/bin/packmp3
	chmod 755 $(DEBSTAGE) $(DEBSTAGE)/usr $(DEBSTAGE)/usr/bin $(DEBSTAGE)/usr/bin/packmp3
	cp ../packaging/debian/control $(DEBSTAGE)/DEBIAN/control
	debver=$$(awk -F': ' '/^Version:/{print $$2}' $(DEBSTAGE)/DEBIAN/control); \
	fakeroot dpkg-deb --build $(DEBSTAGE) bin/packmp3_$${debver}_amd64.deb; \
	echo "[OK] bin/packmp3_$${debver}_amd64.deb"

win-x64: bin
	$(WIN64) $(CROSS_FLAGS) -o bin/$(TARGET)_win_x64.exe $(CXXSRC) -s $(CROSS_LD) \
	  -L$(PACKMP2_WIN64) -lpackmp2 -L$(PACKJPG_WIN64) -lpackJPG \
	  -L$(PACKPNG_WIN64) -lpackpng -L$(PACKPNG_DEPS_WIN64) -llzma -lzstd -lz \
	  -lstdc++ -lws2_32 -luserenv -lbcrypt -lntdll
	@echo "[OK] bin/$(TARGET)_win_x64.exe"

win-x86: bin
	$(WIN32) $(CROSS_FLAGS) -o bin/$(TARGET)_win_x86.exe $(CXXSRC) -s $(CROSS_LD) \
	  -L$(PACKMP2_WIN32) -lpackmp2 -L$(PACKJPG_WIN32) -lpackJPG \
	  -L$(PACKPNG_WIN32) -lpackpng -L$(PACKPNG_DEPS_WIN32) -llzma -lzstd -lz \
	  -lstdc++ -lws2_32 -luserenv -lbcrypt -lntdll
	@echo "[OK] bin/$(TARGET)_win_x86.exe"

all-platforms: linux-x64 win-x64 win-x86
	@echo ""
	@echo "Output binaries:"
	@ls -lh bin/
