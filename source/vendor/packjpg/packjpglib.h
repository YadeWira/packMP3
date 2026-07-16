// packJPGlib.h - function declarations for the packJPG library
//
// VENDORED HEADER -- pinned copy from packJPG (sibling project, see
// /home/forum/git/packJPG). The prebuilt libpackJPG.a this header pairs
// with is a local build artifact, not committed (see .gitignore *.a rule).
//
// IMPORTANT for CLI-only builds (packMP3's non-BUILD_LIB targets): packMP3
// and packJPG share inherited coder architecture and define C++ classes
// with IDENTICAL names (model_s/model_b) -- linking both .o sets directly
// causes a real "multiple definition" ODR violation, not cosmetic. Fixed by
// renaming packJPG's internal model_s/model_b symbols in this vendored copy
// only (never touches packJPG's own source) via `objcopy --redefine-syms`
// with the map in redefine_map.txt (Linux/win64) / redefine_map_win32.txt
// (32-bit PE-COFF prefixes an extra leading underscore on every symbol).
// To rebuild after a packJPG update: get a NON-LTO static lib (`make lib`
// without -flto -- LTO-compiled objects reject objcopy symbol renaming),
// `ar x` it, `objcopy --redefine-syms=redefine_map.txt` each .o, `ar rcs`
// back into libpackJPG.a. Windows libs use the equivalent mingw cross
// -compiled non-LTO static build (packJPG's `make mingw-lib`/`mingw64-lib`).
#if defined BUILD_DLL
	// DLL build: C-linkage + dllexport. The order matters — __declspec
	// attaches to the C-linkage declaration that follows.
	#define EXPORT extern "C" __declspec( dllexport )
#elif defined BUILD_SO
	// Unix shared object (.so): C-linkage + explicit default visibility, so
	// the public API stays exported even when the .so is built with
	// -fvisibility=hidden. Only set by the Makefile `so` target (gcc/clang),
	// so the attribute never reaches MSVC.
	#define EXPORT extern "C" __attribute__(( visibility( "default" ) ))
#elif defined __cplusplus
	#define EXPORT extern "C"   // ensure C-linkage when building as a static lib for FFI hosts
#else
	#define EXPORT extern
#endif

/* C99 / C++ bool shim. The lib API uses `bool` in three places; C
   consumers need stdbool.h, C++ gets it from the language. */
#if !defined(__cplusplus)
#include <stdbool.h>
#endif

// Minimum buffer size callers must allocate for the msg parameter
// in pjglib_convert_* functions.
#define PJG_MSG_SIZE 512

// Output buffers returned by pjglib_convert_stream2mem via *out_file
// are allocated with malloc(). Callers must free them with free(),
// NOT with delete[] (mixing allocators is undefined behaviour and is
// flagged as an error by AddressSanitizer).

/* -----------------------------------------------
	function declarations: library only functions
	----------------------------------------------- */

EXPORT bool pjglib_convert_stream2stream( char* msg );
EXPORT bool pjglib_convert_file2file( char* in, char* out, char* msg );
EXPORT bool pjglib_convert_stream2mem( unsigned char** out_file, unsigned int* out_size, char* msg );
EXPORT void pjglib_init_streams( void* in_src, int in_type, int in_size, void* out_dest, int out_type );
EXPORT const char* pjglib_version_info( void );
EXPORT const char* pjglib_short_name( void );

/* -----------------------------------------------
	function declarations: library threading controls
	----------------------------------------------- */

/* Intra-file threads: parallelism within a single file (Y/Cb/Cr in
   parallel). Same code path as the CLI -sfth flag.

   n =  0  default = auto: ON if host has >=3 logical cores
   n =  1  force OFF (single-threaded per file)
   n >= 3  force ON  (each file uses 3 worker threads)

   Affects all subsequent pjglib_convert_* and pjglib_convert_batch calls.
   Default resolution happens lazily on the first convert call. Setters
   are NOT thread-safe — call them during single-threaded init, before
   spawning workers. */
EXPORT void pjglib_set_intra_file_threads( int n );
EXPORT int  pjglib_get_intra_file_threads( void );

/* Inter-file threads: parallelism across files inside pjglib_convert_batch.
   Each worker handles one op at a time and uses intra_file_threads for the
   encode/decode inside that op.

   n =  0  default = 1 (sequential batch)
   n >= 1  N concurrent worker threads

   Same thread-safety rule as the intra setter: call during init only. */
EXPORT void pjglib_set_inter_file_threads( int n );
EXPORT int  pjglib_get_inter_file_threads( void );

/* Decompression-bomb guard. Cap the size (bytes) of a JPEG the decoder will
   reconstruct from a .pjg; 0 = unlimited (default). When set, decoding a .pjg
   whose output would exceed n bytes fails cleanly instead of producing it.
   Recommended for hosts that decode untrusted .pjg input. Process-wide; set
   during single-threaded init. */
EXPORT void         pjglib_set_max_output_size( unsigned int n );
EXPORT unsigned int pjglib_get_max_output_size( void );

/* Helper: pjglib_suggest_batch_threads() returns max(1, cores/3), which
   keeps total thread budget (inter * intra) close to cores. Useful for
   archivers picking a default value at startup. */
EXPORT int  pjglib_suggest_batch_threads( void );

/* One input/output pair in a batch conversion. Same stream-type semantics
   as pjglib_init_streams (in_type/out_type: 0=file, 1=memory, 2=stream).
   For memory I/O the caller must keep in_src's buffer alive until
   pjglib_convert_batch returns. For file I/O the library creates output
   files unless out_dest is NULL (in which case the default extension
   logic from the single-file API is used). */
typedef struct {
	void*         in_src;
	int           in_type;
	unsigned int  in_size;
	void*         out_dest;
	int           out_type;
} pjglib_batch_io;

/* Convert N (in,out) pairs in parallel over pjglib_get_inter_file_threads()
   workers. Each worker calls pjglib_convert_stream2mem internally with the
   currently configured intra_file_threads (sfth auto/on/off).

   Returns true iff all N conversions succeeded. On the first failure, msg
   is filled with the failing worker's error message (truncated to PJG_MSG_SIZE
   bytes including the null terminator) and remaining workers are allowed
   to finish; their results are discarded. */
EXPORT bool pjglib_convert_batch( pjglib_batch_io* ops, int n_ops, char* msg );

/* a short reminder about input/output stream types
   for the pjglib_init_streams() function
	
	if input is file
	----------------
	in_scr -> name of input file
	in_type -> 0
	in_size -> ignore
	
	if input is memory
	------------------
	in_scr -> array containg data
	in_type -> 1
	in_size -> size of data array
	
	if input is *FILE (f.e. stdin)
	------------------------------
	in_src -> stream pointer
	in_type -> 2
	in_size -> ignore
	
	vice versa for output streams! */
