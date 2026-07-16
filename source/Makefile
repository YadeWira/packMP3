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
OBJ      = $(patsubst %.c,%.o,$(CSRC)) $(patsubst %.cpp,%.o,$(CXXSRC))

# Cross-compilers
WIN64       = x86_64-w64-mingw32-g++
WIN32       = i686-w64-mingw32-g++
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

# Layer I/II backend: packMP2 library (sibling project, vendored .a + header
# under vendor/packmp2/ for now -- see vendor/packmp2/packmp2.h). -lstdc++
# is needed since packmp2's zpaq backend is C++.
LDFLAGS += -Lvendor/packmp2 -lpackmp2 -lstdc++

# Embedded ID3v2 cover-art (APIC) recompression: packJPG library (sibling
# project, vendored .a + header under vendor/packjpg/ -- see
# vendor/packjpg/packjpglib.h).
LDFLAGS += -Lvendor/packjpg -lpackJPG

# Windows-targeted packmp2/packjpg static libs (separate mingw-built .a's,
# since BUILD_LIB isn't set for the full-CLI win-x64/win-x86 cross-compile --
# unlike dll-x64/dll-x86, which never reference these symbols under
# BUILD_DLL and so don't need these).
PACKMP2_WIN64 = vendor/packmp2/win64
PACKMP2_WIN32 = vendor/packmp2/win32
PACKJPG_WIN64 = vendor/packjpg/win64
PACKJPG_WIN32 = vendor/packjpg/win32

# ---- Compiler-specific flags ----
ifeq ($(findstring clang,$(CXX)),clang)
  CXXFLAGS += -Wno-unknown-warning-option
else
  CXXFLAGS += -fsched-spec-load
endif

# ---- Rules ----
%.o: %.cpp $(DEPS)
	$(CXX) -c -o $@ $< $(CXXFLAGS)

%.res: %.rc
	@-$(RC) $< $@

$(TARGET): $(OBJ) $(RES)
	$(CXX) -o $@ $^ -s $(LDFLAGS)
	$(UPX)

.PHONY: all dev lib so dll dll-x64 dll-x86 dll-all clean linux-x64 win-x64 win-x86 all-platforms pgo

all: $(TARGET)

# Developer build: debug symbols, assertions
dev: CXXFLAGS += -DDEV_BUILD -O0 -g
dev: $(TARGET)

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
lib: CXXFLAGS += -DBUILD_LIB
lib: $(OBJ)
	ar rcs $(TARGET)lib.a $(OBJ)

# Shared library (Linux/macOS: lib<name>.so, no import lib needed on ELF)
# -fPIC is required: the codec's thread_local globals need the
# position-independent (global-dynamic) TLS model to be usable from a
# shared object; without it, ld fails with a TPOFF32 relocation error.
so: CXXFLAGS += -DBUILD_DLL -fvisibility=hidden -fPIC
so: $(OBJ)
	$(CXX) -shared -o lib$(TARGET).so $^ $(LDFLAGS)

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
	@-rm -rf *.o *.a *.so *.gcda $(PGODIR) $(TARGET) $(TARGET).exe $(TARGET).dll \
	         bin/$(TARGET)_linux_x64 \
	         bin/$(TARGET)_linux_x64_native \
	         bin/$(TARGET)_win_x64.exe \
	         bin/$(TARGET)_win_x86.exe \
	         bin/$(TARGET).dll \
	         bin/lib$(TARGET).a \
	         bin/$(TARGET)32.dll \
	         bin/lib$(TARGET)32.a

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

win-x64: bin
	$(WIN64) $(CROSS_FLAGS) -o bin/$(TARGET)_win_x64.exe $(CXXSRC) -s $(CROSS_LD) -L$(PACKMP2_WIN64) -lpackmp2 -L$(PACKJPG_WIN64) -lpackJPG -lstdc++
	@echo "[OK] bin/$(TARGET)_win_x64.exe"

win-x86: bin
	$(WIN32) $(CROSS_FLAGS) -o bin/$(TARGET)_win_x86.exe $(CXXSRC) -s $(CROSS_LD) -L$(PACKMP2_WIN32) -lpackmp2 -L$(PACKJPG_WIN32) -lpackJPG -lstdc++
	@echo "[OK] bin/$(TARGET)_win_x86.exe"

all-platforms: linux-x64 win-x64 win-x86
	@echo ""
	@echo "Output binaries:"
	@ls -lh bin/
