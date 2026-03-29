// packMP3dll.h - function import declarations for the packMP3 DLL
//
// Use this header in your application when linking against the packMP3
// shared library (.dll on Windows, .so on Linux).
//
// On Windows, link against the import library (packMP3lib.a or packMP3.lib).
// On Linux/macOS, link with -lpackMP3 and ensure the .so is on LD_LIBRARY_PATH.

#if defined(_WIN32) || defined(__CYGWIN__)
	#define IMPORT __declspec( dllimport )
#else
	#define IMPORT // shared lib symbols are visible by default on Linux/macOS
#endif

/* -----------------------------------------------
	function declarations: library only functions
	----------------------------------------------- */

IMPORT bool pmplib_convert_stream2stream( char* msg );
IMPORT bool pmplib_convert_file2file( char* in, char* out, char* msg );
IMPORT bool pmplib_convert_stream2mem( unsigned char** out_file, unsigned int* out_size, char* msg );
IMPORT void pmplib_init_streams( void* in_src, int in_type, int in_size, void* out_dest, int out_type );
IMPORT const char* pmplib_version_info( void );
IMPORT const char* pmplib_short_name( void );

/* a short reminder about input/output stream types
   for the pmplib_init_streams() function

	if input is file
	----------------
	in_src  -> name of input file (const char*)
	in_type -> 0
	in_size -> ignored

	if input is memory
	------------------
	in_src  -> pointer to buffer containing data
	in_type -> 1
	in_size -> size of data buffer in bytes

	if input is FILE* (e.g. stdin)
	-------------------------------
	in_src  -> FILE* stream pointer
	in_type -> 2
	in_size -> ignored

	vice versa for output streams */
