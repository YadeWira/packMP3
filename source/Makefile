# packMP3 Makefile
# Originally by Matthias Stirner, 01/2016
# Updated 2026: clang support, fixed Linux static flags, C++17, snprintf guards

TARGET   = packMP3

# Compiler selection: use CXX from environment or default to g++
# Override with: make CXX=clang++ or make CXX=g++
CXX     ?= g++
RC       = windres -O coff

# Base flags — shared across all platforms and compilers
CXXFLAGS  = -I. -std=c++17 -O3 -Wall -Wextra -pedantic \
             -funroll-loops -ffast-math -fomit-frame-pointer
LDFLAGS   =

CSRC     = $(wildcard *.c)
CXXSRC   = $(wildcard *.cpp)
DEPS     = $(wildcard *.h) Makefile
OBJ      = $(patsubst %.c,%.o,$(CSRC)) $(patsubst %.cpp,%.o,$(CXXSRC))

# ---- Platform detection ----
ifeq ($(OS),Windows_NT)
  LDFLAGS  += -static -static-libgcc -static-libstdc++ -lpthread -L libwinpthread-1.dll
  RES       = icons.res
  UPX      := upx --best --lzma $(TARGET).exe
else
  CXXFLAGS += -DUNIX
  # On Linux, -static requires all libs to have static variants available.
  # We leave it out by default; pass STATIC=1 to opt in.
  ifeq ($(STATIC),1)
    LDFLAGS += -static -static-libgcc -static-libstdc++
  endif
  RC  =
  RES =
  UPX =
endif

# ---- Compiler-specific flags ----
ifeq ($(findstring clang,$(CXX)),clang)
  # clang does not support -fsched-spec-load
  CXXFLAGS += -Wno-unknown-warning-option
else
  # GCC-specific scheduler optimization
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

.PHONY: all dev lib dll clean

all: $(TARGET)

# Developer build: extra assertions, debug symbols
dev: CXXFLAGS += -DDEV_BUILD -O0 -g
dev: $(TARGET)

# Static library build
lib: CXXFLAGS += -DBUILD_LIB
lib: $(OBJ)
	ar rcs $(TARGET)lib.a $(OBJ)

# Shared library build
dll: CXXFLAGS += -DBUILD_DLL -fvisibility=hidden
dll: LDFLAGS  += -Wl,--out-implib,lib$(TARGET).a
dll: $(OBJ)
	$(CXX) -shared -o $(TARGET).so $^ $(LDFLAGS)

clean:
	@echo "Cleaning..."
	@-rm -f *.o *.a *.so $(TARGET) $(TARGET).exe $(TARGET).dll
