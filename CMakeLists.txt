cmake_minimum_required(VERSION 3.14)
project(packMP3 VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Sources ----
set(PACKMP3_SOURCES
    source/aricoder.cpp
    source/bitops.cpp
    source/huffmp3.cpp
    source/packmp3.cpp
)

set(PACKMP3_HEADERS
    source/aricoder.h
    source/bitops.h
    source/huffmp3.h
    source/huffmp3tbl.h
    source/packmp3dll.h
    source/packmp3lib.h
    source/pmp3bitlen.h
    source/pmp3tbl.h
)

# ---- Common compiler flags ----
set(PACKMP3_COMPILE_OPTIONS
    -Wall -Wextra -pedantic
    -funroll-loops -ffast-math -fomit-frame-pointer
)

# GCC-specific flags
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND PACKMP3_COMPILE_OPTIONS -fsched-spec-load)
endif()

# ---- CLI Executable ----
add_executable(packMP3 ${PACKMP3_SOURCES})
target_include_directories(packMP3 PRIVATE source)
target_compile_definitions(packMP3 PRIVATE UNIX)
target_compile_options(packMP3 PRIVATE ${PACKMP3_COMPILE_OPTIONS})

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(packMP3 PRIVATE DEV_BUILD)
endif()

# ---- Static Library ----
add_library(packMP3lib STATIC ${PACKMP3_SOURCES})
target_include_directories(packMP3lib PUBLIC source)
target_compile_definitions(packMP3lib PRIVATE BUILD_LIB UNIX)
target_compile_options(packMP3lib PRIVATE ${PACKMP3_COMPILE_OPTIONS})

# ---- Shared Library ----
add_library(packMP3dll SHARED ${PACKMP3_SOURCES})
target_include_directories(packMP3dll PUBLIC source)
target_compile_definitions(packMP3dll PRIVATE BUILD_DLL UNIX)
target_compile_options(packMP3dll PRIVATE ${PACKMP3_COMPILE_OPTIONS})
set_target_properties(packMP3dll PROPERTIES
    OUTPUT_NAME packMP3
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# ---- Install ----
include(GNUInstallDirs)
install(TARGETS packMP3
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(TARGETS packMP3lib packMP3dll
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES source/packmp3lib.h source/packmp3dll.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/packMP3
)

# ---- Summary ----
message(STATUS "packMP3 ${PROJECT_VERSION}")
message(STATUS "  Compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build    : ${CMAKE_BUILD_TYPE}")
