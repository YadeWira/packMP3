# packMP3 Makefile
# Originally by Matthias Stirner, 01/2016
# Updated 2026: clang support, cross-compilation, C++17, fixed Linux flags

TARGET   = packMP3

# Compiler: override with make CXX=clang++ or make CXX=g++
CXX     ?= g++
RC       = windres -O coff

# Base flags
CXXFLAGS  = -I. -std=c++17 -O3 -Wall -Wextra -pedantic \
             -funroll-loops -ffast-math -fomit-frame-pointer
LDFLAGS   =

CSRC     = $(wildcard *.c)
CXXSRC   = $(wildcard *.cpp)
DEPS     = $(wildcard *.h) Makefile
OBJ      = $(patsubst %.c,%.o,$(CSRC)) $(patsubst %.cpp,%.o,$(CXXSRC))

# Cross-compilers
WIN64       = x86_64-w64-mingw32-g++
WIN32       = i686-w64-mingw32-g++
WINDRES64   = x86_64-w64-mingw32-windres
WINDRES32   = i686-w64-mingw32-windres
CROSS_FLAGS = -I. -std=c++17 -O3 -Wall -funroll-loops -ffast-math -fomit-frame-pointer
CROSS_LD    = -static -static-libgcc -static-libstdc++

# ---- Platform detection ----
ifeq ($(OS),Windows_NT)
  LDFLAGS  += -static -static-libgcc -static-libstdc++ -lpthread -L libwinpthread-1.dll
  RES       = icons.res
  UPX      := upx --best --lzma $(TARGET).exe 2>/dev/null || true
else
  CXXFLAGS += -DUNIX
  # -static is opt-in on Linux (pass STATIC=1)
  ifeq ($(STATIC),1)
    LDFLAGS += -static -static-libgcc -static-libstdc++
  endif
  RC  =
  RES =
  UPX =
endif

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

.PHONY: all dev lib dll clean linux-x64 win-x64 win-x86 all-platforms

all: $(TARGET)

# Developer build: debug symbols, assertions
dev: CXXFLAGS += -DDEV_BUILD -O0 -g
dev: $(TARGET)

# Static library
lib: CXXFLAGS += -DBUILD_LIB
lib: $(OBJ)
	ar rcs $(TARGET)lib.a $(OBJ)

# Shared library
dll: CXXFLAGS += -DBUILD_DLL -fvisibility=hidden
dll: LDFLAGS  += -Wl,--out-implib,lib$(TARGET).a
dll: $(OBJ)
	$(CXX) -shared -o $(TARGET).so $^ $(LDFLAGS)

clean:
	@echo "Cleaning..."
	@-rm -f *.o *.a *.so $(TARGET) $(TARGET).exe $(TARGET).dll \
	         bin/$(TARGET)_linux_x64 \
	         bin/$(TARGET)_linux_x64_native \
	         bin/$(TARGET)_win_x64.exe \
	         bin/$(TARGET)_win_x86.exe

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
	$(WIN64) $(CROSS_FLAGS) -o bin/$(TARGET)_win_x64.exe $(CXXSRC) -s $(CROSS_LD)
	@echo "[OK] bin/$(TARGET)_win_x64.exe"

win-x86: bin
	$(WIN32) $(CROSS_FLAGS) -o bin/$(TARGET)_win_x86.exe $(CXXSRC) -s $(CROSS_LD)
	@echo "[OK] bin/$(TARGET)_win_x86.exe"

all-platforms: linux-x64 win-x64 win-x86
	@echo ""
	@echo "Output binaries:"
	@ls -lh bin/
