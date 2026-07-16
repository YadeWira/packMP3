#include <stdlib.h>
#include <string.h>
#include <ctime>
#include <cstdio>

// intra-file chunking (-k) needs threading/atomics in both CLI and lib builds.
#include <thread>
#include <atomic>

// v1.2 -th batch threading: process independent files concurrently (CLI only).
#if !defined(BUILD_LIB)
#  include <mutex>
#  include <chrono>
#  include <csignal>
#endif

// v1.2: std::filesystem for -r recursion / -fs folder-structure preservation.
#include <filesystem>
#include <vector>
#include <utility>
#include <system_error>

// v1.2 color support: needs isatty/fileno on POSIX and console-mode APIs on Windows.
#if defined(_WIN32) || defined(WIN32)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#include "pmp3tbl.h"
#include "pmp3bitlen.h"
#include "bitops.h"
#include "aricoder.h"
#include "huffmp3.h"
#include "huffmp3tbl.h"
#include "vendor/packmp2/packmp2.h"	// Layer I/II backend (sibling project, see packMP2)
#if !defined(BUILD_LIB) && !defined(BUILD_DLL)
// Embedded ID3v2 cover-art (APIC) recompression, see packJPG. CLI-only --
// same scope precedent as Layer I/II (packmp2 above): the library API
// never handles M2 archives either. Guarded on BUILD_DLL too, not just
// BUILD_LIB: this include sits *before* the "#if defined BUILD_DLL #define
// BUILD_LIB" cascade a few lines down, so BUILD_LIB isn't set yet here for
// the `so`/`dll-x64`/`dll-x86` targets (which pass -DBUILD_DLL, not
// -DBUILD_LIB, as their command-line flag) -- checking BUILD_LIB alone at
// this point would miss them. Also a hard requirement, not just a scope
// choice: packjpglib.h's EXPORT macro takes the __declspec(dllexport)
// branch whenever BUILD_DLL is defined, and __declspec is not recognized
// by native (non-mingw) g++, so `so` fails to compile outright without
// this guard -- confirmed empirically.
#include "vendor/packjpg/packjpglib.h"
#endif

#if defined BUILD_DLL // define BUILD_LIB from the compiler options if you want to compile a DLL!
	#define BUILD_LIB
#endif

#if defined BUILD_LIB // define BUILD_LIB from the compiler options if you want to compile a library!
	#include "packmp3lib.h"
#endif

// v1.2: on mingw the CRT does not expand command-line wildcards by default, so
// "packMP3 a *.mp3" was passed through literally on Windows. Force globbing on to
// match the documented wildcard support and Linux shell behaviour.
#if defined(__MINGW32__) && !defined(BUILD_LIB)
extern "C" { int _dowildcard = -1; }
#endif

#define INTERN static
// v1.2: per-file mutable state is thread-local so -th can process independent
// files concurrently without data races. Config/shared state stays plain static.
#define THREAD_LOCAL static thread_local

#define INIT_MODEL_S(a,b,c) new model_s( a, b, c, 8191 )
#define INIT_MODEL_B(a,b)   new model_b( a, b, 8191 )

#define ABS(v1)			( (v1 < 0) ? -v1 : v1 )
#define ABSDIFF(v1,v2)	( (v1 > v2) ? (v1 - v2) : (v2 - v1) )
#define ROUND_F(v1)		( (v1 < 0) ? (int) (v1 - 0.5) : (int) (v1 + 0.5) )
#define CLAMPED(l,h,v)	( ( v < l ) ? l : ( v > h ) ? h : v )

#define MEM_ERRMSG	"out of memory error"
#define FRD_ERRMSG	"could not read file / file not found: %s"
#define FWR_ERRMSG	"could not write file / file write-protected: %s"
#define MSG_SIZE	512
#define BARLEN		36

// special realloc with guaranteed free() of previous memory
static inline void* frealloc( void* ptr, size_t size ) {
	void* n_ptr = realloc( ptr, (size) ? size : 1 );
	if ( n_ptr == NULL ) free( ptr );
	return n_ptr;
}


	
/* -----------------------------------------------
	function declarations: main interface
	----------------------------------------------- */
#if !defined( BUILD_LIB )
INTERN void initialize_options( int argc, char** argv );
INTERN void process_ui( void );
INTERN inline const char* get_status( bool (*function)() );
INTERN void show_help( void );
#endif
INTERN void process_file( void );
INTERN void execute( bool (*function)() );


/* -----------------------------------------------
	function declarations: main functions
	----------------------------------------------- */
#if !defined( BUILD_LIB )
INTERN bool check_file( void );
INTERN bool swap_streams( void );
INTERN bool compare_output( void );
#endif
INTERN bool reset_buffers( void );
INTERN bool read_mp3( void );
INTERN bool write_mp3( void );
INTERN bool analyze_frames( void );
INTERN bool compress_mp3( void );
INTERN bool uncompress_pmp( void );
INTERN bool compress_mp3_chunked( void );	// split into K independent sub-streams, compress (parallel)
INTERN bool uncompress_pmp_chunked( void );	// decode an "MK" container, concatenate chunk output


/* -----------------------------------------------
	function declarations: MP3-specific
	----------------------------------------------- */

INTERN inline int mp3_ngr( int mpeg );
#if !defined(BUILD_LIB)
INTERN bool l2_compress( void );	// Layer I/II, backed by the packMP2 library
INTERN bool l2_decompress( void );
INTERN bool stats_l2( void );	// 'stats' on a raw Layer I/II input file
INTERN bool list_l2( void );	// 'list' on an F_PL2 ("M2") archive
#endif
INTERN inline int mp3_frame_bytes( int mpeg, int samples, int bits, int padding );
INTERN inline int mp3_lsf_scf_params( const mp3Frame* frame, const granuleInfo* granule, int ch, int scfsz[4], int scfcnt[4] );
INTERN inline mp3Frame* mp3_read_frame( unsigned char* data, int max_size );
INTERN inline mp3Frame* mp3_build_frame( void );
INTERN inline bool mp3_append_frame( mp3Frame* frame );
INTERN inline bool mp3_discard_frame( mp3Frame* frame );
INTERN inline bool mp3_mute_frame( mp3Frame* frame );
INTERN inline bool mp3_unmute_frame( mp3Frame* frame );
INTERN inline unsigned char* mp3_build_fixed( mp3Frame* frame );
INTERN inline int mp3_seek_firstframe( unsigned char* data, int size );
INTERN inline int mp3_get_id3_size( unsigned char* id3tag, int max_size );
INTERN inline unsigned short mp3_calc_layer3_crc( unsigned char* header, unsigned char* sideinfo, int sidesize );
#if !defined(BUILD_LIB) && defined(DEV_BUILD)
INTERN inline granuleData*** mp3_decode_frame( huffman_reader* dec, mp3Frame* frame );
#endif


/* -----------------------------------------------
	function declarations: PMP-specific
	----------------------------------------------- */
	
INTERN inline bool pmp_write_header( iostream* str );
INTERN inline bool pmp_read_header( iostream* str );
#if !defined(BUILD_LIB)
INTERN bool list_pmp( void );	// v1.2 'list' subcommand: PMP info, no decode
INTERN bool stats_mp3( void );	// v1.2 'stats' subcommand: MP3 info, no compress
#endif
#if !defined( STORE_ID3 )
INTERN inline bool pmp_encode_id3( aricoder* enc );
INTERN inline bool pmp_decode_id3( aricoder* dec );
#else
INTERN inline int pmp_store_data( iostream* str, unsigned char* data, int size );
INTERN inline int pmp_unstore_data( iostream* str, unsigned char** data );
#endif
#if !defined(BUILD_LIB)
// embedded ID3v2 cover-art (APIC) recompression via packJPG -- see the
// implementation comment above apic_try_recompress for the full design.
// encode: detect+recompress; on success returns a freshly allocated modified
// copy of data_before (caller frees) and sets apic_present/offset/lens.
INTERN bool apic_try_recompress( unsigned char** modified, int* modified_size );
// decode: if apic_present, splices the original JPEG back into data_before
// (replaces data_before/data_before_size with a freshly allocated buffer).
INTERN bool apic_reconstruct( void );
#endif
INTERN inline bool pmp_encode_padding( aricoder* enc );
INTERN inline bool pmp_decode_padding( aricoder* dec );
INTERN inline bool pmp_encode_block_types( aricoder* enc );
INTERN inline bool pmp_decode_block_types( aricoder* dec );
INTERN inline bool pmp_decode_stereo_ms( aricoder* dec );
INTERN inline bool pmp_encode_global_gain( aricoder* enc );
INTERN inline bool pmp_decode_global_gain( aricoder* dec );
INTERN inline bool pmp_encode_slength( aricoder* enc );
INTERN inline bool pmp_decode_slength( aricoder* dec );
INTERN inline bool pmp_encode_stereo_ms( aricoder* enc );
INTERN inline bool pmp_decode_stereo_ms( aricoder* dec );
INTERN inline bool pmp_encode_region_data( aricoder* enc );
INTERN inline bool pmp_decode_region_data( aricoder* dec );
INTERN inline bool pmp_encode_sharing( aricoder* enc );
INTERN inline bool pmp_decode_sharing( aricoder* dec );
INTERN inline bool pmp_encode_preemphasis( aricoder* enc );
INTERN inline bool pmp_decode_preemphasis( aricoder* dec );
INTERN inline bool pmp_encode_coarse_sf( aricoder* enc );
INTERN inline bool pmp_decode_coarse_sf( aricoder* dec );
INTERN inline bool pmp_encode_subblock_gain( aricoder* enc );
INTERN inline bool pmp_decode_subblock_gain( aricoder* dec );
INTERN inline bool pmp_encode_main_data( aricoder* enc );
INTERN inline bool pmp_decode_main_data( aricoder* dec );
INTERN inline bool pmp_store_unmute_data( iostream* str );
INTERN inline bool pmp_unstore_unmute_data( iostream* str );
INTERN inline bool pmp_build_context( void );
INTERN inline unsigned char* pmp_predict_lame_anc( int nbits, unsigned char* ref );


/* -----------------------------------------------
	function declarations: miscelaneous helpers
	----------------------------------------------- */
#if !defined( BUILD_LIB )
INTERN inline void progress_bar( int current, int last );
INTERN inline char* create_filename( const char* base, const char* extension );
INTERN inline char* unique_filename( const char* base, const char* extension );
INTERN inline void set_extension( char* filename, const char* extension );
INTERN inline void add_underscore( char* filename );
#endif
INTERN inline bool file_exists( const char* filename );


/* -----------------------------------------------
	function declarations: developers functions
	----------------------------------------------- */

// these are developers functions, they are not needed
// in any way to compress MP3 or decompress PMP
#if !defined(BUILD_LIB) && defined(DEV_BUILD)
INTERN bool write_file( const char* base, const char* ext, void* data, int bpv, int size );
INTERN bool write_errfile( void );
INTERN bool write_file_analysis( void );
INTERN bool write_block_analysis( void );
INTERN bool write_stat_analysis( void );
INTERN bool visualize_headers( void );
INTERN bool visualize_decoded_data( void );
INTERN bool dump_main_sizes( void );
INTERN bool dump_aux_sizes( void );
INTERN bool dump_bitrates( void );
INTERN bool dump_stereo_ms( void );
INTERN bool dump_padding( void );
INTERN bool dump_main_data_bits( void );
INTERN bool dump_big_value_ns( void );
INTERN bool dump_global_gain( void );
INTERN bool dump_slength( void );
INTERN bool dump_block_types( void );
INTERN bool dump_sharing( void );
INTERN bool dump_preemphasis( void );
INTERN bool dump_coarse( void );
INTERN bool dump_htable_sel( void );
INTERN bool dump_region_sizes( void );
INTERN bool dump_subblock_gains( void );
INTERN bool dump_data_files( void );
INTERN bool dump_gg_ctx( void );
INTERN bool dump_decoded_data( void );
#endif


/* -----------------------------------------------
	global variables: library only variables
	----------------------------------------------- */
#if defined(BUILD_LIB)
INTERN int lib_in_type  = -1;
INTERN int lib_out_type = -1;
#endif


/* -----------------------------------------------
	global variables: data storage
	----------------------------------------------- */

THREAD_LOCAL mp3Frame*      firstframe		=	NULL;	// first physical frame
THREAD_LOCAL mp3Frame*      lastframe			=	NULL;	// last physical frame
THREAD_LOCAL unsigned char* main_data			=	NULL;	// (mainly) huffman coded data
THREAD_LOCAL unsigned char* data_before		=	NULL;	// data before (should be ID3v2 tag)
THREAD_LOCAL unsigned char* data_after		=	NULL;	// data after (should be ID3v1 or ID3v2 tag)
THREAD_LOCAL unsigned char* unmute_data		=	NULL;	// fix data (to reverse muted frames)
THREAD_LOCAL int            main_data_size	=     0 ;	// size of main data
THREAD_LOCAL int            data_before_size	=     0 ;	// size of data before
THREAD_LOCAL int            data_after_size	=     0 ;   // size of data after
THREAD_LOCAL int            unmute_data_size	=     0 ;   // size of fix data
THREAD_LOCAL int            n_bad_first		=     0 ;   // # of bad first frames (should be zero!)
THREAD_LOCAL unsigned char* gg_context[2]	= {NULL};	// universal context based on global gain

/* -----------------------------------------------
	global variables: info about audio file
	----------------------------------------------- */

THREAD_LOCAL int  g_nframes     =   0;  // number of frames
THREAD_LOCAL int  g_nchannels   =   0;  // number of channels
THREAD_LOCAL int  g_samplerate  =   0;  // sample rate
THREAD_LOCAL int  g_bitrate     =   0;  // bit rate - global or zero for vbr


/* -----------------------------------------------
	global variables: frame analysis info
	----------------------------------------------- */

THREAD_LOCAL char i_mpeg			= -1; // mpeg - non changing
THREAD_LOCAL char i_layer			= -1; // layer - non changing
THREAD_LOCAL int  pmp_archive_version = 0; // version byte of the PMP being decoded (for format-evolution gating)
THREAD_LOCAL char i_samplerate	= -1; // sample rate - non changing
THREAD_LOCAL char i_bitrate		= -1; // bit rate - value or -1 (variable)
THREAD_LOCAL char i_protection	= -1; // checksum - for all (1), none (0) or some (-1) frames
THREAD_LOCAL char i_padding		= -1; // padding - for all (1), none (0) or some (-1) frames
THREAD_LOCAL char i_privbit		= -1; // private bit - value or -1 (variable)
THREAD_LOCAL char i_channels		= -1; // channel mode - non changing
THREAD_LOCAL char i_stereo_ms		= -1; // ms stereo - for all (1), none (0) or some (-1) frames
THREAD_LOCAL char i_stereo_int	= -1; // int stereo - for all (1), none (0) or some (-1) frames
THREAD_LOCAL char i_copyright		= -1; // copyright bit - value or -1 (variable)
THREAD_LOCAL char i_original		= -1; // original bit - value or -1 (variable)
THREAD_LOCAL char i_emphasis		= -1; // emphasis - value or -1 (variable)
THREAD_LOCAL char i_padbits		= -1; // side info padding bits - value or -1 (variable)
THREAD_LOCAL char i_bit_res		= -1; // bit reservoir - is used (1) or not used (0)
THREAD_LOCAL char i_share			= -1; // scalefactor sharing - is used (1) or not used (0)
THREAD_LOCAL char i_sblocks		= -1; // special blocks - are used (1) or not used (0)
THREAD_LOCAL char i_mixed			= -1; // mixed blocks - are used (1) or not used (0)
THREAD_LOCAL char i_preemphasis	= -1; // preemphasis - value or -1 (variable)
THREAD_LOCAL char i_coarse		= -1; // coarse scalefactors - value or -1 (variable)
THREAD_LOCAL char i_sbgain		= -1; // subblock gain - used properly (1), not used (0) or used for non-short (-1)
THREAD_LOCAL char i_aux_h			= -1; // auxiliary data handling - none (0), at begin and end (1), between frames (-1)
THREAD_LOCAL char i_sb_diff		= -1; // special blocks diffs between ch0 and ch1 - none (0) or some (-1)
	

/* -----------------------------------------------
	global variables: info about files
	----------------------------------------------- */
	
THREAD_LOCAL char*  mp3filename = NULL;	// name of MP3 file
THREAD_LOCAL char*  pmpfilename = NULL;	// name of PMP file
THREAD_LOCAL int    mp3filesize;			// size of MP3 file
THREAD_LOCAL int    pmpfilesize;			// size of PMP file
THREAD_LOCAL int    filetype;				// type of current file
THREAD_LOCAL iostream* str_in  = NULL;	// input stream
THREAD_LOCAL iostream* str_out = NULL;	// output stream

#if !defined(BUILD_LIB)
THREAD_LOCAL iostream* str_str = NULL;	// storage stream

INTERN char** filelist = NULL;		// list of files to process (shared, read-only during run)
INTERN int    file_cnt = 0;			// count of files in list (shared)
THREAD_LOCAL int file_no = 0;		// number of current file (per-thread index into filelist)

INTERN char** err_list = NULL;		// list of error messages (shared; each thread writes a unique slot)
INTERN int*   err_tp   = NULL;		// list of error types (shared; unique slot per file)
#endif


/* -----------------------------------------------
	global variables: messages
	----------------------------------------------- */

THREAD_LOCAL char errormessage [ MSG_SIZE ];
THREAD_LOCAL bool (*errorfunction)();
THREAD_LOCAL int  errorlevel;
// meaning of errorlevel:
// -1 -> wrong input
// 0 -> no error
// 1 -> warning
// 2 -> fatal error


/* -----------------------------------------------
	global variables: settings
	----------------------------------------------- */

INTERN bool compress_only   = false;	// 'a' subcommand: only compress MP3 files, skip PMP silently
INTERN bool decompress_only = false;	// 'x' subcommand: only decompress PMP files, skip MP3 silently
INTERN int  num_threads = 1;		// -th<N> (CLI) / always 1 in lib builds (no batch loop there)
INTERN int  num_chunks  = 1;		// -k<N> (CLI) / intra-file parallel chunks; 1 = single serial stream, best ratio
INTERN int  verify_lv  = 0;			// verification level (0=none/1=simple/2=detailed); always 0 in lib builds (not yet exposed)

#if !defined( BUILD_LIB )
INTERN bool mix_mode        = false;	// 'mix' subcommand: auto-detect (warns if both directions used)
INTERN bool subcmd_given    = false;	// a subcommand was explicitly provided
INTERN int  verbosity  = 0;			// level of verbosity (0 default; -1 progress bar via -vp)
INTERN bool overwrite  = false;		// overwrite files yes / no
INTERN bool wait_exit  = true;		// pause after finished yes / no
INTERN int  err_tol    = 1;			// error threshold ( proceed on warnings yes (2) / no (1) )

INTERN bool developer      = false;		// allow developers functions yes/no
INTERN bool disc_meta      = false;		// -d: discard ID3/meta tags
INTERN bool recursive      = false;		// -r: recurse into subdirectories
INTERN bool fs_mode        = false;		// -fs: preserve source folder structure under -od when -r expands a dir
INTERN bool dry_run        = false;		// -dry: simulate without writing output files
INTERN bool module_mode    = false;		// -module: machine-friendly output (OK/ERROR + time only)
INTERN bool force_no_color = false;		// --no-color
INTERN char* outdir        = NULL;		// -od<DIR>: write output files to this directory
INTERN char** filelist_srcroot = NULL;	// [i] = src dir arg that yielded filelist[i] via -r (for -fs); NULL otherwise
INTERN int  action         = A_COMPRESS;// what to do with MP3/PMP files
INTERN FILE*  msgout   = stdout;	// stream for output of messages
THREAD_LOCAL bool pipe_on  = false;	// use stdin/stdout instead of filelist (per-thread)
#else
INTERN int  err_tol    = 1;			// error threshold ( proceed on warnings yes (2) / no (1) )
INTERN int  action     = A_COMPRESS;// what to do with MP3/PMP files
#endif



/* -----------------------------------------------
	global variables: info about program
	----------------------------------------------- */

INTERN const unsigned char appversion = 30;
// appversion_legacy_min stays at 20 (unchanged since the v2.0 entropy-model
// break) -- v3.0's own additions (packMP2 Layer II backend, packJPG APIC
// recompression) are purely additive/gated on pmp_archive_version, same
// precedent as the v1.3 MPEG-version-bits gate, so v2.0 archives still
// decode unaffected. The major-number jump here is a product-milestone
// label (bundles a full release's worth of features), not a claim that the
// wire format broke -- unlike 1.x->2.0, which genuinely did and moved the
// floor with it.
INTERN const unsigned char appversion_legacy_min = 20;
INTERN const char*  subversion   = "";
INTERN const char*  apptitle     = "packMP3";
INTERN const char*  appname      = "packMP3";
INTERN const char*  versiondate  = "07/16/2026";
INTERN const char*  author       = "Yade Bravo";
#if !defined( BUILD_LIB )
INTERN const char*  website      = "https://github.com/YadeWira/packMP3";
INTERN const char*	copyright    = "2010-2026 Yade Bravo & Matthias Stirner";
INTERN const char*  pmp_ext      = "pm3";
INTERN const char*  mp3_ext      = "mp3";
#endif
INTERN const char   pmp_magic[] = { 'M', 'S' };
INTERN const char   l2_magic[]  = { 'M', '2' };	// separate container for Layer I/II archives
#if !defined(BUILD_LIB)
// packMP2's unpack/pack engine uses shared global buffers internally (not
// thread_local, unlike everything above) -- concurrent calls race per its
// own documented limitation. -th runs multiple files' process_file() on
// separate threads, so any packmp2_compress/_decompress/_query_original_size
// call must be serialized process-wide until packMP2 ships a thread-safe
// (per-call heap-alloc) engine.
INTERN std::mutex l2_pmp2_mutex;

// Embedded ID3v2 cover-art (APIC) recompression via packJPG. packJPG's own
// thread-safety for concurrent pjglib_init_streams/pjglib_convert_stream2mem
// pairs (independent calls, different threads) is unverified -- its API
// shape (no handle threaded between the two calls) means state lives
// somewhere outside caller control, and static inspection of the vendored
// lib couldn't prove it either way. Unlike a wrong-output bug, a race here
// could mean memory corruption inside pjglib that a round-trip check can't
// catch after the fact -- serialize unconditionally rather than assume.
INTERN std::mutex pjg_mutex;
#endif

// Recipe for splicing a recompressed cover back into data_before on decode
// (see pmp_write_header/pmp_read_header and apic_try_recompress/
// apic_reconstruct further below). Declared unconditionally (pmp_write_header/
// pmp_read_header read/set apic_present in every build) but only ever set
// true by apic_try_recompress/apic_reconstruct, which are CLI-only -- lib
// builds simply never produce or consume APIC-recompressed archives, same
// scope precedent as Layer I/II.
THREAD_LOCAL bool apic_present  = false;
THREAD_LOCAL int  apic_offset   = 0;	// byte offset within data_before of the blob
THREAD_LOCAL int  apic_orig_len = 0;	// original JPEG length
THREAD_LOCAL int  apic_pjg_len  = 0;	// stored (packJPG-compressed) length
INTERN const char   pmc_magic[] = { 'M', 'K' };	// chunked container: K independent "MS" sub-streams
#define MAX_CHUNKS       64				// upper bound on -k
#define MIN_FRAMES_CHUNK 16				// don't split below this many frames per chunk
THREAD_LOCAL bool arch_chunked = false;	// set by check_file when input archive is an "MK" container


// ─── v1.2 Color support ──────────────────────────────────────────────────────
// ANSI escape codes — empty strings when colors are disabled.
// Call init_colors() once at startup (no-op in library builds).
#if !defined(BUILD_LIB)
static bool use_color = false;

#define COL_RESET   (use_color ? "\033[0m"   : "")
#define COL_CYAN    (use_color ? "\033[36m"  : "")
#define COL_GRAY    (use_color ? "\033[90m"  : "")
#define COL_BRED    (use_color ? "\033[1;31m": "")
#define COL_BGREEN  (use_color ? "\033[1;32m": "")
#define COL_BYELLOW (use_color ? "\033[1;33m": "")
#define COL_BCYAN   (use_color ? "\033[1;36m": "")

static void init_colors( void )
{
#if defined(_WIN32) || defined(WIN32)
	// Switch console to UTF-8 so multi-byte characters render correctly
	// regardless of --no-color. No-op when output isn't a console.
	SetConsoleOutputCP( CP_UTF8 );
	SetConsoleCP( CP_UTF8 );
	if ( force_no_color ) return;
	HANDLE h = GetStdHandle( STD_OUTPUT_HANDLE );
	if ( h == INVALID_HANDLE_VALUE ) return;
	DWORD mode = 0;
	if ( !GetConsoleMode( h, &mode ) ) return;
	// ENABLE_VIRTUAL_TERMINAL_PROCESSING (0x0004) — Windows 10+.
	if ( SetConsoleMode( h, mode | 0x0004 ) )
		use_color = true;
#else
	if ( force_no_color ) return;
	// Enable colors only if stdout is a real terminal and NO_COLOR is not set.
	if ( isatty( fileno( stdout ) ) && getenv( "NO_COLOR" ) == NULL )
		use_color = true;
#endif
}
#endif


/* -----------------------------------------------
	main-function
	----------------------------------------------- */

#if !defined(BUILD_LIB)
int main( int argc, char** argv )
{	
	snprintf( errormessage, MSG_SIZE, "no errormessage specified" );

	// wall-clock for the overall run: clock() sums CPU across threads, so it
	// overstates time in -th mode. Use steady_clock for the summary total.
	std::chrono::steady_clock::time_point wall_begin, wall_end;

	int error_cnt = 0;
	int warn_cnt  = 0;
	int acc_mp3_cnt = 0;	// files compressed (MP3 -> PMP)
	int acc_pmp_cnt = 0;	// files decompressed (PMP -> MP3)

	double acc_mp3size = 0;
	double acc_pmpsize = 0;
	
	int kbps;
	double cr;
	double total;
	
	errorlevel = 0;
	
	
	// read options from command line
	initialize_options( argc, argv );

	// v1.2: enable ANSI colors if appropriate (after parsing so --no-color
	// and -module are honoured). module_mode disables colors automatically
	// to keep machine-friendly output clean.
	if ( module_mode ) force_no_color = true;
	init_colors();

	// write program info to screen
	if ( !module_mode ) {
		fprintf( msgout,  "\n%s--> %s v%i.%i%s (%s) by %s <--%s\n",
				COL_BCYAN, apptitle, appversion / 10, appversion % 10,
				subversion, versiondate, author, COL_RESET );
		fprintf( msgout, "Copyright %s\nAll rights reserved\n\n", copyright );
	}

	// check if user input is wrong, show help screen if it is.
	// v1.2: require a subcommand (a/x/mix/list/stats) unless piped or running
	// in developer/dev-build mode.
	if ( ( file_cnt == 0 ) ||
		( ( !developer ) && ( !subcmd_given ) && ( !pipe_on ) ) ||
		( ( !developer ) && (
		    (action != A_COMPRESS && action != A_LIST && action != A_STATS) ||
		    (verify_lv > 1) ) ) ) {
		show_help();
		return -1;
	}
	
	// (re)set program has to be done first
	reset_buffers();
	
	// process file(s) - this is the main function routine
	wall_begin = std::chrono::steady_clock::now();

	// tally one processed file's result into the accumulators. Called from the
	// single-threaded loop directly, and from each worker under a lock in -th.
	auto tally = [&]( int fn ) {
		if ( errorlevel > 0 ) {
			err_list[ fn ] = (char*) calloc( MSG_SIZE, sizeof( char ) );
			err_tp[ fn ] = errorlevel;
			if ( err_list[ fn ] != NULL )
				strcpy( err_list[ fn ], errormessage );
		}
		if ( errorlevel >= err_tol ) error_cnt++;
		else {
			if ( errorlevel == 1 ) warn_cnt++;
			acc_mp3size += mp3filesize;
			acc_pmpsize += pmpfilesize;
			if ( action == A_COMPRESS ) {
				if ( filetype == F_MP3 )      acc_mp3_cnt++;
				else if ( filetype == F_PMP ) acc_pmp_cnt++;
			}
		}
	};

	if ( num_threads <= 1 ) {
		// --- single-threaded (original behaviour) ---
		for ( file_no = 0; file_no < file_cnt; file_no++ ) {
			process_ui();
			tally( file_no );
		}
	} else {
		// --- multi-threaded batch (-th) ---
		// Each file is fully independent; per-file state is THREAD_LOCAL.
		// Force verification: a missed thread_local would corrupt output, and
		// verify catches it (compress→decompress→compare) before it's written.
		if ( verify_lv < 1 ) verify_lv = 1;
		if ( num_threads > file_cnt ) num_threads = ( file_cnt > 0 ) ? file_cnt : 1;
		if ( !module_mode )
			fprintf( msgout, "Using %i thread(s), verify enabled\n\n", num_threads );

		std::atomic<int> next_file( 0 );
		std::mutex mtx;
		int done = 0, spin = 0;

		auto worker = [&]() {
			int fn;
			while ( ( fn = next_file.fetch_add( 1 ) ) < file_cnt ) {
				file_no = fn;          // THREAD_LOCAL: this thread's current file
				process_ui();
				std::lock_guard<std::mutex> lk( mtx );
				tally( fn );
				done++;
				if ( !module_mode ) {
					const char* sp = "|/-\\";
					fprintf( msgout, "\r  %c  %i / %i processed", sp[ spin++ & 3 ], done, file_cnt );
					fflush( msgout );
				}
			}
		};

		std::vector<std::thread> pool;
		pool.reserve( num_threads );
		for ( int i = 0; i < num_threads; i++ ) pool.emplace_back( worker );
		for ( auto& t : pool ) t.join();
		if ( !module_mode ) fprintf( msgout, "\r%*s\r", 32, "" ); // clear bar line
	}

	wall_end = std::chrono::steady_clock::now();

	// module mode: single machine-friendly line, nothing else
	if ( module_mode ) {
		total = std::chrono::duration<double>( wall_end - wall_begin ).count();
		if ( error_cnt == 0 ) fprintf( msgout, "OK %.2f\n", total );
		else                  fprintf( msgout, "ERROR %i %.2f\n", error_cnt, total );
		return ( error_cnt == 0 ) ? 0 : -1;
	}

	// errors summary: only needed for -v2 or progress bar (inline per-file at v0/v1)
	if ( ( verbosity == -1 ) || ( verbosity == 2 ) ) {
		// print summary of errors to screen
		if ( error_cnt > 0 ) {
			fprintf( stderr, "\n\n%sfiles with errors:%s\n", COL_BRED, COL_RESET );
			fprintf( stderr, "------------------\n" );
			for ( file_no = 0; file_no < file_cnt; file_no++ ) {
				if ( err_tp[ file_no ] >= err_tol ) {
					fprintf( stderr, "%s%s%s (%s)\n", COL_BRED, filelist[ file_no ], COL_RESET, err_list[ file_no ] );
				}
			}
		}
		// print summary of warnings to screen
		if ( warn_cnt > 0 ) {
			fprintf( stderr, "\n\n%sfiles with warnings:%s\n", COL_BYELLOW, COL_RESET );
			fprintf( stderr, "------------------\n" );
			for ( file_no = 0; file_no < file_cnt; file_no++ ) {
				if ( err_tp[ file_no ] == 1 ) {
					fprintf( stderr, "%s%s%s (%s)\n", COL_BYELLOW, filelist[ file_no ], COL_RESET, err_list[ file_no ] );
				}
			}
		}
	}

	// mixed-mode warning: both directions happened in one run
	if ( mix_mode && acc_mp3_cnt > 0 && acc_pmp_cnt > 0 && verbosity >= 0 ) {
		fprintf( msgout, "\n%s[WARNING]%s Mixed mode: compressed %i MP3 and decompressed %i .pm3 files.\n",
			COL_BYELLOW, COL_RESET, acc_mp3_cnt, acc_pmp_cnt );
		fprintf( msgout, "  Running 'mix' on already-processed files can undo previous work.\n" );
		fprintf( msgout, "  Use 'a' (compress only) or 'x' (decompress only) for safer operation.\n" );
	}

	// show statistics
	fprintf( msgout,  "\n%i file(s)  %i ok  %i error(s)  %i warning(s)\n",
		file_cnt, file_cnt - error_cnt, error_cnt, warn_cnt );
	if ( acc_mp3_cnt > 0 || acc_pmp_cnt > 0 ) {
		fprintf( msgout, " " );
		bool prev = false;
		if ( acc_mp3_cnt > 0 ) { fprintf( msgout, "compressed: %i MP3", acc_mp3_cnt ); prev = true; }
		if ( acc_pmp_cnt > 0 ) { if ( prev ) fprintf( msgout, "  " ); fprintf( msgout, "decompressed: %i PMP", acc_pmp_cnt ); }
		fprintf( msgout, "\n" );
	}
	if ( ( file_cnt > error_cnt ) && ( verbosity != 0 ) && ( acc_mp3size > 0 || acc_pmpsize > 0 ) ) {
		acc_mp3size /= 1024.0; acc_pmpsize /= 1024.0;
		total = std::chrono::duration<double>( wall_end - wall_begin ).count();
		kbps  = ( total > 0 ) ? ( acc_mp3size / total ) : acc_mp3size;
		cr    = ( acc_mp3size > 0 ) ? ( 100.0 * acc_pmpsize / acc_mp3size ) : 0;

		fprintf( msgout,  "%s --------------------------------- %s\n", COL_GRAY, COL_RESET );
		if ( total >= 0 ) {
			fprintf( msgout,  " time    %8.2f sec\n", total );
			fprintf( msgout,  " speed   %8i KB/s\n", kbps );
		}
		else {
			fprintf( msgout,  " time    %8s sec\n", "N/A" );
			fprintf( msgout,  " speed   %8s KB/s\n", "N/A" );
		}
		fprintf( msgout,  " ratio   %8.2f %%\n", cr );
		fprintf( msgout,  "%s --------------------------------- %s\n", COL_GRAY, COL_RESET );
	}

	// pause before exit
	if ( wait_exit && ( msgout != stderr ) ) {
		fprintf( msgout, "\n\n< press ENTER >\n" );
		fgetc( stdin );
	}
	
	
	return 0;
}
#endif

/* ----------------------- Begin of library only functions -------------------------- */

/* -----------------------------------------------
	DLL export converter function
	----------------------------------------------- */
	
#if defined(BUILD_LIB)
EXPORT bool pmplib_convert_stream2stream( char* msg )
{
	// process in main function
	return pmplib_convert_stream2mem( NULL, NULL, msg ); 
}
#endif


/* -----------------------------------------------
	DLL export converter function
	----------------------------------------------- */

#if defined(BUILD_LIB)
EXPORT bool pmplib_convert_file2file( char* in, char* out, char* msg )
{
	// init streams
	pmplib_init_streams( (void*) in, 0, 0, (void*) out, 0 );
	
	// process in main function
	return pmplib_convert_stream2mem( NULL, NULL, msg ); 
}
#endif


/* -----------------------------------------------
	DLL export converter function
	----------------------------------------------- */
	
#if defined(BUILD_LIB)
EXPORT bool pmplib_convert_stream2mem( unsigned char** out_file, unsigned int* out_size, char* msg )
{
	clock_t begin, end;
	int total;
	float cr;	
	
	
	// (re)set buffers
	reset_buffers();
	action = A_COMPRESS;
	
	// main compression / decompression routines
	begin = clock();
	
	// process one file
	process_file();
	
	// fetch pointer and size of output (only for memory output)
	if ( ( errorlevel < err_tol ) && ( lib_out_type == 1 ) &&
		 ( out_file != NULL ) && ( out_size != NULL ) ) {
		*out_size = str_out->getsize();
		*out_file = str_out->getptr();
	}
	
	// close iostreams
	if ( str_in  != NULL ) { delete( str_in  ); str_in  = NULL; }
	if ( str_out != NULL ) { delete( str_out ); str_out = NULL; }
	
	end = clock();
	
	// copy errormessage / remove files if error (and output is file)
	if ( errorlevel >= err_tol ) {
		if ( lib_out_type == 0 ) {
			if ( filetype == F_MP3 ) {
				if ( file_exists( pmpfilename ) ) remove( pmpfilename );
			} else if ( filetype == F_PMP ) {
				if ( file_exists( mp3filename ) ) remove( mp3filename );
			}
		}
		if ( msg != NULL ) strcpy( msg, errormessage );
		return false;
	}
	
	// get compression info
	total = (int) ( (double) (( end - begin ) * 1000) / CLOCKS_PER_SEC );
	cr    = ( mp3filesize > 0 ) ? ( 100.0 * pmpfilesize / mp3filesize ) : 0;
	
	// write success message else
	if ( msg != NULL ) {
		switch( filetype )
		{
			case F_MP3:
				snprintf( msg, MSG_SIZE, "Compressed to %s (%.2f%%) in %ims",
					pmpfilename, cr, ( total >= 0 ) ? total : -1 );
				break;
			case F_PMP:
				snprintf( msg, MSG_SIZE, "Decompressed to %s (%.2f%%) in %ims",
					mp3filename, cr, ( total >= 0 ) ? total : -1 );
				break;
			case F_UNK:
				snprintf( msg, MSG_SIZE, "Unknown filetype" );
				break;	
		}
	}
	
	
	return true;
}
#endif


/* -----------------------------------------------
	DLL export init input (file/mem)
	----------------------------------------------- */
	
#if defined(BUILD_LIB)
EXPORT void pmplib_init_streams( void* in_src, int in_type, int in_size, void* out_dest, int out_type )
{
	/* a short reminder about input/output stream types:
	
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
	
	unsigned char buffer[ 2 ];
	
	
	// (re)set errorlevel
	errorfunction = NULL;
	errorlevel = 0;
	mp3filesize = 0;
	pmpfilesize = 0;
	
	// open input stream, check for errors
	str_in = new iostream( in_src, in_type, in_size, 0 );
	if ( str_in->chkerr() ) {
		snprintf( errormessage, MSG_SIZE, "error opening input stream" );
		errorlevel = 2;
		return;
	}	
	
	// open output stream, check for errors
	str_out = new iostream( out_dest, out_type, 0, 1 );
	if ( str_out->chkerr() ) {
		snprintf( errormessage, MSG_SIZE, "error opening output stream" );
		errorlevel = 2;
		return;
	}
	
	// free memory from filenames if needed
	if ( mp3filename != NULL ) { free( mp3filename ); mp3filename = NULL; }
	if ( pmpfilename != NULL ) { free( pmpfilename ); pmpfilename = NULL; }
	
	// check input stream
	str_in->read( buffer, 1, 2 );
	str_in->rewind();
	if ( (buffer[0] == pmp_magic[0]) && (buffer[1] == pmp_magic[1]) ) {
		// file is PMP
		filetype = F_PMP;
		// skip silently if -c (compress only)
		if ( compress_only ) { return; }
		// copy filenames
		const char* _in_name  = ( in_type  == 0 ) ? static_cast<const char*>( in_src )  : "PMP in memory";
		const char* _out_name = ( out_type == 0 ) ? static_cast<const char*>( out_dest ) : "MP3 in memory";
		pmpfilename = (char*) calloc( strlen( _in_name  ) + 1, sizeof( char ) );
		mp3filename = (char*) calloc( strlen( _out_name ) + 1, sizeof( char ) );
		strcpy( pmpfilename, _in_name  );
		strcpy( mp3filename, _out_name );
	} else {
		// file is MP3
		filetype = F_MP3;
		// skip silently if -x (decompress only)
		if ( decompress_only ) { return; }
		// copy filenames
		const char* _in_name  = ( in_type  == 0 ) ? static_cast<const char*>( in_src )  : "MP3 in memory";
		const char* _out_name = ( out_type == 0 ) ? static_cast<const char*>( out_dest ) : "PMP in memory";
		mp3filename = (char*) calloc( strlen( _in_name  ) + 1, sizeof( char ) );
		pmpfilename = (char*) calloc( strlen( _out_name ) + 1, sizeof( char ) );
		strcpy( mp3filename, _in_name  );
		strcpy( pmpfilename, _out_name );
	}
	
	// store types of in-/output
	lib_in_type  = in_type;
	lib_out_type = out_type;
}
#endif


/* -----------------------------------------------
	DLL export version information
	----------------------------------------------- */
	
#if defined(BUILD_LIB)
EXPORT const char* pmplib_version_info( void )
{
	static char v_info[ 256 ];
	
	// copy version info to string
	snprintf( v_info, 256, "--> %s library v%i.%i%s (%s) by %s <--",
			apptitle, appversion / 10, appversion % 10, subversion, versiondate, author );
			
	return (const char*) v_info;
}
#endif


/* -----------------------------------------------
	DLL export version information
	----------------------------------------------- */
	
#if defined(BUILD_LIB)
EXPORT const char* pmplib_short_name( void )
{
	static char v_name[ 256 ];
	
	// copy version info to string
	snprintf( v_name, 256, "%s v%i.%i%s",
			apptitle, appversion / 10, appversion % 10, subversion );
			
	return (const char*) v_name;
}
#endif

/* ----------------------- End of libary only functions -------------------------- */

/* ----------------------- Begin of main interface functions -------------------------- */


/* -----------------------------------------------
	v1.2 helpers for -r recursion + -fs folder-structure preservation
	----------------------------------------------- */

#if !defined(BUILD_LIB)
static bool is_mp3_or_pmp( const std::filesystem::path& p )
{
	std::string ext = p.extension().string();
	for ( auto& ch : ext ) ch = (char)tolower( (unsigned char)ch );
	return ext == ".mp3" || ext == ".pm3" || ext == ".pmp";
}

static void collect_files_recursive( const std::filesystem::path& dir,
                                     std::vector<std::pair<std::string,std::string>>& out )
{
	std::error_code ec;
	std::string src_root = dir.string();
	for ( auto& entry : std::filesystem::recursive_directory_iterator( dir,
	        std::filesystem::directory_options::skip_permission_denied, ec ) ) {
		if ( entry.is_regular_file( ec ) && is_mp3_or_pmp( entry.path() ) )
			out.push_back( { entry.path().string(), src_root } );
	}
}
#endif


/* -----------------------------------------------
	reads in commandline arguments
	----------------------------------------------- */

#if !defined(BUILD_LIB)
INTERN void initialize_options( int argc, char** argv )
{
	int tmp_val;
	char** tmp_flp;
	int i;


	// v1.2: get memory for filelist with generous capacity. Wildcard
	// expansion on Windows or recursive -r can yield many more files than
	// argc. 65536 entries covers any realistic batch.
	const int FILELIST_MAX = 65536;
	filelist = (char**) calloc( FILELIST_MAX, sizeof( char* ) );
	filelist_srcroot = (char**) calloc( FILELIST_MAX, sizeof( char* ) );
	for ( i = 0; i < FILELIST_MAX; i++ ) {
		filelist[ i ] = NULL;
		filelist_srcroot[ i ] = NULL;
	}

	// preset temporary filelist pointer
	tmp_flp = filelist;


	// v1.2: pipe mode bypasses subcommand requirement
	for ( int pi = 1; pi < argc; pi++ ) {
		if ( strcmp(argv[pi], "-") == 0 ) { subcmd_given = true; break; }
	}

	// v1.2: first argument can be a subcommand. Mirrors packJPG v4.0d:
	//   a    -> compress only (MP3 -> PMP, skip PMP)
	//   x    -> decompress only (PMP -> MP3, skip MP3)
	//   mix  -> auto-detect (warns if both directions used)
	//   list -> list PMP file info without decompressing (PMP only)
	//   stats-> show MP3 file info without compressing (MP3 only)
	if ( argc > 1 ) {
		const char* subcmd = argv[1];
		if ( strcmp(subcmd, "a") == 0 ) {
			compress_only = true;
			subcmd_given = true;
			argv++; argc--;
		} else if ( strcmp(subcmd, "x") == 0 ) {
			decompress_only = true;
			subcmd_given = true;
			argv++; argc--;
		} else if ( strcmp(subcmd, "mix") == 0 ) {
			mix_mode = true;
			subcmd_given = true;
			argv++; argc--;
		} else if ( strcmp(subcmd, "list") == 0 ) {
			action = A_LIST;
			decompress_only = true; // list only inspects PMP files; skip MP3
			subcmd_given = true;
			argv++; argc--;
		} else if ( strcmp(subcmd, "stats") == 0 ) {
			action = A_STATS;
			compress_only = true; // stats only inspects MP3 files
			subcmd_given = true;
			argv++; argc--;
		}
	}

	// read in arguments
	while ( --argc > 0 ) {
		argv++;
		// switches begin with '-'
		if ( strcmp((*argv), "-p" ) == 0 ) {
			err_tol = 2;
		}
		else if ( strcmp((*argv), "-d" ) == 0 ) {
			disc_meta = true;
		}
		else if ( strcmp((*argv), "-ver" ) == 0 ) {
			verify_lv = ( verify_lv < 1 ) ? 1 : verify_lv;
		}
		else if ( sscanf( (*argv), "-v%i", &tmp_val ) == 1 ){
			verbosity = tmp_val;
			verbosity = ( verbosity < 0 ) ? 0 : verbosity;
			verbosity = ( verbosity > 2 ) ? 2 : verbosity;
		}
		else if ( strcmp((*argv), "-vp" ) == 0 ) {
			verbosity = -1;
		}
		else if ( strcmp((*argv), "-np" ) == 0 ) {
			wait_exit = false;
		}
		else if ( strcmp((*argv), "-o" ) == 0 ) {
			overwrite = true;
		}
		else if ( strcmp((*argv), "--no-color" ) == 0 ) {
			force_no_color = true;
		}
		else if ( strcmp((*argv), "-r" ) == 0 ) {
			recursive = true;
		}
		else if ( strcmp((*argv), "-fs" ) == 0 ) {
			fs_mode = true;
		}
		else if ( strcmp((*argv), "-dry" ) == 0 ) {
			dry_run = true;
		}
		else if ( strcmp((*argv), "-module" ) == 0 ) {
			module_mode = true;
			wait_exit = false;
			verbosity = 0; // suppress all output except final OK/ERROR line
		}
		else if ( strncmp((*argv), "-od", 3 ) == 0 && (*argv)[3] != '\0' ) {
			outdir = (*argv) + 3; // -odPATH
		}
		else if ( strncmp((*argv), "-th", 3 ) == 0 ) {
			// -th<N> batch threads; -th or -th0 => auto (hardware concurrency)
			int cores = (int) std::thread::hardware_concurrency();
			if ( (*argv)[3] == '\0' ) {
				num_threads = ( cores > 1 ) ? cores : 1;
			} else if ( sscanf( (*argv) + 3, "%i", &tmp_val ) == 1 ) {
				num_threads = ( tmp_val == 0 ) ? ( ( cores > 1 ) ? cores : 1 )
				            : ( ( tmp_val < 1 ) ? 1 : tmp_val );
			}
		}
		else if ( strncmp((*argv), "-k", 2 ) == 0 && ( (*argv)[2] == '\0' || ( (*argv)[2] >= '0' && (*argv)[2] <= '9' ) ) ) {
			// -k<N> intra-file chunks; -k alone => auto (hardware concurrency, capped)
			int cores = (int) std::thread::hardware_concurrency();
			if ( (*argv)[2] == '\0' ) {
				num_chunks = ( cores > 1 ) ? ( ( cores < MAX_CHUNKS ) ? cores : MAX_CHUNKS ) : 1;
			} else if ( sscanf( (*argv) + 2, "%i", &tmp_val ) == 1 ) {
				num_chunks = ( tmp_val < 1 ) ? 1 : ( ( tmp_val > MAX_CHUNKS ) ? MAX_CHUNKS : tmp_val );
			}
		}
		#if defined(DEV_BUILD)
		else if ( strcmp((*argv), "-dev") == 0 ) {
			developer = true;
		}
		else if ( strcmp((*argv), "-test") == 0 ) {
			verify_lv = 2;
		}
		else if ( strcmp((*argv), "-san") == 0 ) {
			if ( file_exists( STAT_ANALYSIS_CSV ) )
				remove( STAT_ANALYSIS_CSV );
			action = A_STATS_ANALYSIS;
		}
		else if ( strcmp((*argv), "-fan") == 0 ) {
			if ( file_exists( FILE_ANALYSIS_CSV ) )
				remove( FILE_ANALYSIS_CSV );
			action = A_FILE_ANALYSIS;
		}
		else if ( strcmp((*argv), "-ban") == 0 ) {
			action = A_BLOCK_ANALYSIS;
		}
		else if ( strcmp((*argv), "-dmp") == 0 ) {
			action = A_DUMP_SEPERATE;
		}
		else if ( strcmp((*argv), "-pgm") == 0 ) {
			action = A_PGM_INFO;
		}
		else if ( ( strcmp((*argv), "-comp") == 0) ) {
			action = A_COMPRESS;
		}
		#endif
		else if ( strcmp((*argv), "-") == 0 ) {
			// switch standard message out stream
			msgout = stderr;
			// use "-" as placeholder for stdin
			*(tmp_flp++) = (char*) "-";
		}
		else {
			// if argument is not switch, it's a filename
			*(tmp_flp++) = *argv;
		}
	}

	// v1.2: -r expansion. Replace any directory entries in filelist with
	// the .mp3/.pmp files they contain (recursively). Tracks src_root in
	// filelist_srcroot so -fs can mirror the relative subdir under -od.
	if ( recursive ) {
		std::vector<std::pair<std::string,std::string>> extra_files; // (path, src_root)
		for ( int fi = 0; filelist[ fi ] != NULL; fi++ ) {
			std::error_code ec;
			std::filesystem::path p;
			try { p = std::filesystem::path( filelist[ fi ] ); }
			catch ( ... ) { continue; }
			if ( std::filesystem::is_directory( p, ec ) ) {
				filelist[ fi ] = NULL; // mark for compaction
				collect_files_recursive( p, extra_files );
			}
		}
		if ( !extra_files.empty() ) {
			int existing = 0;
			for ( ; filelist[ existing ] != NULL; existing++ );
			filelist = (char**) realloc( filelist,
				( existing + extra_files.size() + 1 ) * sizeof( char* ) );
			filelist_srcroot = (char**) realloc( filelist_srcroot,
				( existing + extra_files.size() + 1 ) * sizeof( char* ) );
			int wi = 0;
			for ( int fi = 0; fi < existing + (int)extra_files.size(); fi++ ) {
				if ( fi < existing && filelist[ fi ] != NULL ) {
					filelist[ wi ] = filelist[ fi ];
					filelist_srcroot[ wi ] = NULL;  // direct args have no src_root
					wi++;
				}
			}
			for ( auto& pr : extra_files ) {
				char* fn = (char*) malloc( pr.first.size() + 1 );
				strcpy( fn, pr.first.c_str() );
				filelist[ wi ] = fn;
				char* sr = (char*) malloc( pr.second.size() + 1 );
				strcpy( sr, pr.second.c_str() );
				filelist_srcroot[ wi ] = sr;
				wi++;
			}
			filelist[ wi ] = NULL;
			filelist_srcroot[ wi ] = NULL;
		}
	}

	// count number of files (or filenames) in filelist
	for ( file_cnt = 0; filelist[ file_cnt ] != NULL; file_cnt++ );

	// alloc arrays for error messages and types storage
	err_list = (char**) calloc( file_cnt, sizeof( char* ) );
	err_tp   = (int*) calloc( file_cnt, sizeof( int ) );
}
#endif


/* -----------------------------------------------
	UI for processing one file
	----------------------------------------------- */
	
#if !defined(BUILD_LIB)
INTERN void process_ui( void )
{
	clock_t begin, end;
	const char* actionmsg  = NULL;
	const char* errtypemsg = NULL;
	int total, bpms;
	float cr;	
	
	
	errorfunction = NULL;
	errorlevel = 0;
	mp3filesize = 0;
	pmpfilesize = 0;	
	#if !defined(DEV_BUILD)
	// v1.2: preserve the list/stats subcommands; everything else is compress.
	// Single-thread only: in -th, 'action' is shared and already fixed by the
	// subcommand, so writing it per-file would be a (benign-valued) data race.
	if ( num_threads <= 1 && action != A_LIST && action != A_STATS ) action = A_COMPRESS;
	#endif

	// compare file name, set pipe if needed
	if ( ( strcmp( filelist[ file_no ], "-" ) == 0 ) && ( action == A_COMPRESS ) ) {
		pipe_on = true;
		filelist[ file_no ] = (char*) "STDIN";
	}
	else {		
		pipe_on = false;
	}
	
	if ( verbosity < 0 && num_threads <= 1 ) { // progress bar UI (single-thread only)
		// update progress message
		fprintf( msgout, "Processing file %2i of %2i ", file_no + 1, file_cnt );
		progress_bar( file_no, file_cnt );
		fprintf( msgout, "\r" );
		execute( check_file );
	}
	else { // standard UI (verbosity 0/1/2; module_mode and -th run silently)
		if ( verbosity > 1 && !module_mode && num_threads <= 1 )
			fprintf( msgout,  "\nProcessing file %i of %i \"%s\"\n----------------------------------------",
						file_no + 1, file_cnt, filelist[ file_no ] );

		// check input file and determine filetype
		execute( check_file );

		// get specific action message
		if ( filetype == F_UNK ) actionmsg = "unknown filetype";
		else switch ( action ) {
			case A_COMPRESS: ( filetype == F_MP3 || filetype == F_MP2 ) ? actionmsg = "Compressing" : actionmsg = "Decompressing";
				break;
			case A_DUMP_SEPERATE: actionmsg = "Dumping binary data"; break;
			case A_PGM_INFO: actionmsg = "Writing PGM"; break;
			case A_FILE_ANALYSIS: actionmsg = "Analysing files"; break;
			case A_BLOCK_ANALYSIS: actionmsg = "Analysing frames"; break;
			case A_STATS_ANALYSIS: actionmsg = "Analysing statistics"; break;
			case A_LIST: actionmsg = "Listing"; break;
			case A_STATS: actionmsg = "Analyzing"; break;
		}

		if ( !module_mode && num_threads <= 1 && filetype != F_UNK ) {
			// list/stats print the filename as a header; their info follows below
			if ( action == A_LIST || action == A_STATS )
				fprintf( msgout, "\n%s\n", filelist[ file_no ] );
			else if ( verbosity < 2 )
				fprintf( msgout, "%s%s%s -> ", COL_CYAN, actionmsg, COL_RESET );
		}
	}
	if ( num_threads <= 1 ) fflush( msgout );

	// silent skip: check_file set F_UNK with no error → wrong file type for the
	// chosen subcommand (compress-only saw PMP, decompress-only/list saw MP3,
	// stats saw PMP). Nothing to process, no result line, no output written.
	if ( filetype == F_UNK && errorlevel == 0 ) {
		if ( str_in  != NULL ) { delete( str_in  ); str_in  = NULL; }
		if ( str_out != NULL ) { delete( str_out ); str_out = NULL; }
		return;
	}
	
	
	// main function routine
	begin = clock();
	
	// streams are initiated, start processing file
	process_file();
	
	// close iostreams
	if ( str_in  != NULL ) { delete( str_in  ); str_in  = NULL; }
	if ( str_out != NULL ) { delete( str_out ); str_out = NULL; }
	if ( str_str != NULL ) { delete( str_str ); str_str = NULL; }
	// delete if broken or if output not needed. list/stats create no output
	// file, and -dry only ever wrote to memory, so skip both.
	if ( ( !pipe_on ) && ( action != A_LIST ) && ( action != A_STATS ) && ( !dry_run )
	     && ( ( errorlevel >= err_tol ) || ( action != A_COMPRESS ) ) ) {
		if ( filetype == F_MP3 ) {
			if ( file_exists( pmpfilename ) ) remove( pmpfilename );
		} else if ( filetype == F_PMP ) {
			if ( file_exists( mp3filename ) ) remove( mp3filename );
		}
	}
	
	end = clock();	
	
	// speed and compression ratio calculation
	total = (int) ( (double) (( end - begin ) * 1000) / CLOCKS_PER_SEC );
	bpms  = ( total > 0 ) ? ( mp3filesize / total ) : mp3filesize;
	cr    = ( mp3filesize > 0 ) ? ( 100.0 * pmpfilesize / mp3filesize ) : 0;

	
	if ( verbosity >= 0 && !module_mode && num_threads <= 1 ) { // standard UI
		if ( verbosity > 1 )
			fprintf( msgout,  "\n----------------------------------------" );
		
		// display success/failure message
		switch ( verbosity ) {
			case 0:
				{
					const char* _sl = strrchr( filelist[ file_no ], '/' );
					#if defined(_WIN32) || defined(WIN32)
					const char* _bs = strrchr( filelist[ file_no ], '\\' );
					if ( _bs && ( !_sl || _bs > _sl ) ) _sl = _bs;
					#endif
					const char* _fn = _sl ? _sl + 1 : filelist[ file_no ];
					if ( errorlevel < err_tol ) {
						if ( action == A_COMPRESS && ( filetype == F_MP3 || filetype == F_MP2 ) ) {
							long long orig_kb = ( mp3filesize + 512 ) / 1024;
							long long comp_kb = ( pmpfilesize + 512 ) / 1024;
							double time_s = ( total >= 0 ) ? total / 1000.0 : 0.0;
							#if defined(_WIN32) || defined(WIN32)
							fprintf( msgout, "\r  +  %-46.46s %6lld KB -> %6lld KB  %5.1f%%  %5.2fs\n",
								_fn, orig_kb, comp_kb, cr, time_s );
							#else
							fprintf( msgout, "\r  %s\xe2\x9c\x93%s  %-46.46s %6lld KB \xe2\x86\x92 %6lld KB  %5.1f%%  %5.2fs\n",
								COL_BGREEN, COL_RESET, _fn, orig_kb, comp_kb, cr, time_s );
							#endif
						} else if ( action != A_LIST && action != A_STATS ) {
							#if defined(_WIN32) || defined(WIN32)
							fprintf( msgout, "\r  +  %-46.46s DONE\n", _fn );
							#else
							fprintf( msgout, "\r  %s\xe2\x9c\x93%s  %-46.46s DONE\n", COL_BGREEN, COL_RESET, _fn );
							#endif
						}
					} else {
						#if defined(_WIN32) || defined(WIN32)
						fprintf( msgout, "\r  x  %-46.46s ERROR\n", _fn );
						#else
						fprintf( msgout, "\r  %s\xe2\x9c\x97%s  %-46.46s %sERROR%s\n",
							COL_BRED, COL_RESET, _fn, COL_BRED, COL_RESET );
						#endif
					}
				}
				break;
			
			case 1:
				if ( errorlevel < err_tol ) fprintf( msgout, "%sDONE%s\n",  COL_BGREEN, COL_RESET );
				else                        fprintf( msgout, "%sERROR%s\n", COL_BRED,   COL_RESET );
				break;

			case 2:
				if ( errorlevel < err_tol ) fprintf( msgout,  "\n-> %s %sOK%s\n",    actionmsg, COL_BGREEN, COL_RESET );
				else                        fprintf( msgout,  "\n-> %s %sERROR%s\n", actionmsg, COL_BRED,   COL_RESET );
				break;
		}
		
		// set type of error message
		switch ( errorlevel ) {
			case 0:	errtypemsg = "none"; break;
			case 1: ( err_tol > 1 ) ?  errtypemsg = "warning (ignored)" : errtypemsg = "warning (skipped file)"; break;
			case 2: errtypemsg = "fatal error"; break;
		}
		
		// error/ warning message
		if ( errorlevel > 0 ) {
			fprintf( msgout, " %s -> %s:\n", get_status( errorfunction ), errtypemsg  );
			fprintf( msgout, " %s\n", errormessage );
		}
		if ( (verbosity > 0) && (errorlevel < err_tol) && (action == A_COMPRESS) ) {
			if ( total >= 0 ) {
				fprintf( msgout,  " time taken  : %7i msec\n", total );
				fprintf( msgout,  " byte per ms : %7i byte\n", bpms );
			}
			else {
				fprintf( msgout,  " time taken  : %7s msec\n", "N/A" );
				fprintf( msgout,  " byte per ms : %7s byte\n", "N/A" );
			}
			fprintf( msgout,  " comp. ratio : %7.2f %%\n", cr );		
		}	
		if ( ( verbosity > 1 ) && ( action == A_COMPRESS ) )
			fprintf( msgout,  "\n" );
	}
	else if ( verbosity < 0 && num_threads <= 1 ) { // progress bar UI (single-thread)
		// if this is the last file, update progress bar one last time
		if ( file_no + 1 == file_cnt ) {
			// update progress message
			fprintf( msgout, "Processed %2i of %2i files ", file_no + 1, file_cnt );
			progress_bar( 1, 1 );
			fprintf( msgout, "\r" );
		}	
	}
}
#endif


/* -----------------------------------------------
	gets statusmessage for function
	----------------------------------------------- */
	
#if !defined(BUILD_LIB)
INTERN inline const char* get_status( bool (*function)() )
{	
	if ( function == NULL ) {
		return "unknown action";
	} else if ( function == *check_file ) {
		return "Determining filetype";
	} else if ( function == *read_mp3 ) {
		return "Reading MP3";
	} else if ( function == *write_mp3 ) {
		return "Writing MP3";
	} else if ( function == *analyze_frames ) {
		return "Analysing frames";
	} else if ( function == *compress_mp3 ) {
		return "Compressing to PMP";
	} else if ( function == *uncompress_pmp ) {
		return "Uncompressing PMP";
	} else if ( function == *compress_mp3_chunked ) {
		return "Compressing to PMP (chunked)";
	} else if ( function == *uncompress_pmp_chunked ) {
		return "Uncompressing PMP (chunked)";
	} else if ( function == *swap_streams ) {
		return "Swapping input/output streams";
	} else if ( function == *compare_output ) {
		return "Verifying output stream";
	} else if ( function == *reset_buffers ) {
		return "Resetting program";
	}
	#if defined(DEV_BUILD)
	else if ( function == *write_file_analysis ) {
		return "Writing file analysis to csv";
	} else if ( function == *write_block_analysis ) {
		return "Writing block analysis to csv";
	} else if ( function == *write_stat_analysis ) {
		return "Writing statistic analysis to csv";
	} else if ( function == *visualize_headers ) {
		return "Writing binary info PGM";
	} else if ( function == *visualize_decoded_data ) {
		return "Writing decoded data PGMs";
	} else if ( function == *dump_main_sizes ) {
		return "Dumping main data sizes";
	} else if ( function == *dump_aux_sizes ) {
		return "Dumping aux data sizes";
	} else if ( function == *dump_bitrates ) {
		return "Dumping bitrates";
	} else if ( function == *dump_stereo_ms ) {
		return "Dumping MS stereo bits";
	} else if ( function == *dump_padding ) {
		return "Dumping padding bits";
	} else if ( function == *dump_main_data_bits ) {
		return "Dumping main data bits";
	} else if ( function == *dump_big_value_ns ) {
		return "Dumping big value pair #s";
	} else if ( function == *dump_global_gain ) {
		return "Dumping global gain";
	} else if ( function == *dump_slength ) {
		return "Dumping slength values";
	} else if ( function == *dump_block_types ) {
		return "Dumping block types";
	} else if ( function == *dump_sharing ) {
		return "Dumping sharing bits";
	} else if ( function == *dump_preemphasis ) {
		return "Dumping preemphasis setting";
	} else if ( function == *dump_coarse ) {
		return "Dumping coarse scf settings";
	} else if ( function == *dump_htable_sel ) {
		return "Dumping hufftable selections";
	} else if ( function == *dump_region_sizes ) {
		return "Dumping region sizes";
	} else if ( function == *dump_subblock_gains ) {
		return "Dumping subblock gain";
	} else if ( function == *dump_data_files ) {
		return "Dumping data files";
	} else if ( function == *dump_gg_ctx ) {
		return "Dumping global gain context";
	} else if ( function == *dump_decoded_data ) {
		return "Dumping decoded data";
	}
	#endif
	else {
		return "Function description missing!";
	}
}
#endif


/* -----------------------------------------------
	shows help in case of wrong input
	----------------------------------------------- */
	
#if !defined(BUILD_LIB)
INTERN void show_help( void )
{
	fprintf( msgout, "\n" );
	fprintf( msgout, "%s -- lossless MP3 compression. Typical reduction: ~16%%.\n", appname );
	fprintf( msgout, "Compresses MPEG audio (MP3) files to .pm3 archives and decompresses\n" );
	fprintf( msgout, "them back, with bit-for-bit identical reconstruction.\n" );
	fprintf( msgout, "\n" );
	fprintf( msgout, "Website: %s\n", website );
	fprintf( msgout, "\n" );
	fprintf( msgout, "Usage: %s <subcommand> [switches] [filename(s)]\n", appname );
	fprintf( msgout, "\n" );
	fprintf( msgout, "Subcommands:\n" );
	fprintf( msgout, " a         compress only: process MP3 files, skip .pm3\n" );
	fprintf( msgout, " x         decompress only: process .pm3 files, skip MP3\n" );
	fprintf( msgout, " mix       mixed mode: auto-detect (warns if both directions used)\n" );
	fprintf( msgout, " list      list .pm3 file info without decompressing\n" );
	fprintf( msgout, " stats     show MP3 file info (size, layer, channels) without compressing\n" );
	fprintf( msgout, "\n" );
	fprintf( msgout, "Switches:\n" );
	fprintf( msgout, "\n" );
	fprintf( msgout, " [-ver]   verify files after processing\n" );
	fprintf( msgout, " [-v?]    set level of verbosity (max: 2) (def: 0)\n" );
	fprintf( msgout, " [-vp]    progress bar mode (overrides -v?)\n" );
	fprintf( msgout, " [-np]    no pause after processing files\n" );
	fprintf( msgout, " [--no-color] disable ANSI color output (also respected via NO_COLOR env var)\n" );
	fprintf( msgout, " [-o]     overwrite existing files\n" );
	fprintf( msgout, " [-th<N>] use N threads for batch processing (0=auto; forces verify)\n" );
	fprintf( msgout, " [-k<N>]  intra-file parallel chunks for speed (default 1=max ratio; 0=auto)\n" );
	fprintf( msgout, " [-r]     recurse into subdirectories\n" );
	fprintf( msgout, " [-fs]    preserve source folder structure under -od (use with -r)\n" );
	fprintf( msgout, " [-dry]   dry run: simulate without writing output files\n" );
	fprintf( msgout, " [-module] machine-friendly output: OK/ERROR + time only\n" );
	fprintf( msgout, " [-od<p>] write output files to directory <p>\n" );
	fprintf( msgout, " [-p]     proceed on warnings\n" );
	fprintf( msgout, " [-d]     discard meta-info (ID3 tags)\n" );
	#if defined(DEV_BUILD)
	if ( developer ) {
	fprintf( msgout, "\n" );
	fprintf( msgout, " [-fan]   write files analysis to CSV file\n" );
	fprintf( msgout, " [-ban]   write block analysis to CSV file\n" );
	fprintf( msgout, " [-san]   write stats analysis to CSV file\n" );
	fprintf( msgout, " [-pgm]   visualize data as PGM image\n" );
	fprintf( msgout, " [-dmp]   dump data to several binary files\n" );
	}
	#endif
	fprintf( msgout, "\n" );
	fprintf( msgout, "Examples: \"%s a -v1 -o luka.%s\"\n", appname, mp3_ext );
	fprintf( msgout, "          \"%s a -p *.%s\"\n", appname, mp3_ext );
	fprintf( msgout, "          \"%s x archive.%s\"\n", appname, pmp_ext );
	fprintf( msgout, "          \"%s mix -r -od/out folder/\"\n", appname );
}
#endif


/* -----------------------------------------------
	processes one file
	----------------------------------------------- */

INTERN void process_file( void )
{	
	if ( filetype == F_MP3 ) {
		switch ( action ) {
			case A_COMPRESS:
				if ( num_chunks > 1 ) {
					// intra-file parallel chunking; self-verifies internally when verify_lv>0
					execute( compress_mp3_chunked );
				} else {
					execute( read_mp3 );
					execute( analyze_frames );
					execute( compress_mp3 );
					#if !defined(BUILD_LIB)
					if ( verify_lv > 0 ) { // verifcation
						execute( reset_buffers );
						execute( swap_streams );
						execute( uncompress_pmp );
						execute( write_mp3 );
						execute( compare_output );
					}
					#endif
				}
				break;
				
			#if !defined(BUILD_LIB)
			case A_STATS:
				execute( read_mp3 );
				execute( analyze_frames );
				execute( stats_mp3 );
				break;

			case A_LIST:
				snprintf( errormessage, MSG_SIZE, "list is only supported for .pm3 files" );
				errorlevel = 2;
				break;
			#endif

			#if !defined(BUILD_LIB) && defined(DEV_BUILD)
			case A_DUMP_SEPERATE:
				execute( read_mp3 );
				execute( analyze_frames );
				execute( dump_main_sizes );
				execute( dump_aux_sizes );
				execute( dump_main_data_bits );
				execute( dump_big_value_ns );
				execute( dump_global_gain );
				execute( dump_bitrates );
				execute( dump_htable_sel );
				execute( dump_region_sizes );
				execute( dump_slength );
				execute( dump_stereo_ms );
				execute( dump_padding );
				execute( dump_block_types );
				execute( dump_sharing );
				execute( dump_preemphasis );
				execute( dump_coarse );
				execute( dump_subblock_gains );
				execute( dump_data_files );
				execute( dump_gg_ctx );
				execute( dump_decoded_data );
				break;
			
			case A_PGM_INFO:
				execute( read_mp3 );
				execute( analyze_frames );
				execute( visualize_headers );
				execute( visualize_decoded_data );
				break;
				
			case A_FILE_ANALYSIS:
				execute( read_mp3 );
				execute( analyze_frames );
				execute( write_file_analysis );
				break;
			
			case A_BLOCK_ANALYSIS:
				execute( read_mp3 );
				execute( analyze_frames );
				execute( write_block_analysis );
				break;
			
			case A_STATS_ANALYSIS:
				execute( read_mp3 );
				execute( analyze_frames );
				execute( write_stat_analysis );
				break;
			#else
			default:
				break;
			#endif
		}
	}
	else if ( filetype == F_PMP )	{
		switch ( action )
		{
			case A_COMPRESS:
				if ( arch_chunked ) {
					// "MK" container: decode all chunks, write concatenated mp3 directly
					execute( uncompress_pmp_chunked );
				} else {
					execute( uncompress_pmp );
					execute( write_mp3 );
					#if !defined(BUILD_LIB)
					if ( verify_lv > 0 ) { // verify
						execute( reset_buffers );
						execute( swap_streams );
						execute( read_mp3 );
						execute( analyze_frames );
						execute( compress_mp3 );
						execute( compare_output );
					}
					#endif
				}
				break;

			#if !defined(BUILD_LIB)
			case A_LIST:
				execute( list_pmp );
				break;

			case A_STATS:
				snprintf( errormessage, MSG_SIZE, "stats is only supported for MP3 files" );
				errorlevel = 2;
				break;
			#endif

			#if !defined(BUILD_LIB) && defined(DEV_BUILD)
			case A_DUMP_SEPERATE:
				execute( uncompress_pmp );
				execute( dump_main_sizes );
				execute( dump_aux_sizes );
				execute( dump_main_data_bits );
				execute( dump_big_value_ns );
				execute( dump_global_gain );
				execute( dump_bitrates );
				execute( dump_htable_sel );
				execute( dump_region_sizes );
				execute( dump_slength );
				execute( dump_stereo_ms );
				execute( dump_padding );
				execute( dump_block_types );
				execute( dump_sharing );
				execute( dump_preemphasis );
				execute( dump_coarse );
				execute( dump_subblock_gains );
				execute( dump_data_files );
				execute( dump_gg_ctx );
				execute( dump_decoded_data );
				break;
				
			case A_PGM_INFO:
				execute( uncompress_pmp );
				execute( visualize_headers );
				execute( visualize_decoded_data );
				break;
				
			case A_FILE_ANALYSIS:
				execute( uncompress_pmp );
				execute( write_file_analysis );
				break;
			
			case A_BLOCK_ANALYSIS:
				execute( uncompress_pmp );
				execute( write_block_analysis );
				break;
			
			case A_STATS_ANALYSIS:
				execute( uncompress_pmp );
				execute( write_stat_analysis );
				break;
			#else
			default:
				break;
			#endif
		}
	}
	#if !defined(BUILD_LIB)
	// Layer I/II, backed by the packMP2 library
	else if ( filetype == F_MP2 ) {
		if ( action == A_COMPRESS ) {
			execute( l2_compress );
			if ( verify_lv > 0 ) {
				execute( reset_buffers );
				execute( swap_streams );
				execute( l2_decompress );
				execute( compare_output );
			}
		}
		else if ( action == A_STATS ) execute( stats_l2 );
		else if ( action == A_LIST ) {
			snprintf( errormessage, MSG_SIZE, "list is only supported for .pm3 archives" );
			errorlevel = 2;
		}
	}
	else if ( filetype == F_PL2 ) {
		if ( action == A_COMPRESS ) execute( l2_decompress );
		else if ( action == A_LIST ) execute( list_l2 );
		else if ( action == A_STATS ) {
			snprintf( errormessage, MSG_SIZE, "stats is only supported for raw mp2/mp1 files" );
			errorlevel = 2;
		}
	}
	#endif
	#if !defined(BUILD_LIB) && defined(DEV_BUILD)
	// write error file if verify lv > 1
	if ( ( verify_lv > 1 ) && ( errorlevel >= err_tol ) )
		write_errfile();
	#endif
	// reset buffers
	reset_buffers();
}


/* -----------------------------------------------
	main-function execution routine
	----------------------------------------------- */

INTERN void execute( bool (*function)() )
{	
	if ( errorlevel < err_tol ) {
		#if !defined BUILD_LIB
		clock_t begin, end;
		bool success;
		int total;
		
		// write statusmessage (suppressed in -th batch mode to avoid interleaving)
		if ( verbosity == 2 && num_threads <= 1 ) {
			fprintf( msgout,  "\n%s ", get_status( function ) );
			for ( int i = strlen( get_status( function ) ); i <= 30; i++ )
				fprintf( msgout,  " " );
		}
		
		// set starttime
		begin = clock();
		// call function
		success = ( *function )();
		// set endtime
		end = clock();
		
		if ( ( errorlevel > 0 ) && ( errorfunction == NULL ) )
			errorfunction = function;
		
		// write time or failure notice
		if ( success ) {
			total = (int) ( (double) (( end - begin ) * 1000) / CLOCKS_PER_SEC );
			if ( verbosity == 2 && num_threads <= 1 ) fprintf( msgout,  "%6ims", ( total >= 0 ) ? total : -1 );
		}
		else {
			errorfunction = function;
			if ( verbosity == 2 && num_threads <= 1 ) fprintf( msgout,  "%8s", "ERROR" );
		}
		#else
		// call function
		( *function )();
		
		// store errorfunction if needed
		if ( ( errorlevel > 0 ) && ( errorfunction == NULL ) )
			errorfunction = function;
		#endif
	}
}

/* ----------------------- End of main interface functions -------------------------- */

/* ----------------------- Begin of main functions -------------------------- */


/* -----------------------------------------------
	check file and determine filetype
	----------------------------------------------- */

#if !defined(BUILD_LIB)
INTERN bool check_file( void )
{	
	unsigned char fileid[ 2 ] = { 0, 0 };
	const char* filename = filelist[ file_no ];
	
	
	// open input stream, check for errors
	str_in = new iostream( (void*) filename, ( !pipe_on ) ? 0 : 2, 0, 0 );
	if ( str_in->chkerr() ) {
		snprintf( errormessage, MSG_SIZE, FRD_ERRMSG, filename );
		errorlevel = 2;
		return false;
	}
	
	// free memory from filenames if needed
	if ( mp3filename != NULL ) { free( mp3filename ); mp3filename = NULL; }
	if ( pmpfilename != NULL ) { free( pmpfilename ); pmpfilename = NULL; }
	
	// immediately return error if 2 bytes can't be read
	if ( str_in->read( fileid, 1, 2 ) != 2 ) { 
		filetype = F_UNK;
		snprintf( errormessage, MSG_SIZE, "file doesn't contain enough data" );
		errorlevel = 2;
		return false;
	}
	
	// rewind (need to start from the beginning)
	if ( str_in->rewind() != 0 ) {
		snprintf( errormessage, MSG_SIZE, FRD_ERRMSG, filename );
		errorlevel = 2;
		return false;
	}
	
	// check file id, determine filetype
	bool is_chunked = ( fileid[0] == pmc_magic[0] ) && ( fileid[1] == pmc_magic[1] );
	bool is_pmp = ( ( fileid[0] == pmp_magic[0] ) && ( fileid[1] == pmp_magic[1] ) ) || is_chunked;
	bool is_pl2 = ( fileid[0] == (unsigned char) l2_magic[0] ) && ( fileid[1] == (unsigned char) l2_magic[1] );
	arch_chunked = is_chunked;	// "MK" container -> decode via uncompress_pmp_chunked
	if ( is_pmp || is_pl2 ) {
		// PMP/M2 marker -> compressed archive (Layer III / Layer I-II)
		filetype = is_pl2 ? F_PL2 : F_PMP;
		// skip silently if compress-only: reset to F_UNK so process_file()
		// does nothing (was a NULL-str_out crash when it tried to decode).
		if ( compress_only ) { filetype = F_UNK; return false; }
		// v1.2 'list' reads the header from the input stream only — no output
		// (L3 -> F_PMP; Layer I/II -> F_PL2, v3.0)
		if ( action == A_LIST && ( filetype == F_PMP || filetype == F_PL2 ) ) return true;
		// create filenames. Layer I/II archives reconstruct to .mp2 (Layer II
		// dominates real-world content; Layer I is rare -- content itself is
		// always correct regardless of this cosmetic extension choice).
		const char* out_ext = is_pl2 ? "mp2" : mp3_ext;
		if ( !pipe_on ) {
			pmpfilename = (char*) calloc( strlen( filename ) + 1, sizeof( char ) );
			strcpy( pmpfilename, filename );
			mp3filename = ( overwrite ) ?
				create_filename( filename, (char*) out_ext ) :
				unique_filename( filename, (char*) out_ext );
		}
		else {
			mp3filename = create_filename( "STDOUT", NULL );
			pmpfilename = create_filename( "STDIN", NULL );
		}
		// open output stream, check for errors. -dry writes to memory (discarded).
		str_out = dry_run ? new iostream( NULL, 1, 0, 1 )
			: new iostream( (void*) mp3filename, ( !pipe_on ) ? 0 : 2, 0, 1 );
		if ( str_out->chkerr() ) {
			snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, mp3filename );
			errorlevel = 2;
			return false;
		}
	}
	else {
		// input file: peek the first MPEG frame to route Layer III (main codec)
		// vs Layer I/II (separate codec). Layer III -> F_MP3, Layer I/II -> F_MP2.
		filetype = F_MP3;
		{
			unsigned char pk[ 8192 ];
			str_in->rewind();
			int pn = str_in->read( pk, 1, sizeof(pk) );
			str_in->rewind();
			for ( int p = 0; p + 4 <= pn; p++ ) {
				if ( pk[p] != 0xFF || ( pk[p+1] & 0xE0 ) != 0xE0 ) continue;
				int lb = ( pk[p+1] >> 1 ) & 0x3, ver = ( pk[p+1] >> 3 ) & 0x3;
				int br = ( pk[p+2] >> 4 ) & 0xF, sr = ( pk[p+2] >> 2 ) & 0x3;
				if ( lb == 0 || ver == 1 || br == 0 || br == 15 || sr == 3 ) continue; // invalid hdr
				int layer = 4 - lb;	// 1=I, 2=II, 3=III
				// Layer II (mp2) is backed by the packMP2 library. Layer I (mp1)
				// stays disabled -- packMP2 is structurally Layer II-only (different
				// frame length formula, no SCFSI, different bit-alloc/bitrate
				// tables; confirmed by both packMP2's own source and an empirical
				// test: a Layer II file with its sync header patched to Layer I
				// is cleanly rejected, not misdecoded). Rejecting explicitly here
				// keeps that failure honest instead of silently falling back to
				// a verbatim "compressed 100%" archive that never actually helped.
				if ( layer == 2 ) filetype = F_MP2;
				else if ( layer == 1 ) {
					filetype = F_UNK;
					snprintf( errormessage, MSG_SIZE, "Layer I (mp1) not supported - Layer II (mp2) and Layer III (mp3) only" );
					errorlevel = 1;
					return false;
				}
				break;
			}
		}
		// skip silently if decompress-only: reset to F_UNK so process_file()
		// does nothing (was a NULL-str_out crash when it tried to compress).
		if ( decompress_only ) { filetype = F_UNK; return false; }
		// v1.2 'stats' reads the input only — no output file
		// (L3 -> F_MP3; Layer I/II -> F_MP2, v3.0)
		if ( action == A_STATS && ( filetype == F_MP3 || filetype == F_MP2 ) ) return true;
		// create filenames
		if ( !pipe_on ) {
			mp3filename = (char*) calloc( strlen( filename ) + 1, sizeof( char ) );
			strcpy( mp3filename, filename );
			pmpfilename = ( overwrite ) ?
				create_filename( filename, (char*) pmp_ext ) :
				unique_filename( filename, (char*) pmp_ext );
		}
		else {
			mp3filename = create_filename( "STDIN", NULL );
			pmpfilename = create_filename( "STDOUT", NULL );
		}
		// open output stream, check for errors. -dry writes to memory (discarded).
		str_out = dry_run ? new iostream( NULL, 1, 0, 1 )
			: new iostream( (void*) pmpfilename, ( !pipe_on ) ? 0 : 2, 0, 1 );
		if ( str_out->chkerr() ) {
			snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, pmpfilename );
			errorlevel = 2;
			return false;
		}
	}
	
	
	return true;
}
#endif


/* -----------------------------------------------
	swap streams / init verification
	----------------------------------------------- */
	
#if !defined(BUILD_LIB)
INTERN bool swap_streams( void )	
{
	// store input stream
	str_str = str_in;
	str_str->rewind();
	
	// replace input stream by output stream / switch mode for reading
	str_in = str_out;
	str_in->switch_mode();
	
	// open new stream for output / check for errors
	str_out = new iostream( NULL, 1, 0, 1 );
	if ( str_out->chkerr() ) {
		snprintf( errormessage, MSG_SIZE, "error opening comparison stream" );
		errorlevel = 2;
		return false;
	}
	
	
	return true;
}
#endif


/* -----------------------------------------------
	comparison between input & output
	----------------------------------------------- */

#if !defined(BUILD_LIB)
INTERN bool compare_output( void )
{
	unsigned char* buff_ori;
	unsigned char* buff_cmp;
	int bsize = 1024;
	int dsize;
	int i, b;
	
	
	// init buffer arrays
	buff_ori = ( unsigned char* ) calloc( bsize, sizeof( char ) );
	buff_cmp = ( unsigned char* ) calloc( bsize, sizeof( char ) );
	if ( ( buff_ori == NULL ) || ( buff_cmp == NULL ) ) {
		if ( buff_ori != NULL ) free( buff_ori );
		if ( buff_cmp != NULL ) free( buff_cmp );
		snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
		errorlevel = 2;
		return false;
	}
	
	// switch output stream mode / check for stream errors
	str_out->switch_mode();
	while ( true ) {
		if ( str_out->chkerr() )
			snprintf( errormessage, MSG_SIZE, "error in comparison stream" );
		else if ( str_in->chkerr() )
			snprintf( errormessage, MSG_SIZE, "error in output stream" );
		else if ( str_str->chkerr() )
			snprintf( errormessage, MSG_SIZE, "error in input stream" );
		else break;
		errorlevel = 2;
		return false;
	}
	
	// compare sizes
	dsize = str_str->getsize();
	if ( str_out->getsize() != dsize ) {
		snprintf( errormessage, MSG_SIZE, "file sizes do not match" );
		errorlevel = 2;
		return false;
	}
	
	// compare files byte by byte
	for ( i = 0; i < dsize; i++ ) {
		b = i % bsize;
		if ( b == 0 ) {
			str_str->read( buff_ori, sizeof( char ), bsize );
			str_out->read( buff_cmp, sizeof( char ), bsize );
		}
		if ( buff_ori[ b ] != buff_cmp[ b ] ) {
			snprintf( errormessage, MSG_SIZE, "difference found at 0x%X", i );
			errorlevel = 2;
			return false;
		}
	}
	
	
	return true;
}
#endif


/* -----------------------------------------------
	set each variable to its initial value
	----------------------------------------------- */

INTERN bool reset_buffers( void )
{
	mp3Frame* frame;
	
	
	
	// --- free frame data ---
	// start from first frame, throw away all frame data
	if ( firstframe != NULL ) {
		frame = firstframe;
		while ( true ) {
			if ( frame->next != NULL ) {
				frame = frame->next;
				mp3_discard_frame( frame->prev );
				frame->prev = NULL;
			} else {
				mp3_discard_frame( frame );
				break;
			}
		}
	}
	firstframe = NULL;
	lastframe = NULL;
	
	// throw away huffman coded data block
	if ( main_data != NULL ) free ( main_data );
	main_data = NULL;
	main_data_size = 0;
	
	// --- free other data ---
	if ( data_before != NULL ) free ( data_before );
	if ( data_after != NULL ) free( data_after );
	if ( unmute_data != NULL ) free( unmute_data );
	data_before = NULL;
	data_after = NULL;
	unmute_data = NULL;
	data_before_size = 0;
	data_after_size = 0;
	unmute_data_size = 0;
	// stale apic_* would otherwise leak into the next file processed on this
	// thread (e.g. a file with no ID3 tag at all right after one that had a
	// recompressed cover) -- pmp_write_header/uncompress_pmp both read these
	// unconditionally, so they must not carry over.
	apic_present = false;
	apic_offset = apic_orig_len = apic_pjg_len = 0;
	if ( gg_context[0] != NULL ) free ( gg_context[0] );
	if ( gg_context[1] != NULL ) free ( gg_context[1] );
	gg_context[0] = NULL;
	gg_context[1] = NULL;
	
	// --- reset global variables ---
	g_nframes    =  0; // number of frames
	g_nchannels  =  0; // number of channels
	g_samplerate =  0; // sample rate
	g_bitrate    =  0; // average bit rate
	n_bad_first  =  0; // # of bad first frames
	
	// --- reset frame analysis variables ---
	i_mpeg			= -1; // mpeg - non changing
	i_layer			= -1; // layer - non changing
	i_samplerate	= -1; // sample rate - non changing
	i_bitrate		= -1; // bit rate - value or -1 (variable)
	i_protection	= -1; // checksum - for all (1), none (0) or some (-1) frames
	i_padding		= -1; // padding - for all (1), none (0) or some (-1) frames
	i_privbit		= -1; // private bit - value or -1 (variable)
	i_channels		= -1; // channel mode - non changing
	i_stereo_ms		= -1; // ms stereo - for all (1), none (0) or some (-1) frames
	i_stereo_int	= -1; // int stereo - for all (1), none (0) or some (-1) frames
	i_copyright		= -1; // copyright bit - value or -1 (variable)
	i_original		= -1; // original bit - value or -1 (variable)
	i_emphasis		= -1; // emphasis - value or -1 (variable)
	i_padbits		= -1; // side info padding bits - value or -1 (variable)
	i_bit_res		= -1; // bit reservoir - is used (1) or not used (0)
	i_share			= -1; // scalefactor sharing - is used (1) or not used (0)
	i_sblocks		= -1; // special blocks - are used (1) or not used (0)
	i_mixed			= -1; // mixed blocks - are used (1) or not used (0)
	i_preemphasis	= -1; // preemphasis - value or -1 (variable)
	i_coarse		= -1; // coarse scalefactors - value or -1 (variable)
	i_sbgain		= -1; // subblock gain - is used (1) or not used (0)
	i_aux_h			= -1; // auxiliary data handling - none (0), at begin and end (1), between frames (-1)
	i_sb_diff		= -1; // special blocks diffs between ch0 and ch1 - none (0) or some (-1)
	
	
	return true;
}


/* -----------------------------------------------
	parse MP3 frame structure
	----------------------------------------------- */

INTERN bool read_mp3( void )
{
	unsigned char* mp3data;
	int main_data_begin;
	int main_data_end;
	
	char mpeg = -1;
	char layer = -1;
	char samples = -1;
	char channels = -1;
	
	abytewriter* data_writer;
	mp3Frame* frame = NULL;
	bool incomplete_last_frame = false;
	int type;
	int pos;
	
	
	// read the first few bytes from the file into memory
	// (these will be used to check for proper frames)
	mp3filesize = str_in->getsize();
	if ( mp3filesize > FIRST_FRAME_AREA ) mp3filesize = FIRST_FRAME_AREA;
	mp3data = (unsigned char*) calloc( mp3filesize + 1, sizeof( char ) );
	if ( ( mp3data == NULL ) || ( mp3filesize <= 0 ) ) {
		if ( mp3data != NULL ) free( mp3data );
		snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
		errorlevel = 2;
		return false;
	}
	str_in->read( mp3data, 1, mp3filesize );
	
	// find first proper mpeg audio frame
	pos = mp3_seek_firstframe( mp3data, mp3filesize );
	if ( pos == -1 ) {
		snprintf( errormessage, MSG_SIZE, "no mpeg audio data recognized" );
		errorlevel = 2;
		free( mp3data );
		return false;
	}
	main_data_begin = pos;
	
	// check first frame header for MPEG and LAYER version.
	// v1.3: accept Layer III for MPEG-1, MPEG-2 and MPEG-2.5; reject Layers I/II.
	if ( mp3filesize - pos >= 2 ) {
		type = MBITS( mp3data[pos+1], 5, 1 );
		if ( ( type != MPEG1_LAYER_III ) && ( type != MPEG2_LAYER_III ) && ( type != MPEG2_5_LAYER_III ) ) {
			snprintf( errormessage, MSG_SIZE, "file is %s, not supported", filetype_description[type] );
			errorlevel = 2;
			free( mp3data );
			return false;
		}
	}
	
	// if theres still more data, read the whole file into memory now
	// (bad for mem consumption, but for now -> so what?)
	if ( mp3filesize == FIRST_FRAME_AREA ) {
		mp3filesize = str_in->getsize(); // check size of file again
		mp3data = (unsigned char*) frealloc( mp3data, mp3filesize * sizeof( char ) );
		if ( mp3data == NULL ) {
			snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
			errorlevel = 2;
			return false;
		}
		str_in->read( mp3data + FIRST_FRAME_AREA, 1, mp3filesize - FIRST_FRAME_AREA );
	}
	
	
	// now, read frames until the end of the stream or a bad frame is encountered
	data_writer = new abytewriter(0);
	while ( pos < mp3filesize ) {
		// read frame, check result
		frame = mp3_read_frame( mp3data + pos, mp3filesize - pos );
		if ( frame == NULL ) {
			if ( errorlevel == 2 ) {
				// append some useful info to the existing error message
				size_t _emlen = strlen( errormessage );
				snprintf( errormessage + _emlen, MSG_SIZE - _emlen, " (frame #%i at 0x%X)",
					( lastframe != NULL ) ? lastframe->n + 1 : 0, pos );
				free( mp3data );
				delete( data_writer );
				return false;
			}
			else break;
		} else if ( frame->frame_size > mp3filesize - pos ) {
			// check for incomplete last frame
			incomplete_last_frame = true;
			mp3_discard_frame( frame );	
			break;
		} else if ( ( frame->mpeg != mpeg ) || ( frame->layer != layer ) ||
			( frame->samples != samples ) || ( frame->channels != channels ) ) {
			// check for inconsistencies
			if ( mpeg == -1 ) {
				mpeg     = frame->mpeg;
				layer    = frame->layer;
				samples  = frame->samples;
				channels = frame->channels;
			} else {			
				mp3_discard_frame( frame );			
				break;
			}
		}
		if ( lastframe != NULL ) { // fix previous frames aux size, check for problems
			if ( frame->aux_size < 0 ) { // main data out of bounds
				if ( lastframe->n > MAX_BAD_FIRST - 2 ) { mp3_discard_frame( frame ); break; }
				else n_bad_first = lastframe->n + 2;
			} else if ( lastframe->aux_size < frame->bit_reservoir ) { // overlapping main data
				if ( lastframe->n > MAX_BAD_FIRST - 1 ) { mp3_discard_frame( frame ); break; }
				else n_bad_first = lastframe->n + 1;
			} else if ( frame->n == -1 ) { // obvious contradiction (see mp3_read_frame())
				if ( lastframe->n > MAX_BAD_FIRST - 2 ) { mp3_discard_frame( frame ); break; }
				else n_bad_first = lastframe->n + 2;
			}
			// fix previous frame aux size
			lastframe->aux_size -= frame->bit_reservoir;
		} else if ( ( frame->aux_size < 0 ) || ( frame->n == -1 ) ) n_bad_first = 1;
		// update main index, store main data
		frame->main_index = data_writer->getpos() - frame->bit_reservoir;
		data_writer->write_n( mp3data + pos + frame->fixed_size, frame->frame_size - frame->fixed_size );
		// insert frame into the frame chain
		mp3_append_frame( frame );
		// check for pointing to empty space
		if ( frame->main_index < 0 ) n_bad_first = frame->n + 1;
		// advance to next physical frame
		pos += frame->frame_size;
	}
	
	// store main data
	main_data_size = data_writer->getpos();
	if ( main_data_size > 0 ) main_data = data_writer->getptr();
	delete( data_writer );
	
	// check number of proper frames (must be at least 5)
	if ( lastframe->n + 1 - n_bad_first < 5  ) {
		snprintf( errormessage, MSG_SIZE, "corrupted file, compression not possible" );
		errorlevel = 2;
		return false;
	}
	
	
	// end of main mp3 data processing
	main_data_end = pos;
	// check for ID3 tags at end of file
	if ( !incomplete_last_frame && ( main_data_end < mp3filesize ) ) {
		if ( main_data_end + mp3_get_id3_size( mp3data+main_data_end, mp3filesize-main_data_end ) != mp3filesize ) {
			// allow for a tolerance of 64K unidentified garbage data at EOF
			if ( mp3filesize - main_data_end > GARBAGE_TOLERANCE ) {
				snprintf( errormessage, MSG_SIZE, "synching failure (frame #%i at 0x%X)", lastframe->n + 1, pos );
				errorlevel = 2;
				free( mp3data );
				return false;
			}/* else { // (!!!) strict mode?
				snprintf( errormessage, MSG_SIZE, "%i byte of unidentified garbage after EOF", mp3filesize - main_data_end );
				errorlevel = 1;
			}*/
		}
	}
	
	// clean up and store id3 tags and garbage data
	data_after_size = mp3filesize - main_data_end;
	data_before_size = main_data_begin;
#if !defined(BUILD_LIB)
	// v1.2 -d (disc_meta): drop any leading/trailing non-frame bytes
	// (typically ID3v2 / ID3v1 / garbage). Roundtrip becomes irreversible
	// — user opted in via the flag.
	if ( disc_meta ) {
		data_before_size = 0;
		data_after_size  = 0;
	}
#endif
	if ( data_after_size > 0 ) {
		data_after = (unsigned char*) calloc( data_after_size, sizeof( char ) );
		if ( ( mp3data == NULL ) || ( mp3filesize <= 0 ) ) {
			snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
			errorlevel = 2;
			return false;
		}
		memcpy( data_after, mp3data + main_data_end, data_after_size );
	}
	if ( data_before_size > 0 )
		data_before = (unsigned char*) frealloc( mp3data, data_before_size * sizeof(char) );
	else free( mp3data );
	
	
	return true;
}


/* -----------------------------------------------
	write MP3 from frame structure
	----------------------------------------------- */

INTERN bool write_mp3( void )
{
	unsigned char* data = main_data;
	unsigned char* fixed;
	mp3Frame* frame;
	
	
	// write data before (usually ID3v2 tag)
	str_out->write( data_before, 1, data_before_size );
	
	// write physical frames
	// main data has to be in order!
	for ( frame = firstframe; frame != NULL; frame = frame->next ) {
		fixed = mp3_build_fixed( frame );
		str_out->write( fixed, 1, frame->fixed_size );
		str_out->write( data, 1, frame->frame_size - frame->fixed_size );
		data += frame->frame_size - frame->fixed_size;
	}
	
	// write data after
	str_out->write( data_after, 1, data_after_size );
	
	// errormessage if write error
	if ( str_out->chkerr() ) {
		snprintf( errormessage, MSG_SIZE, "write error, possibly drive is full" );
		errorlevel = 2;		
		return false;
	}
	
	// get filesize
	mp3filesize = str_out->getsize();
	
	
	return true;
}


/* -----------------------------------------------
	analyse frames for redundancies and errors
	----------------------------------------------- */

INTERN bool analyze_frames( void )
{
	mp3Frame* frame;
	granuleInfo* granule;
	bool aux0 = false;
	bool aux1 = false;
	bool sbx_gain = false;
	int nch;
	int ch, gr;
	
	
	// (re)set analysis data
	i_mpeg			= firstframe->mpeg;
	i_layer			= firstframe->layer;
	i_samplerate	= firstframe->samples;
	i_bitrate		= firstframe->bits;
	i_protection	= firstframe->protection;
	i_padding		= firstframe->padding;
	i_privbit		= firstframe->privbit;
	i_channels		= firstframe->channels;
	i_stereo_ms		= firstframe->stereo_ms;
	i_stereo_int	= firstframe->stereo_int;
	i_copyright		= firstframe->copyright;
	i_original		= firstframe->original;
	i_emphasis		= firstframe->emphasis;
	i_padbits		= firstframe->padbits;
	i_bit_res = ( firstframe->bit_reservoir > 0 ) ? 1 : 0;
	if ( firstframe->aux_size > 0 ) {
		i_aux_h = 1;
		aux0 = true;
	} else i_aux_h = 0;
	// for granules
	granule = firstframe->granules[0][0];
	i_share = ( granule->share != 0 ) ? 1 : 0;
	i_sblocks = ( granule->window_switching != 0 ) ? 1 : 0;
	i_mixed = ( granule->mixed_flag != 0 ) ? 1 : 0;
	i_preemphasis = granule->preemphasis;
	i_coarse = granule->coarse_scalefactors;
	// easy way out for these - will be tested later
	i_sb_diff = 0;
	i_sbgain = 0;
	
	
	// analyse frames, check for redundancies and errors
	for ( frame = firstframe; frame != NULL; frame = frame->next ) {
		// no need to check mpeg, layer, samples, channels
		// already did this in read_mp3()!
		
		// analyse (mainly) frame header data
		// special tolerance for the following, as they often get mixed up in frame #0
		if ( ( i_privbit != -1 ) && ( i_privbit != frame->privbit ) ) {
			if ( frame->n == 1 ) { // tolerance for first frame
				if ( n_bad_first == 0 ) n_bad_first = 1;
				i_privbit = frame->privbit;
			} else i_privbit = -1;
		}		
		if ( ( i_copyright != -1 ) && ( i_copyright != frame->copyright ) ) {
			if ( frame->n == 1 ) { // tolerance for first frame
				if ( n_bad_first == 0 ) n_bad_first = 1;
				i_copyright = frame->copyright;
			} else i_copyright = -1;
		}
		if ( ( i_original != -1 ) && ( i_original != frame->original ) ) {
			if ( frame->n == 1 ) { // tolerance for first frame
				if ( n_bad_first == 0 ) n_bad_first = 1;
				i_original = frame->original;
			} else i_original = -1;
		}
		if ( ( i_protection != -1 ) && ( i_protection != frame->protection ) ) i_protection = -1;
		if ( ( i_bitrate != -1 ) && ( i_bitrate != frame->bits ) ) i_bitrate = -1;
		if ( ( i_padding != -1 ) && ( i_padding != frame->padding ) ) i_padding = -1;
		if ( ( i_stereo_ms != -1 ) && ( i_stereo_ms != frame->stereo_ms ) ) i_stereo_ms = -1;
		if ( ( i_stereo_int != -1 ) && ( i_stereo_int != frame->stereo_int ) ) i_stereo_int = -1;
		if ( ( i_emphasis != -1 ) && ( i_emphasis != frame->emphasis ) ) i_emphasis = -1;
		if ( ( i_padbits != -1 ) && ( i_padbits != frame->padbits ) ) i_padbits = -1;
		if ( ( i_bit_res != 1 ) && ( frame->bit_reservoir > 0 ) ) i_bit_res = 1;
		if ( i_aux_h != -1 ) {
			// no auxiliary before aux0 or after aux1 
			if  ( frame->aux_size > 0 ) {
				if ( !aux0 ) aux1 = true;
			} else {
				if ( aux1 ) i_aux_h = -1;
				if ( aux0 ) aux0 = false;
			}
		}
		nch = frame->nchannels;
		for ( ch = 0; ch < nch; ch++ ) {
			for ( gr = 0; gr < mp3_ngr( frame->mpeg ); gr++ ) {
				granule = frame->granules[ch][gr];
				sbx_gain =
					( granule->sb_gain[0] != 0 ) ||
					( granule->sb_gain[1] != 0 ) ||
					( granule->sb_gain[2] != 0 );
				if ( ( i_share != 1 ) && ( granule->share != 0 ) ) i_share = 1;
				if ( ( i_sblocks != 1 ) && ( granule->window_switching ) ) i_sblocks = 1;
				if ( ( i_mixed != 1 ) && ( granule->mixed_flag ) ) i_mixed = 1;
				if ( ( i_preemphasis != -1 ) && ( i_preemphasis != granule->preemphasis ) ) i_preemphasis = -1;
				if ( ( i_coarse != -1 ) && ( i_coarse != granule->coarse_scalefactors ) ) i_coarse = -1;
				if ( ( i_sbgain != -1 ) && ( sbx_gain ) ) i_sbgain = ( granule->block_type == SHORT_BLOCK ) ? 1 : -1;
				if ( ( i_sb_diff != 1 ) && ( ch == 1 ) )
					if ( granule->block_type != frame->granules[0][gr]->block_type ) i_sb_diff = 1;
			}
		}
	}
	
	// last check for aux data handling
	if ( ( i_aux_h != -1 ) && ( aux1 ) ) i_aux_h = 1;
	
	// fill in some global info
	g_nframes = lastframe->n + 1;
	g_nchannels = ( i_channels == MP3_MONO ) ? 1 : 2;  // number of channels
	g_samplerate = samplerate_table[(int)i_mpeg][(int)i_samplerate];  // sample rate (per MPEG version)
	g_bitrate = ( i_bitrate == -1 ) ? 0 : bitrate_table[(int)i_mpeg][LAYER_III][(int)i_bitrate]; // bit rate
	
	
	return true;
}


INTERN bool compress_mp3( void )
{
	aricoder* encoder;
	
	
	// --- check for incompatibilities and problems ---
	
	// unsupported stuff -> not recoverable
	if ( i_protection == -1 ) { // inconsistent use of checksums
		snprintf( errormessage, MSG_SIZE, "inconsistent use of checksums, not supported" );
		errorlevel = 2;
		return false;
	}
	if ( i_stereo_int == -1 ) { // inconsistent use of intensity stereo
		snprintf( errormessage, MSG_SIZE, "inconsistent use of int stereo, not supported" );
		errorlevel = 2;
		return false;
	}
	if ( i_emphasis == -1 ) { // inconsistent use of emphasis
		snprintf( errormessage, MSG_SIZE, "inconsistent use of emphasis, not supported" );
		errorlevel = 2;
		return false;
	}
	if ( i_mixed != 0x0 ) { // mixed blocks
		snprintf( errormessage, MSG_SIZE, "mixed blocks used, not supported" );
		errorlevel = 2;
		return false;
	}
	
	// inconsistencies -> recoverable
	if ( i_privbit == -1 ) { // inconsistent private bit -> steganograhy?
		snprintf( errormessage, MSG_SIZE, "inconsistent private bit" );
		errorlevel = 1;
		i_privbit = 0;
	}
	if ( i_copyright == -1 ) { // inconsistent copyright bit -> steganograhy?
		snprintf( errormessage, MSG_SIZE, "inconsistent copyright bit" );
		errorlevel = 1;
		i_copyright = 0;
	}
	if ( i_original == -1 ) { // inconsistent original bit -> steganograhy?
		snprintf( errormessage, MSG_SIZE, "inconsistent original bit" );
		errorlevel = 1;
		i_original = 1;
	}
	if ( i_padbits != 0 ) { // non-zero padbits -> steganograhy?
		snprintf( errormessage, MSG_SIZE, "non-zero padbits found" );
		errorlevel = 1;
		i_padbits = 0;
	}
	
	
	// --- try recompressing an embedded cover (sets apic_present et al, must
	// happen before pmp_write_header so the header bit reflects it) ---

	#if !defined(BUILD_LIB) && !defined(STORE_ID3)
	unsigned char* apic_modified = NULL; int apic_modified_size = 0;
	if ( data_before_size > 0 )
		apic_try_recompress( &apic_modified, &apic_modified_size );
	#endif


	// --- write PMP header with some basic info ---

	// PMP magic number & version byte
	str_out->write( (void*) pmp_magic, 1, 2 );
	str_out->write( (void*) &appversion, 1, 1 );

	// PMP header data
	if ( !pmp_write_header( str_out ) ) return false;

	#if !defined(BUILD_LIB) && !defined(STORE_ID3)
	// v2.1: raw recipe record (offset/orig_len/pjg_len), immediately after
	// the header and before the bad-first-frames unmute block -- order is
	// significant and must match uncompress_pmp exactly.
	if ( apic_present ) {
		unsigned char rec[12];
		rec[0]  = (unsigned char)( apic_offset        ); rec[1]  = (unsigned char)( apic_offset   >>  8 );
		rec[2]  = (unsigned char)( apic_offset   >> 16 ); rec[3]  = (unsigned char)( apic_offset   >> 24 );
		rec[4]  = (unsigned char)( apic_orig_len      ); rec[5]  = (unsigned char)( apic_orig_len >>  8 );
		rec[6]  = (unsigned char)( apic_orig_len >> 16 ); rec[7]  = (unsigned char)( apic_orig_len >> 24 );
		rec[8]  = (unsigned char)( apic_pjg_len       ); rec[9]  = (unsigned char)( apic_pjg_len  >>  8 );
		rec[10] = (unsigned char)( apic_pjg_len  >> 16 ); rec[11] = (unsigned char)( apic_pjg_len  >> 24 );
		str_out->write( (void*) rec, 1, 12 );
	}
	#endif


	#if defined( STORE_ID3 )
	// --- store ID3 data instead of compressing ---

	if ( data_before_size > 0 )
		if ( pmp_store_data( str_out, data_before, data_before_size ) != data_before_size ) return false;
	if ( data_after_size > 0 )
		if ( pmp_store_data( str_out, data_after, data_after_size ) != data_after_size ) return false;
	#endif


	// --- mute frames, store fix data (only if broken) ---

	if ( n_bad_first > 0 ) {
		// mute all frames up to last bad
		for ( mp3Frame* frame = firstframe; n_bad_first > 0; frame = frame->next, n_bad_first-- )
			mp3_mute_frame( frame );
		// store fix data
		if ( !pmp_store_unmute_data( str_out ) ) return false;
	}


	// --- actual compressed data writing starts here ---

	// init arithmetic compression
	encoder = new aricoder( str_out, 1 );

	#if !defined( STORE_ID3 )
	// id3 tags / other tags / garbage -- if a cover was recompressed, encode
	// the modified (shrunk) copy instead of the original data_before; swap
	// back immediately after so nothing downstream ever sees the swap.
	if ( ( data_before_size > 0 ) || ( data_after_size > 0 ) ) {
		#if !defined(BUILD_LIB)
		unsigned char* real_data_before = data_before; int real_data_before_size = data_before_size;
		if ( apic_present ) { data_before = apic_modified; data_before_size = apic_modified_size; }
		#endif
		bool id3_ok = pmp_encode_id3( encoder );
		#if !defined(BUILD_LIB)
		if ( apic_present ) { free( data_before ); data_before = real_data_before; data_before_size = real_data_before_size; }
		#endif
		if ( !id3_ok ) return false;
	}
	#endif

	// frame header data
	if ( i_padding != 0 ) // padding bits
		if ( !pmp_encode_padding( encoder ) ) return false;
	if ( i_sblocks != 0 ) // special block types
		if ( !pmp_encode_block_types( encoder ) ) return false;
	// global gain
	if ( !pmp_encode_global_gain( encoder ) ) return false;
	// build context from global gain
	if ( !pmp_build_context() ) return false;
	// slength
	if ( !pmp_encode_slength( encoder ) ) return false;
	/*
	// region sizes
	if ( !pmp_encode_region_bounds( encoder ) ) return false;
	// huffman table selections
	if ( !pmp_encode_htable_selection( encoder ) ) return false;
	*/
	if ( !pmp_encode_region_data( encoder ) ) return false;
	if ( i_share != 0 ) // scalefactor sharing
		if ( !pmp_encode_sharing( encoder ) ) return false;
	if ( i_preemphasis != 0 ) // preemphasis
		if ( !pmp_encode_preemphasis( encoder ) ) return false;
	if ( i_coarse != 0 ) // coarse setting
		if ( !pmp_encode_coarse_sf( encoder ) ) return false;
	if ( i_sbgain != 0 ) // subblock gain
		if ( !pmp_encode_subblock_gain( encoder ) ) return false;
	if ( i_stereo_ms != 0 ) // ms stereo settings
		if ( !pmp_encode_stereo_ms( encoder ) ) return false;
	// main data
	if ( !pmp_encode_main_data( encoder ) ) return false;
	
	// finalize arithmetic compression
	delete( encoder );
	
	
	// --- final checks ---
	
	// errormessage if write error
	if ( str_out->chkerr() ) {
		snprintf( errormessage, MSG_SIZE, "write error, possibly drive is full" );
		errorlevel = 2;		
		return false;
	}
	
	// get filesize
	pmpfilesize = str_out->getsize();
	
	
	return true;
}


INTERN bool uncompress_pmp( void )
{
	aricoder* decoder;
	unsigned char hcode;
	
	
	// --- no error checks needed! ---
	// (we already did this when compressing)
	
	
	// --- read PMP header and analyse basic info ---
	
	// PMP magic number
	// skip 2 bytes (no need to check again)
	str_in->read( &hcode, 1, 1 );
	str_in->read( &hcode, 1, 1 );
	
	// version number
	str_in->read( &hcode, 1, 1 );
	// v1.2: accept v1.1 (byte 0x0B) and v1.2 (byte 0x0C) archives.
	// Format payload after the version byte is unchanged across the bump.
	if ( hcode != appversion && hcode < appversion_legacy_min ) {
		snprintf( errormessage, MSG_SIZE, "incompatible file, use %s v%i.%i",
			appname, hcode / 10, hcode % 10 );
		errorlevel = 2;
		return false;
	}
	if ( hcode > appversion ) {
		snprintf( errormessage, MSG_SIZE, "file from a newer %s build (v%i.%i); upgrade to decode",
			appname, hcode / 10, hcode % 10 );
		errorlevel = 2;
		return false;
	}
	// remember the archive's version so pmp_read_header knows which format
	// fields are present (e.g. MPEG-version bits exist only from v1.3 on).
	pmp_archive_version = hcode;

	// read and analyse header
	if ( !pmp_read_header( str_in ) ) return false;

	#if !defined(BUILD_LIB) && !defined(STORE_ID3)
	// v2.1: raw recipe record, immediately after the header and before the
	// bad-first-frames unmute block -- must match compress_mp3 exactly.
	if ( apic_present ) {
		unsigned char rec[12];
		if ( str_in->read( rec, 1, 12 ) != 12 ) {
			snprintf( errormessage, MSG_SIZE, "truncated archive (APIC record)" );
			errorlevel = 2;
			return false;
		}
		apic_offset   = (int)( rec[0] | (rec[1]<<8) | (rec[2]<<16) | (rec[3]<<24) );
		apic_orig_len = (int)( rec[4] | (rec[5]<<8) | (rec[6]<<16) | (rec[7]<<24) );
		apic_pjg_len  = (int)( rec[8] | (rec[9]<<8) | (rec[10]<<16) | (rec[11]<<24) );
	}
	#endif


	#if defined( STORE_ID3 )
	// --- unstore ID3 data ---

	if ( data_before_size > 0 ) {
		data_before_size = pmp_unstore_data( str_in, &data_before );
		if ( data_before_size <= 0 ) return false;
	}
	if ( data_after_size > 0 ) {
		data_after_size = pmp_unstore_data( str_in, &data_after );
		if ( data_after_size <= 0 ) return false;
	}
	#endif


	// --- unstore unmute fix data for later use (only if needed) ---
	
	// unstore fix data
	if ( n_bad_first > 0 ) {
		if ( !pmp_unstore_unmute_data( str_in ) ) return false;
		n_bad_first = 0; // not a beautiful solution
	}
	
	
	// --- actual decompression starts here ---
	
	// init arithmetic decompression
	decoder = new aricoder( str_in, 0 );
	
	#if !defined( STORE_ID3 )
	// id3 tags / other tags / garbage
	if ( ( data_before_size > 0 ) || ( data_after_size > 0 ) )
		if ( !pmp_decode_id3( decoder ) ) return false;
	#if !defined(BUILD_LIB)
	// splice the original JPEG back in, undoing apic_try_recompress
	if ( apic_present )
		if ( !apic_reconstruct() ) return false;
	#endif
	#endif

	// frame header data
	if ( i_padding != 0 ) // padding bits
		if ( !pmp_decode_padding( decoder ) ) return false;
	if ( i_sblocks != 0 ) // special block types
		if ( !pmp_decode_block_types( decoder ) ) return false;
	// global gain
	if ( !pmp_decode_global_gain( decoder ) ) return false;
	// build context from global gain
	if ( !pmp_build_context() ) return false;
	// slength
	if ( !pmp_decode_slength( decoder ) ) return false;
	/*
	// region sizes
	if ( !pmp_decode_region_bounds( decoder ) ) return false;
	// huffman table selections
	if ( !pmp_decode_htable_selection( decoder ) ) return false;
	*/
	if ( !pmp_decode_region_data( decoder ) ) return false;
	if ( i_share != 0 ) // scalefactor sharing
		if ( !pmp_decode_sharing( decoder ) ) return false;
	if ( i_preemphasis != 0 ) // preemphasis
		if ( !pmp_decode_preemphasis( decoder ) ) return false;
	if ( i_coarse != 0 ) // coarse setting
		if ( !pmp_decode_coarse_sf( decoder ) ) return false;
	if ( i_sbgain != 0 ) // subblock gain
		if ( !pmp_decode_subblock_gain( decoder ) ) return false;
	if ( i_stereo_ms != 0 ) // ms stereo settings
		if ( !pmp_decode_stereo_ms( decoder ) ) return false;
	// main data
	if ( !pmp_decode_main_data( decoder ) ) return false;
	
	// finalize arithmetic compression
	delete( decoder );
	
	
	// --- unmute frames and reverse to bad (but bitwise identical) state ---
	
	if ( unmute_data_size > 0 ) // unmute all frames up to # found in unmute data first byte
		for ( mp3Frame* frame = firstframe; n_bad_first < *unmute_data; frame = frame->next, n_bad_first++ )
			mp3_unmute_frame( frame );
	
			
	// --- final checks ---
	
	// get filesize
	pmpfilesize = str_in->getsize();


	return true;
}


/* -----------------------------------------------
	intra-file parallel chunking (-k): a file is split at frame boundaries into
	K byte ranges, each compressed/decompressed independently (its own arithmetic
	stream + fresh models), so they can run on separate threads. The "MK" container
	holds K self-contained "MS" sub-streams. K=1 keeps the original single stream.
	Cost: each chunk restarts the models (a small ratio hit at the boundary); the
	chunks tile the file exactly, so concatenation is bit-identical to the input.
	----------------------------------------------- */

INTERN bool compress_mp3_chunked( void )
{
	iostream* real_in  = str_in;
	iostream* real_out = str_out;

	int fsize = real_in->getsize();
	if ( fsize <= 0 ) { snprintf( errormessage, MSG_SIZE, "empty input" ); errorlevel = 2; return false; }
	unsigned char* d = (unsigned char*) malloc( fsize );
	if ( d == NULL ) { snprintf( errormessage, MSG_SIZE, MEM_ERRMSG ); errorlevel = 2; return false; }
	real_in->rewind();
	real_in->read( d, 1, fsize );

	// --- find frame-aligned cut points: cuts[0]=0 .. cuts[nch]=fsize ---
	int K = num_chunks; if ( K > MAX_CHUNKS ) K = MAX_CHUNKS;
	int cuts[ MAX_CHUNKS + 1 ]; int nch = 0;
	{
		int first = mp3_seek_firstframe( d, fsize );
		if ( first >= 0 ) {
			int save_err = errorlevel; char save_msg[ MSG_SIZE ]; memcpy( save_msg, errormessage, MSG_SIZE );
			std::vector<int> starts;
			int pos = first;
			while ( pos + 4 <= fsize ) {
				mp3Frame* f = mp3_read_frame( d + pos, fsize - pos );
				if ( f == NULL ) break;
				int fb = f->frame_size;
				mp3_discard_frame( f );
				if ( fb <= 0 || pos + fb > fsize ) break;
				starts.push_back( pos );
				pos += fb;
			}
			errorlevel = save_err; memcpy( errormessage, save_msg, MSG_SIZE );	// scan is side-effect free
			int nf = (int) starts.size();
			if ( nf >= 2 * MIN_FRAMES_CHUNK ) {
				if ( K > nf / MIN_FRAMES_CHUNK ) K = nf / MIN_FRAMES_CHUNK;
				if ( K > 1 ) {
					cuts[0] = 0;										// leading junk -> chunk 0
					for ( int g = 1; g < K; g++ )
						cuts[g] = starts[ (int) ( (long long) g * nf / K ) ];
					cuts[K] = fsize;									// trailing junk -> last chunk
					nch = K;
				}
			}
		}
	}

	// --- not worth splitting: single serial stream (original behaviour) ---
	if ( nch <= 1 ) {
		free( d );
		str_in = real_in; str_out = real_out; real_in->rewind();
		reset_buffers();
		if ( !read_mp3() ) return false;
		if ( !analyze_frames() ) return false;
		return compress_mp3();
	}

	// --- compress each chunk into its own memory buffer ---
	std::vector<unsigned char*> bufs( nch, NULL );
	std::vector<int> sizes( nch, 0 );
	std::atomic<bool> all_ok( true );

	auto do_chunk = [&] ( int i ) {
		str_in  = new iostream( d + cuts[i], 1, cuts[i+1] - cuts[i], 0 );
		str_out = new iostream( NULL, 1, 0, 1 );
		reset_buffers();
		bool ok = read_mp3();
		if ( ok ) ok = analyze_frames();
		if ( ok ) ok = compress_mp3();
		if ( ok ) {
			int s = str_out->getsize();
			unsigned char* p = (unsigned char*) malloc( s );
			if ( p != NULL ) { memcpy( p, str_out->getptr(), s ); bufs[i] = p; sizes[i] = s; }
			else ok = false;
		}
		if ( !ok ) all_ok = false;
		delete( str_in ); delete( str_out ); str_in = NULL; str_out = NULL;
		reset_buffers();
	};

	// parallel only when a batch (-th) isn't already saturating the cores
	if ( num_threads <= 1 && nch > 1 ) {
		std::atomic<int> next( 0 );
		std::vector<std::thread> pool; pool.reserve( nch );
		auto worker = [&] () { int i; while ( ( i = next.fetch_add( 1 ) ) < nch ) do_chunk( i ); };
		for ( int t = 0; t < nch; t++ ) pool.emplace_back( worker );
		for ( auto& t : pool ) t.join();
	} else {
		for ( int i = 0; i < nch; i++ ) do_chunk( i );
	}

	str_in = real_in; str_out = real_out;
	reset_buffers();

	// --- on any chunk failure, fall back to a single serial stream ---
	if ( !all_ok ) {
		for ( int i = 0; i < nch; i++ ) if ( bufs[i] ) free( bufs[i] );
		free( d ); real_in->rewind(); reset_buffers();
		if ( !read_mp3() ) return false;
		if ( !analyze_frames() ) return false;
		return compress_mp3();
	}

	// --- assemble "MK" container: magic, ver, K, K x uint32 sizes, sub-streams ---
	int hdr = 2 + 1 + 1 + 4 * nch;
	long long total = hdr; for ( int i = 0; i < nch; i++ ) total += sizes[i];
	unsigned char* arch = (unsigned char*) malloc( (size_t) total );
	int o = 0;
	arch[o++] = pmc_magic[0]; arch[o++] = pmc_magic[1];
	arch[o++] = appversion;   arch[o++] = (unsigned char) nch;
	for ( int i = 0; i < nch; i++ ) {
		unsigned int s = (unsigned int) sizes[i];
		arch[o++] = s & 0xFF; arch[o++] = (s>>8) & 0xFF; arch[o++] = (s>>16) & 0xFF; arch[o++] = (s>>24) & 0xFF;
	}
	for ( int i = 0; i < nch; i++ ) { memcpy( arch + o, bufs[i], sizes[i] ); o += sizes[i]; free( bufs[i] ); }

	// --- opt-in self-verify (mp3 convention): decode container, compare to input ---
	bool ok = true;
	if ( verify_lv > 0 ) {
		int save_err = errorlevel; char save_msg[ MSG_SIZE ]; memcpy( save_msg, errormessage, MSG_SIZE );
		str_in  = new iostream( arch, 1, (int) total, 0 );
		str_out = new iostream( NULL, 1, 0, 1 );
		reset_buffers();
		ok = uncompress_pmp_chunked();
		if ( ok ) ok = ( str_out->getsize() == fsize ) && ( memcmp( str_out->getptr(), d, fsize ) == 0 );
		delete( str_in ); delete( str_out );
		str_in = real_in; str_out = real_out;
		reset_buffers();
		errorlevel = save_err; memcpy( errormessage, save_msg, MSG_SIZE );
	}

	if ( ok ) {
		real_out->write( arch, 1, (int) total );
		free( arch ); free( d );
		mp3filesize = fsize;
		pmpfilesize = real_out->getsize();
		return true;
	}

	// self-verify failed -> safe single-stream fallback
	free( arch ); free( d ); real_in->rewind(); reset_buffers();
	if ( !read_mp3() ) return false;
	if ( !analyze_frames() ) return false;
	return compress_mp3();
}

INTERN bool uncompress_pmp_chunked( void )
{
	iostream* real_in  = str_in;
	iostream* real_out = str_out;

	int asize = real_in->getsize();
	if ( asize < 5 ) { snprintf( errormessage, MSG_SIZE, "corrupt chunked archive" ); errorlevel = 2; return false; }
	unsigned char* a = (unsigned char*) malloc( asize );
	if ( a == NULL ) { snprintf( errormessage, MSG_SIZE, MEM_ERRMSG ); errorlevel = 2; return false; }
	real_in->rewind();
	real_in->read( a, 1, asize );

	int nch = a[3];
	if ( nch < 1 || nch > MAX_CHUNKS ) { snprintf( errormessage, MSG_SIZE, "corrupt chunked archive" ); errorlevel = 2; free( a ); return false; }
	int o = 4; long long sum = 0; int sizes[ MAX_CHUNKS ];
	for ( int i = 0; i < nch; i++ ) {
		sizes[i] = a[o] | (a[o+1]<<8) | (a[o+2]<<16) | (a[o+3]<<24); o += 4; sum += sizes[i];
		if ( sizes[i] <= 0 ) { snprintf( errormessage, MSG_SIZE, "corrupt chunked archive" ); errorlevel = 2; free( a ); return false; }
	}
	if ( (long long) o + sum != asize ) { snprintf( errormessage, MSG_SIZE, "corrupt chunked archive" ); errorlevel = 2; free( a ); return false; }
	int offs[ MAX_CHUNKS ]; { int p = o; for ( int i = 0; i < nch; i++ ) { offs[i] = p; p += sizes[i]; } }

	std::vector<unsigned char*> bufs( nch, NULL );
	std::vector<int> bsz( nch, 0 );
	std::atomic<bool> all_ok( true );

	auto do_chunk = [&] ( int i ) {
		str_in  = new iostream( a + offs[i], 1, sizes[i], 0 );
		str_out = new iostream( NULL, 1, 0, 1 );
		reset_buffers();
		bool ok = uncompress_pmp();
		if ( ok ) ok = write_mp3();
		if ( ok ) {
			int s = str_out->getsize();
			unsigned char* q = (unsigned char*) malloc( s );
			if ( q != NULL ) { memcpy( q, str_out->getptr(), s ); bufs[i] = q; bsz[i] = s; }
			else ok = false;
		}
		if ( !ok ) all_ok = false;
		delete( str_in ); delete( str_out ); str_in = NULL; str_out = NULL;
		reset_buffers();
	};

	if ( num_threads <= 1 && nch > 1 ) {
		std::atomic<int> next( 0 );
		std::vector<std::thread> pool; pool.reserve( nch );
		auto worker = [&] () { int i; while ( ( i = next.fetch_add( 1 ) ) < nch ) do_chunk( i ); };
		for ( int t = 0; t < nch; t++ ) pool.emplace_back( worker );
		for ( auto& t : pool ) t.join();
	} else {
		for ( int i = 0; i < nch; i++ ) do_chunk( i );
	}

	str_in = real_in; str_out = real_out;
	reset_buffers();

	if ( !all_ok ) {
		for ( int i = 0; i < nch; i++ ) if ( bufs[i] ) free( bufs[i] );
		free( a );
		if ( errorlevel < 2 ) { snprintf( errormessage, MSG_SIZE, "chunk decode failed" ); errorlevel = 2; }
		return false;
	}

	for ( int i = 0; i < nch; i++ ) { real_out->write( bufs[i], 1, bsz[i] ); free( bufs[i] ); }
	free( a );
	mp3filesize = real_out->getsize();
	pmpfilesize = asize;
	return true;
}

/* ----------------------- End of main functions -------------------------- */

/* ----------------------- Begin of MP3 specific functions -------------------------- */


/* -----------------------------------------------
	granules per frame: MPEG-1 has 2, MPEG-2/2.5 (LSF) have 1
	----------------------------------------------- */
INTERN inline int mp3_ngr( int mpeg )
{
	return ( mpeg == MP3_V1_0 ) ? 2 : 1;
}


/* -----------------------------------------------
	Layer III frame size in bytes. MPEG-1 = 1152 samples/frame (factor 144),
	MPEG-2/2.5 LSF = 576 samples/frame (factor 72). The static frame_size_table
	is only valid for MPEG-1, so compute LSF sizes from the bitrate/samplerate.
	----------------------------------------------- */
INTERN inline int mp3_frame_bytes( int mpeg, int samples, int bits, int padding )
{
	if ( mpeg == MP3_V1_0 )
		return frame_size_table[ MP3_V1_0 ][ LAYER_III ][ samples ][ bits ] + ( padding ? 1 : 0 );
	int br = bitrate_table[ mpeg ][ LAYER_III ][ bits ];   // kbps
	int sr = samplerate_table[ mpeg ][ samples ];          // Hz
	if ( br <= 0 || sr <= 0 ) return 0;
	return ( 72 * br * 1000 ) / sr + ( padding ? 1 : 0 );
}


/* -----------------------------------------------
	MPEG-2/2.5 (LSF) scalefactor layout (ISO/IEC 13818-3).
	scalefac_compress (9 bits) -> partition bit-widths scfsz[4] and band
	counts scfcnt[4]; returns total band count. Algorithm verified vs minimp3.
	The right channel under intensity stereo uses a shifted table.
	----------------------------------------------- */
INTERN inline int mp3_lsf_scf_params( const mp3Frame* frame, const granuleInfo* granule,
                                      int ch, int scfsz[4], int scfcnt[4] )
{
	static const unsigned char g_mod[ 6*4 ] =
		{ 5,5,4,4, 5,5,4,1, 4,3,1,1, 5,6,6,1, 4,4,4,1, 4,3,1,1 };
	static const unsigned char g_scf_part[ 3 ][ 28 ] = {
		{ 6,5,5,5,6,5,5,5,6,5,7,3,11,10,0,0,7,7,7,0,6,6,6,3,8,8,5,0 },
		{ 8,9,6,12,6,9,9,9,6,9,12,6,15,18,0,0,6,15,12,0,6,12,9,6,6,18,9,0 },
		{ 9,9,6,12,9,9,9,9,9,9,12,6,18,18,0,0,12,12,12,0,12,9,9,6,15,12,9,0 }
	};
	int ist = ( ( frame->channels == MP3_JOINT_STEREO ) && frame->stereo_int && ( ch == 1 ) ) ? 1 : 0;
	int sfc = ( (int) granule->scalefac_compress ) >> ist;
	int kk = ist * 12, modprod, i, tot = 0;
	for ( ; sfc >= 0; sfc -= modprod, kk += 4 ) {
		modprod = 1;
		for ( i = 3; i >= 0; i-- ) { scfsz[ i ] = ( sfc / modprod ) % g_mod[ kk + i ]; modprod *= g_mod[ kk + i ]; }
	}
	int prow = ( granule->block_type == SHORT_BLOCK ) ? ( granule->mixed_flag ? 1 : 2 ) : 0;
	for ( i = 0; i < 4; i++ ) { scfcnt[ i ] = g_scf_part[ prow ][ kk + i ]; tot += scfcnt[ i ]; }
	return tot;
}


/* -----------------------------------------------
	read one physical frame
	----------------------------------------------- */
INTERN inline mp3Frame* mp3_read_frame( unsigned char* data, int max_size )
{
	mp3Frame* frame;
	granuleInfo* granule;
	abitreader* side_reader;
	unsigned char* header;
	unsigned char* sideinfo;
	unsigned short crc;
	int nsb = 0;
	int nch = 0;
	int ngr = 2;
	int ch, gr;


	// immediately return if not enough data (min size for header and side info = 21)
	if ( max_size < 21 ) return NULL;

	// --- frame header ---
	header = data + 0;
	
	// check syncword + Layer III (any MPEG version: 1, 2 or 2.5).
	// Reject: missing 11-bit sync, reserved MPEG version (0b01), or non-Layer-III.
	// (might be end of audio data -> handled by the caller)
	if ( ( header[0] != 0xFF ) ||
	     ( ( header[1] & 0xE0 ) != 0xE0 ) ||
	     ( ( (header[1]>>3) & 0x3 ) == 0x1 ) ||
	     ( ( (header[1]>>1) & 0x3 ) != LAYER_III ) ) {
		return NULL;
	}
	
	// alloc memory for frame
	frame = (mp3Frame*) calloc( 1, sizeof( mp3Frame ) );
	if ( frame == NULL ) {
		snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
		errorlevel = 2;
		return NULL;
	}
	
	// preset pointers
	frame->granules = NULL;
	frame->prev = NULL;
	frame->next = NULL;
	frame->n = 0;
	
	// extract data
	frame->mpeg       = (header[1]>>3)&0x3;
	frame->layer      = (header[1]>>1)&0x3;
	frame->protection = ((header[1]>>0)&0x1)^1;
	frame->bits       = (header[2]>>4)&0xF;
	frame->samples    = (header[2]>>2)&0x3;
	frame->padding    = (header[2]>>1)&0x1;
	frame->privbit    = (header[2]>>0)&0x1;
	frame->channels   = (header[3]>>6)&0x3;
	frame->stereo_ms  = (header[3]>>5)&0x1;
	frame->stereo_int = (header[3]>>4)&0x1;
	frame->copyright  = (header[3]>>3)&0x1;
	frame->original   = (header[3]>>2)&0x1;
	frame->emphasis   = (header[3]>>0)&0x3;
	
	// check data for problems
	// this also contains the never used free form stream flag -> so what?
	if ( ( frame->bits == 0x0 ) || ( frame->bits == 0xF ) || ( frame->samples == 0x3 ) ) {
		free( frame );
		return NULL;
	}
	
	// number of channels
	frame->nchannels = ( frame->channels == MP3_MONO ) ? 1 : 2;
	nch = frame->nchannels;

	// alloc memory for granules (MPEG-1: 2 per frame, MPEG-2/2.5 LSF: 1)
	ngr = mp3_ngr( frame->mpeg );
	frame->granules = (granuleInfo***) calloc( nch, sizeof( granuleInfo** ) );
	for ( ch = 0; ch < nch; ch++ ) {
		frame->granules[ch] = (granuleInfo**) calloc( ngr, sizeof( granuleInfo* ) );
		for ( gr = 0; gr < ngr; gr++ )
			frame->granules[ch][gr] = (granuleInfo*) calloc( 1, sizeof( granuleInfo ) );
	}
	
	// calculate size of frame (MPEG-1 via table, MPEG-2/2.5 via formula)
	frame->frame_size = mp3_frame_bytes( frame->mpeg, frame->samples, frame->bits, frame->padding );
	
	// this is the actual calculation routine, which is now no more used 
	/* if ( frame->layer != LAYER_I ) frame->frame_size = (int) ( frame->padding +
			( ( 144000 * bitrate_table[frame->mpeg][frame->layer][frame->bits] ) /
			samplerate_table[frame->mpeg][frame->samples] ) );
	else frame->frame_size = (int) ( ( ( frame->padding ) ? 4 : 0 ) +
			( ( 48000 * bitrate_table[frame->mpeg][frame->layer][frame->bits] ) /
			samplerate_table[frame->mpeg][frame->samples] ) );*/	

	// --- side information... ---
	// MPEG-2/2.5 (LSF) side info is half-size: 9/17 bytes vs MPEG-1's 17/32.
	const bool lsf = ( frame->mpeg != MP3_V1_0 );
	nsb = lsf ? ( ( nch == 1 ) ? 9 : 17 ) : ( ( nch == 1 ) ? 17 : 32 );
	frame->fixed_size = 4 + nsb + (frame->protection ? 2 : 0);
	// check if enough data is available
	if ( frame->fixed_size > max_size ) {
		mp3_discard_frame( frame );
		return NULL;
	}
	// --- ...and optional crc checksum ---
	if ( frame->protection == 0x1 ) {
		sideinfo = data + 6;
		// if there is a crc: check and discard
		crc = (data[4]<<8) + (data[5]<<0);
		if ( crc != mp3_calc_layer3_crc( header, sideinfo, nsb ) ) {
			snprintf( errormessage, MSG_SIZE, "crc checksum mismatch" );
			errorlevel = 1; // (!!!) careful - file might be broken
		}
	}
	else sideinfo = data + 4;
	side_reader = new abitreader( sideinfo, nsb );
	// frame global side info. LSF: main_data_begin is 8 bits (not 9), private
	// bits are 1/2 (not 5/3), and there is NO scfsi (only 1 granule).
	frame->bit_reservoir = (short) side_reader->read( lsf ? 8 : 9 );
	frame->padbits = (char) side_reader->read( lsf ? ( (nch==1) ? 1 : 2 ) : ( (nch==1) ? 5 : 3 ) );
	if ( !lsf ) {
		for ( ch = 0; ch < nch; ch++ ) {
			frame->granules[ch][0]->share = (char) side_reader->read( 4 );
			if ( ngr > 1 ) frame->granules[ch][1]->share = 0x0;
		}
	} else {
		for ( ch = 0; ch < nch; ch++ ) frame->granules[ch][0]->share = 0x0;
	}
	// granule specific side info
	for ( gr = 0; gr < ngr; gr++ ) {
		for ( ch = 0; ch < nch; ch++ ) {
			granule = frame->granules[ch][gr];
			granule->main_data_bit = (short) side_reader->read( 12 );
			granule->big_val_pairs = (short) side_reader->read( 9 );
			granule->global_gain = (short) side_reader->read( 8 );
			// LSF: scalefac_compress is 9 bits; MPEG-1: slength is 4 bits.
			if ( lsf ) { granule->scalefac_compress = (short) side_reader->read( 9 ); granule->slength = 0; }
			else       { granule->slength = (char) side_reader->read( 4 ); granule->scalefac_compress = 0; }
			granule->window_switching = (char) side_reader->read( 1 );
			if ( granule->window_switching == 0 ) { // for normal blocks
				granule->region_table[0] = (char) side_reader->read( 5 );
				granule->region_table[1] = (char) side_reader->read( 5 );
				granule->region_table[2] = (char) side_reader->read( 5 );
				granule->region0_size = (char) side_reader->read( 4 );
				granule->region1_size = (char) side_reader->read( 3 );
				if ( granule->region0_size+granule->region1_size > 20 ) {
					snprintf( errormessage, MSG_SIZE, "region size out of bounds" );
					errorlevel = 2;
					delete( side_reader );
					mp3_discard_frame( frame );
					return NULL;
				}
				granule->region_bound[0] =
					bandwidth_bounds[(int)frame->mpeg][(int)frame->samples][(int)granule->region0_size+1];
				granule->region_bound[1] =
					bandwidth_bounds[(int)frame->mpeg][(int)frame->samples][(int)granule->region0_size+granule->region1_size+2];
				granule->region_bound[2] = CLAMPED( 0, 576, granule->big_val_pairs << 1 );
				if ( granule->region_bound[0] > granule->region_bound[2] ) {
					granule->region_bound[0] = granule->region_bound[2];
					granule->region_bound[1] = granule->region_bound[2];
				} else if ( granule->region_bound[1] > granule->region_bound[2] )
					granule->region_bound[1] = granule->region_bound[2];
				granule->block_type = LONG_BLOCK;
				granule->mixed_flag = 0;
				granule->sb_gain[0] = 0;
				granule->sb_gain[1] = 0;
				granule->sb_gain[2] = 0;
			} else { // for special blocks
				granule->block_type = (char) side_reader->read( 2 );
				granule->mixed_flag = (char) side_reader->read( 1 );
				granule->region_table[0] = (char) side_reader->read( 5 );
				granule->region_table[1] = (char) side_reader->read( 5 );
				granule->sb_gain[0] = (char) side_reader->read( 3 );
				granule->sb_gain[1] = (char) side_reader->read( 3 );
				granule->sb_gain[2] = (char) side_reader->read( 3 );
				if ( granule->block_type != SHORT_BLOCK ) {
					// region sizes for different block types
					granule->region0_size = 8;
					granule->region1_size = 0;
					granule->region_bound[0] =
						bandwidth_bounds[(int)frame->mpeg][(int)frame->samples][8];
				} else { // special treatment for mixed blocks needed (!)
					granule->region0_size = 9;
					granule->region1_size = 0;
					granule->region_bound[0] =
						bandwidth_bounds_short[(int)frame->mpeg][(int)frame->samples][9/3] * 3;
				}
				granule->region_bound[1] = CLAMPED( 0, 576, granule->big_val_pairs << 1 );
				if ( granule->region_bound[0] > granule->region_bound[1] )
					granule->region_bound[0] = granule->region_bound[1];
				granule->region_bound[2] = granule->region_bound[1];
				granule->region_table[2] = 0;
			}
			// LSF has no preflag; MPEG-1 reads a 1-bit preflag here.
			granule->preemphasis = lsf ? 0 : (char) side_reader->read( 1 );
			granule->coarse_scalefactors = (char) side_reader->read( 1 );
			granule->select_htabB = (char) side_reader->read( 1 );
			granule->sv_bound = 0;
			// check for obvious problems/contradictions
			if ( granule->main_data_bit == 0 ) {
				if ( ( granule->big_val_pairs != 0 ) || ( granule->slength != 0 ) || ( granule->scalefac_compress != 0 ) )
					frame->n = -1; // mistreat frame number as trouble indicator :-)
			}
		}
	}
	delete( side_reader );
	
	// calculate total size of main (not aux) data (in byte)
	frame->main_bits = 0;
	for ( gr = 0; gr < ngr; gr++ )
		for ( ch = 0; ch < nch; ch++ )
			frame->main_bits += frame->granules[ch][gr]->main_data_bit;
	frame->main_size = (frame->main_bits+7)>>3;
	
	// calculate temporary size of aux data (in byte), subject to change
	frame->aux_size =
		frame->frame_size -
		frame->fixed_size +
		frame->bit_reservoir -
		frame->main_size;
	
	
	return frame;
}


/* -----------------------------------------------
	build frame from i_variables
	----------------------------------------------- */
INTERN inline mp3Frame* mp3_build_frame( void )
{
	mp3Frame* frame;
	granuleInfo* granule;
	int ch, gr;
	int nch;
	int ngr = mp3_ngr( i_mpeg );
	
	
	// alloc memory for frame
	frame = (mp3Frame*) calloc( 1, sizeof( mp3Frame ) );
	if ( frame == NULL ) {
		snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
		errorlevel = 2;
		return NULL;
	}
	
	// preset pointers
	frame->granules = NULL;
	frame->prev = NULL;
	frame->next = NULL;
	
	// fill in some info
	frame->mpeg		 		= i_mpeg;
	frame->layer 			= i_layer;
	frame->protection 		= i_protection;
	frame->bits 			= i_bitrate;
	frame->samples 			= i_samplerate;
	frame->padding			= i_padding;
	frame->privbit			= i_privbit;
	frame->channels			= i_channels;
	frame->stereo_ms		= i_stereo_ms;
	frame->stereo_int		= i_stereo_int;
	frame->copyright		= i_copyright;
	frame->original			= i_original;
	frame->emphasis			= i_emphasis;
	frame->bit_reservoir 	= 0;
	frame->padbits			= i_padbits;
	
	// stuff that is known right now (# channels and fixed size).
	// LSF side info is half-size: 9 (mono) / 17 (stereo) vs MPEG-1's 17 / 32.
	const bool lsf_bf = ( frame->mpeg != MP3_V1_0 );
	if ( frame->channels == MP3_MONO ) {
		int sib = lsf_bf ? 9 : 17;
		frame->fixed_size = ( frame->protection ) ? 4 + 2 + sib : 4 + sib;
		frame->nchannels = 1; nch = frame->nchannels;
	} else {
		int sib = lsf_bf ? 17 : 32;
		frame->fixed_size = ( frame->protection ) ? 4 + 2 + sib : 4 + sib;
		frame->nchannels = 2; nch = frame->nchannels;
	}
	
	// alloc memory for granules (MPEG-1: 2 per frame, MPEG-2/2.5 LSF: 1)
	frame->granules = (granuleInfo***) calloc( nch, sizeof( granuleInfo** ) );
	for ( ch = 0; ch < nch; ch++ ) {
		frame->granules[ch] = (granuleInfo**) calloc( ngr, sizeof( granuleInfo* ) );
		for ( gr = 0; gr < ngr; gr++ )
			frame->granules[ch][gr] = (granuleInfo*) calloc( 1, sizeof( granuleInfo ) );
	}

	// granule specific stuff
	for ( ch = 0; ch < nch; ch++ ) {
		for ( gr = 0; gr < ngr; gr++ ) {
			granule = frame->granules[ch][gr];
			granule->share				= ( gr == 0 ) ? i_share : 0;
			granule->window_switching	= i_sblocks;
			granule->mixed_flag			= i_mixed;
			granule->block_type			= LONG_BLOCK;
			granule->sb_gain[0]			= i_sbgain;
			granule->sb_gain[1]			= i_sbgain;
			granule->sb_gain[2]			= i_sbgain;
			granule->preemphasis		= i_preemphasis;
			granule->coarse_scalefactors = i_coarse;
		}
	}
	
	
	return frame;
}


/* -----------------------------------------------
	append frame to the frame chain
	----------------------------------------------- */
INTERN inline bool mp3_append_frame( mp3Frame* frame )
{
	// thread_local: -th processes independent files concurrently and each needs
	// its own frame-chain build state (reset when this thread's lastframe==NULL).
	static thread_local granuleInfo* lastgranule[2] = { NULL, NULL };
	static thread_local int n = 0;
	int ch, gr;
	int ngr = mp3_ngr( frame->mpeg );  // 2 for MPEG-1, 1 for MPEG-2/2.5


	// insert frame into the frame chain, set up links between frames
	// aux size correction has to take place elsewhere
	if ( lastframe == NULL ) {
		firstframe = frame;
		frame->prev = NULL;
		lastgranule[0] = NULL;
		lastgranule[1] = NULL;
		n = 0;
	} else {
		lastframe->next = frame;
		frame->prev = lastframe;
	}
	lastframe = frame;

	// set up the per-channel doubly-linked granule chain across frames.
	// Global granule number = frame_no * ngr + gr (MPEG-1: n<<1 | gr).
	for ( ch = 0; ch < frame->nchannels; ch++ ) {
		for ( gr = 0; gr < ngr; gr++ ) {
			granuleInfo* g = frame->granules[ch][gr];
			g->n = n * ngr + gr;
			g->next = NULL;
			if ( gr == 0 ) {
				g->prev = lastgranule[ch];
				if ( lastgranule[ch] != NULL ) lastgranule[ch]->next = g;
			} else {
				g->prev = frame->granules[ch][gr-1];
				frame->granules[ch][gr-1]->next = g;
			}
		}
		lastgranule[ch] = frame->granules[ch][ngr-1];
	}
	
	// some generic stuff
	frame->n = n++;
	frame->next = NULL;
	
	
	return true;
}


/* -----------------------------------------------
	discard frame data
	----------------------------------------------- */
INTERN inline bool mp3_discard_frame( mp3Frame* frame )
{
	int nch;
	int ch, gr;
	
	
	// discard all data in one frame
	nch = frame->nchannels;
	if ( frame->granules != NULL ) {
		int ngr = mp3_ngr( frame->mpeg );  // MPEG-2/2.5 allocated only 1 granule
		for ( ch = 0; ch < nch; ch++ ) {
			for ( gr = 0; gr < ngr; gr++ )
				free ( frame->granules[ch][gr] );
			free ( frame->granules[ch] );
		}
		free ( frame->granules );
	}
	free ( frame );	
	
	
	return true;
}


/* -----------------------------------------------
	mute a single frame
	----------------------------------------------- */
INTERN inline bool mp3_mute_frame( mp3Frame* frame )
{
	granuleInfo* granule;
	unsigned char* ptr;
	int nch, ums;
	int ch, gr;
	
	
	// mute a single frame -> dangerous, be careful!
	// LSF has 1 granule and a 9-bit scalefac_compress, so it needs 5 bytes per
	// granule (vs MPEG-1's 4 bytes holding a 4-bit slength).
	nch = frame->nchannels;
	int ngr = mp3_ngr( frame->mpeg );
	bool lsf = ( frame->mpeg != MP3_V1_0 );
	int  gb  = lsf ? 5 : 4;
	ums = 2 + ( nch * ngr * gb );
	
	// store reconstruction data
	if ( unmute_data_size == 0 ) { // first alloc - no mem check needed 
		unmute_data = (unsigned char*) calloc( ++unmute_data_size, sizeof( char ) );
		unmute_data_size = 1;
		*unmute_data = 0;
	}	
	unmute_data = (unsigned char*) // realloc for new size
		frealloc( unmute_data, ( unmute_data_size + ums ) * sizeof( char ) );
	if ( unmute_data == NULL ) {
		snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
		errorlevel = 2;
		return false;
	}
	ptr = unmute_data + unmute_data_size;
	unmute_data_size += ums;
	// some bits are wasted here
	// normally muting frames shouldn't be necessary at all anyways
	(*ptr)    = (frame->privbit&0x1)    << 7;
	(*ptr)   |= (frame->original&0x1)   << 6;
	(*ptr)   |= (frame->copyright&0x1)  << 5;
	(*ptr++) |= (frame->bit_reservoir >> 8) & 0x1;
	(*ptr++)  = (frame->bit_reservoir >> 0) & 0xFF;
	for ( ch = 0; ch < nch; ch++ ) {
		for ( gr = 0; gr < ngr; gr++ ) {
			granule = frame->granules[ch][gr];
			if ( lsf ) { // 5 bytes: main_data_bit(12), big_val_pairs(9), scalefac_compress(9)
				(*ptr++) = granule->main_data_bit & 0xFF;
				(*ptr++) = (granule->main_data_bit >> 8) & 0x0F;
				(*ptr++) = granule->big_val_pairs & 0xFF;
				(*ptr++) = ( (granule->big_val_pairs >> 8) & 0x1 ) | ( ( (granule->scalefac_compress >> 8) & 0x1 ) << 1 );
				(*ptr++) = granule->scalefac_compress & 0xFF;
			} else { // 4 bytes: main_data_bit(12), slength(4), big_val_pairs(9)
				(*ptr++) = (granule->main_data_bit >> 4) & 0xFF;
				(*ptr++) = ( (granule->main_data_bit << 4) & 0xF0 ) | ( granule->slength & 0x0F );
				(*ptr++) = (granule->big_val_pairs >> 1) & 0xFF;
				(*ptr++) = (granule->big_val_pairs << 7) & 0x80;
			}
		}
	}
	// take count - careful this doesn't excede 255
	(*unmute_data)++;
	
	// mute frame - this has to be done in order
	// we assume that all previous frames are muted, too!
	// move aux data around the bit reservoirs only for files that already provide a
	// bit reservoir. the decoder depends on this.
	if ( frame->prev != NULL && i_bit_res != 0 ) {
		// make some room, use prev frame aux too!
		frame->bit_reservoir += frame->prev->aux_size;
		if ( frame->bit_reservoir >= 512 ) {
			frame->prev->aux_size = frame->bit_reservoir - 511;
			frame->bit_reservoir = 511;
		} else frame->prev->aux_size = 0;
	} else frame->bit_reservoir = 0;
	frame->aux_size = frame->frame_size - frame->fixed_size + frame->bit_reservoir;
	if ( frame->next != NULL ) frame->aux_size -= frame->next->bit_reservoir;
	frame->main_size = 0;
	frame->main_bits = 0;
	if ( frame->granules != NULL ) {
		for ( ch = 0; ch < nch; ch++ ) {
			for ( gr = 0; gr < ngr; gr++ ) {
				granule = frame->granules[ch][gr];
				granule->main_data_bit = 0;
				granule->big_val_pairs = 0;
				granule->slength = 0;
				granule->scalefac_compress = 0;
				// fix region bounds
				granule->region_bound[0] = 0;
				granule->region_bound[1] = 0;
				granule->region_bound[2] = 0;
			}
		}
	}

	
	return true;
}


/* -----------------------------------------------
	unmute a single frame
	----------------------------------------------- */
INTERN inline bool mp3_unmute_frame( mp3Frame* frame )
{
	granuleInfo* granule;
	unsigned char* ptr;
	int nch, ums;
	int ch, gr;
	
	
	// restore a single frame to it's original, broken state
	// enough fix data has to be present, channel mode has to be consistent
	nch = g_nchannels;
	int ngr = mp3_ngr( frame->mpeg );
	bool lsf = ( frame->mpeg != MP3_V1_0 );
	int  gb  = lsf ? 5 : 4;
	ums = 2 + ( nch * ngr * gb );
	ptr = unmute_data + 1 + ( n_bad_first * ums );
	
	// unmute frame - has to be done in order, too!
	frame->privbit    = ((*ptr)>>7) & 0x1;
	frame->original   = ((*ptr)>>6) & 0x1;
	frame->copyright  = ((*ptr)>>5) & 0x1;
	frame->bit_reservoir  = (*ptr++) << 8;
	frame->bit_reservoir |= (*ptr++) << 0;
	for ( ch = 0; ch < nch; ch++ ) {
		for ( gr = 0; gr < ngr; gr++ ) {
			granule = frame->granules[ch][gr];
			if ( lsf ) { // mirror of the 5-byte LSF layout in mp3_mute_frame
				granule->main_data_bit  = (*ptr++);
				granule->main_data_bit |= ( (*ptr++) & 0x0F ) << 8;
				granule->big_val_pairs  = (*ptr++);
				granule->big_val_pairs |= ( (*ptr) & 0x1 ) << 8;
				granule->scalefac_compress = ( ( (*ptr++) >> 1 ) & 0x1 ) << 8;
				granule->scalefac_compress |= (*ptr++);
				granule->slength = 0;
			} else {
				granule->main_data_bit = (*ptr++) << 4;
				granule->main_data_bit |= (*ptr) >> 4;
				granule->slength = (*ptr++) & 0x0F;
				granule->big_val_pairs = (*ptr++) << 1;
				granule->big_val_pairs |= (*ptr++) >> 7;
			}
		}
	} // no need to accomodate for the changed frame params - for now (!!!)
	
	
	return true;
}


/* -----------------------------------------------
	build fixed part of physical frame
	----------------------------------------------- */
INTERN inline unsigned char* mp3_build_fixed( mp3Frame* frame )
{
	static thread_local unsigned char* fixed = ( unsigned char* ) calloc( 64, 1 );
	unsigned char* tmp_ptr;
	
	granuleInfo* granule;
	abitwriter* side_writer;
	
	unsigned char* header;
	unsigned char* sideinfo;
	unsigned short crc;
	
	int nsb = 0;
	int nch = 0;
	int ch, gr;

	
	// preparations
	memset( fixed, 0, 64 );
	nch = frame->nchannels;
	const bool lsf = ( frame->mpeg != MP3_V1_0 );
	const int  ngr = mp3_ngr( frame->mpeg );
	nsb = lsf ? ( ( nch == 1 ) ? 9 : 17 ) : ( ( nch == 1 ) ? 17 : 32 );
	header = fixed + 0;
	sideinfo = fixed + ( ( frame->protection ) ? 4 + 2 : 4 );
	
	
	// --- frame header ---
	
	// insert data. Base 0xE0 = the 3 top sync bits only, so the MPEG version
	// (bits 4-3) is set by the OR below — was hardcoded 0xFA (=MPEG-1), which an
	// OR cannot clear, forcing every reconstructed frame back to MPEG-1.
	header[0]  = 0xFF;
	header[1]  = 0xE0;
	header[1] |= frame->mpeg       << 3;
	header[1] |= frame->layer      << 1;
	header[1] |= frame->protection  ^ 1;
	header[2] |= frame->bits       << 4;
	header[2] |= frame->samples    << 2;
	header[2] |= frame->padding    << 1;
	header[2] |= frame->privbit    << 0;
	header[3] |= frame->channels   << 6;
	header[3] |= frame->stereo_ms  << 5;
	header[3] |= frame->stereo_int << 4;
	header[3] |= frame->copyright  << 3;
	header[3] |= frame->original   << 2;
	header[3] |= frame->emphasis   << 0;

	
	// --- side information... ---
	
	// init abitwriter
	side_writer = new abitwriter( 64 );
	
	// frame global side info. LSF: 8-bit main_data_begin, 1/2 private bits, no scfsi.
	side_writer->write( frame->bit_reservoir, lsf ? 8 : 9 );
	side_writer->write( frame->padbits, lsf ? ( (nch==1) ? 1 : 2 ) : ( (nch==1) ? 5 : 3 ) );
	if ( !lsf )
		for ( ch = 0; ch < nch; ch++ )
			side_writer->write( frame->granules[ch][0]->share, 4 );

	// granule specific side info
	for ( gr = 0; gr < ngr; gr++ ) {
		for ( ch = 0; ch < nch; ch++ ) {
			granule = frame->granules[ch][gr];
			side_writer->write( granule->main_data_bit, 12 );
			side_writer->write( granule->big_val_pairs, 9 );
			side_writer->write( granule->global_gain, 8 );
			// LSF: 9-bit scalefac_compress; MPEG-1: 4-bit slength.
			if ( lsf ) side_writer->write( granule->scalefac_compress, 9 );
			else       side_writer->write( granule->slength, 4 );
			side_writer->write( granule->window_switching, 1 );
			if ( granule->window_switching == 0 ) { // for normal blocks
				side_writer->write( granule->region_table[0], 5 );
				side_writer->write( granule->region_table[1], 5 );
				side_writer->write( granule->region_table[2], 5 );
				side_writer->write( granule->region0_size, 4 );
				side_writer->write( granule->region1_size, 3 );
			} else { // for special blocks
				side_writer->write( granule->block_type, 2 );
				side_writer->write( granule->mixed_flag, 1 );
				side_writer->write( granule->region_table[0], 5 );
				side_writer->write( granule->region_table[1], 5 );
				side_writer->write( granule->sb_gain[0], 3 );
				side_writer->write( granule->sb_gain[1], 3 );
				side_writer->write( granule->sb_gain[2], 3 );
			}
			// LSF has no preflag.
			if ( !lsf ) side_writer->write( granule->preemphasis, 1 );
			side_writer->write( granule->coarse_scalefactors, 1 );
			side_writer->write( granule->select_htabB, 1 );
		}
	}
	
	// get pointer, store and free up memory
	tmp_ptr = side_writer->getptr();
	memcpy( sideinfo, tmp_ptr, nsb ); 
	delete( side_writer );
	free( tmp_ptr );
	
	
	// --- ...and optional crc checksum ---
	
	if ( frame->protection == 0x1 ) {
		crc = mp3_calc_layer3_crc( header, sideinfo, nsb );
		header[ 4 ] = (crc>>8)&0xFF;
		header[ 5 ] = (crc>>0)&0xFF;
	}
	
	
	return fixed;	
}


/* -----------------------------------------------
	seeks for the first proper MPEG audio frame
	----------------------------------------------- */
INTERN inline int mp3_seek_firstframe( unsigned char* data, int size )
{
	int mpeg = -1;
	int layer = -1;
	int samples = -1;
	int channels = -1;
	int protection = -1;
	
	int bits;
	int padding;
	int frame_size;
	
	int pos0 = 0;
	int pos1 = 0;
	int pos, n;
	
	
	// check for ID3 or other tag at beginning of data
	pos0 = mp3_get_id3_size( data, size );
	
	// calculate last tolerable seek position
	pos1 = pos0 + GARBAGE_TOLERANCE;
	if ( pos1 > size - 4 ) pos1 = size - 4;
	
	// mp3 frame seeker loop
	// conditions for proper first frame: 5 consecutive frames,
	// same channel, mpeg, layer, samples setting
	while( true ) {
		// seek for first frame candidate
		for ( ; pos0 < pos1; pos0++ )
			if ( data[pos0] == 0xFF ) if ( (data[pos0+1]&0xE0) == 0xE0 ) break;
		// nothing found -> give up
		if ( pos0 == pos1 ) return -1;
		// check for consecutive frames
		for ( pos = pos0, n = 0; ( n < 5 ) && ( pos < size - 4 ); n++, pos += frame_size ) {
			// check syncword
			if ( ( data[pos] != 0xFF ) || ( (data[pos+1]&0xE0) != 0xE0 ) ) break;
			// extract and store or compare data from header
			if ( n == 0 ) {
				mpeg        = (data[pos+1]>>3)&0x3;
				layer       = (data[pos+1]>>1)&0x3;
				protection  = (data[pos+1]>>0)&0x1;
				samples     = (data[pos+2]>>2)&0x3;
				channels    = (data[pos+3]>>6)&0x3;
			} else if (
				( mpeg       != ((data[pos+1]>>3)&0x3) ) ||
				( layer      != ((data[pos+1]>>1)&0x3) ) ||
				( protection != ((data[pos+1]>>0)&0x1) ) ||
				( samples    != ((data[pos+2]>>2)&0x3) ) ||
				( channels   != ((data[pos+3]>>6)&0x3) ) ) break;
			bits     = (data[pos+2]>>4)&0xF;
			padding  = (data[pos+2]>>1)&0x1;			
			// check for problems
			if ( ( mpeg == 0x1 ) || ( layer == 0x0 ) ||
				( bits == 0x0 ) || ( bits == 0xF ) || ( samples == 0x3 ) ) break;
			// find out frame size
			frame_size = frame_size_table[mpeg][layer][samples][bits];
			if ( padding ) frame_size += (layer == LAYER_I) ? 4 : 1;
		}
		// consider first frame proper if 5 consecutive frames are found
		if ( n >= 5 ) break;
		pos0++;		
	}
	
	
	return pos0;
}


/* -----------------------------------------------
	extract ID3v2 tag from beginning of file
	----------------------------------------------- */
INTERN inline int mp3_get_id3_size( unsigned char* id3tag, int max_size )
{
	static const char* id3v1_begin = "TAG";
	static const char* id3v2_begin = "ID3";
	static const char* lyrics3_begin = "LYRICSBEGIN";
	static const char* apetag_begin = "APETAGEX";
	bool unsynchronized = false;
	int size;
	
	
	// check if at least 10 bytes are available
	if ( max_size < 10 ) return 0;
	
	if ( memcmp( id3v2_begin, id3tag, 3 ) == 0 ) {
		// ID3v2 tag -> find out size, start with 10
		size = 10;
		// check for unsynchronization
		unsynchronized = ( BITN( id3tag[5], 7 ) == 1 );
		// check for footer
		size += ( BITN( id3tag[5], 4 ) == 1 ) ? 10 : 0;
		// calculate size
		size += id3tag[9] <<  0;
		size += id3tag[8] <<  7;
		size += id3tag[7] << 14;
		size += id3tag[6] << 21;		
		// check if size is ok
		if ( size > max_size ) return 0;
		// pay attention to unsynchronization where needed
		if ( unsynchronized ) for ( int pos = 0; pos < size-1; pos++ ) {
			if ( id3tag[pos] == 0xFF ) if ( id3tag[pos+1] == 0x00 ) {
				pos++; size++;
				if ( size > max_size ) return 0;
			}
		}
	}
	else if ( memcmp( id3v1_begin, id3tag, 3 ) == 0 ) {
		// ID3v1 tag -> size is exactle 128 byte
		size = 128;
		// check if size is ok
		if ( size > max_size ) return 0;
	}
	else if ( ( memcmp( lyrics3_begin, id3tag, 6 ) == 0 ) && ( max_size >= 148 ) ) {
		// LYRICS3 tag, must be followed by ID3v1 -> doublecheck and find out size
		if ( ( memcmp( lyrics3_begin, id3tag, 11 ) == 0 ) && ( memcmp( id3v1_begin, id3tag + max_size - 128, 3 ) == 0 ) && ( memcmp( lyrics3_begin, (char*) id3tag + max_size - 128 - 9, 6 ) == 0 ) )
			size = max_size;
		else return 0;
	}
	else if ( memcmp( apetag_begin, id3tag, 8 ) == 0 ) {
		// APE tag -> keep all the data after (ok solution for now)
		size = max_size;
	}
	else {
		// no tag header found -> not a proper tag
		return 0;
	}
	
	
	return size;
}


/* -----------------------------------------------
	calculate frame crc
	----------------------------------------------- */
INTERN inline unsigned short mp3_calc_layer3_crc( unsigned char* header, unsigned char* sideinfo, int sidesize )
{
	// crc has a start value of 0xFFFF
	unsigned short crc = 0xFFFF;
	
	// process two last bytes from header...
	crc = (crc << 8) ^ crc_table[(crc>>8) ^ header[2]];
	crc = (crc << 8) ^ crc_table[(crc>>8) ^ header[3]];
	// ... and all the bytes from the side information
	for ( int i = 0; i < sidesize; i++ )
		crc = (crc << 8) ^ crc_table[(crc>>8) ^ sideinfo[i]];
	
	return crc;
}


/* -----------------------------------------------
	decode one MP3 frame
	----------------------------------------------- */
#if !defined(BUILD_LIB) && defined(DEV_BUILD)
INTERN inline granuleData*** mp3_decode_frame( huffman_reader* dec, mp3Frame* frame )
{
	// storage (thread_local: each -th worker decodes a different frame)
	static thread_local granuleData*** frame_data = NULL;
	granuleInfo* granule;
	signed short* coefs;
	unsigned char* scfs;
	// scalefactors settings
	const int* slen;
	int sl;	
	// coefficients settings
	short* region_bounds;
	char* region_tables;
	huffman_dec_table* bv_table;
	huffman_conv_set* conv_set;
	signed short* cf_start;
	int linbits;
	// general settings
	unsigned char vals[4];
	int bitp = 0;
	int lmaxp = 0;
	char share;
	// counters
	int ch, gr;
	int p, g, r;
	int i;
	
	
	
	// alloc mem for frame data if not done before
	// no need to free this memory again!
	if ( frame_data == NULL ) {
		frame_data = ( granuleData*** ) calloc( 2, sizeof( granuleData** ) );
		for ( ch = 0; ch < 2; ch++ ) {
			frame_data[ ch ] = ( granuleData** ) calloc( 2, sizeof( granuleData* ) );
			for ( gr = 0; gr < 2; gr++ ) {
				frame_data[ ch ][ gr ] = ( granuleData* ) calloc( 1, sizeof( granuleData ) );
				frame_data[ ch ][ gr ]->scalefactors = ( unsigned char* ) calloc( 36, sizeof( char ) );
				frame_data[ ch ][ gr ]->coefficients = ( signed short* ) calloc( 578, sizeof( short ) );
			}
		}
	}
	
	
	// decode frame (MPEG-1: 2 granules, MPEG-2/2.5: 1)
	for ( gr = 0; gr < mp3_ngr( frame->mpeg ); gr++ ) {
		for ( ch = 0; ch < frame->nchannels; ch++ ) {
			// --- preparations ---
			granule = frame->granules[ch][gr];
			coefs = frame_data[ch][gr]->coefficients;
			scfs = frame_data[ch][gr]->scalefactors;
			cf_start = coefs;
			// clear memory (= set coefficents/scalefactors zero)
			memset( coefs, 0, sizeof( short ) * 576 );
			memset( scfs, 0, sizeof( char ) * 36 );
			// set position in stream
			dec->setpos( frame->main_index + ( bitp / 8 ), 8 - ( bitp % 8 ) );
			dec->reset_counter();
			// take count of inner frame main data bits
			lmaxp = granule->main_data_bit;
			bitp += lmaxp;
			// set decoding parameters
			slen = slength_table[ (int) granule->slength ];
			region_bounds = granule->region_bound;
			region_tables = granule->region_table;
			
			// --- scale factors ---
			if ( frame->mpeg != MP3_V1_0 ) {
				// MPEG-2/2.5 (LSF) scalefactors: read scfcnt[g] values of scfsz[g]
				// bits, linearly, no sharing. Stored raw for lossless reconstruction.
				int scfsz[ 4 ], scfcnt[ 4 ];
				mp3_lsf_scf_params( frame, granule, ch, scfsz, scfcnt );
				for ( g = 0; g < 4; g++ )
					for ( p = 0; p < scfcnt[ g ]; p++ )
						*(scfs++) = dec->read_bits( scfsz[ g ] );
			}
			// read long block scalefactors (with sharing)
			else if ( granule->block_type != SHORT_BLOCK ) {
				// get sharing params if any
				share = ( gr ) ? frame->granules[ch][0]->share : 0;
				// read 21 (max) scalefactors
				for ( g = 0, p = 0; g < 4; g++ ) {
					if ( (share>>(3-g)) & 0x1 ) { // shared
						memcpy( scfs, frame_data[ch][0]->scalefactors + p, sizeof( char) * scf_width[ g ] );
						scfs += scf_width[ g ];
						p = scf_bounds[ g ];
					} else for ( sl = slen[ (g<2)?0:1 ]; p < scf_bounds[ g ]; p++ ) { // non shared
						*(scfs++) = dec->read_bits( sl );
					}
				}
			} else { // read short block scfs
				// read 36 (3*12) scalefactors
				for ( i = 0; i < 3; i++ ) // 3 subblocks
					for ( g = 0, p = 0; g < 3; g++ ) // 3 groups
						for ( sl = slen[ (g==0)?0:1 ]; p < scf_bounds_short[ g ]; p++ ) // no sharing!
							*(scfs++) = dec->read_bits( sl );
			}
			
			// --- coefficients / big values ---
			for ( p = 0, r = 0; r < 3; r++ ) {
				if ( region_tables[ r ] == 0 ) { // tbl0 skipping
					coefs = cf_start + region_bounds[ r ];
					p = region_bounds[ r ];			
					continue;
				}
				// decoding with other tables
				bv_table = bv_dec_table + region_tables[ r ];
				if ( bv_table == NULL ) return NULL;
				conv_set = bv_table->h;
				linbits = bv_table->linbits;
				if ( linbits == 0 ) { // without linbits
					for ( ; p < region_bounds[ r ]; p += 2, coefs += 2 ) {
						dec->decode_pair( conv_set, vals );
						for ( i = 0; i < 2; i++ ) if ( vals[i] > 0 ) 
							coefs[i] = ( dec->read_bit() ) ? -vals[i] : vals[i];
						if ( dec->get_count() > lmaxp ) {
							// high error tolerance!
							memset( coefs, 0, sizeof( short ) * 2 );
							break;
						}
					}
				} else { // with linbits
					for ( ; p < region_bounds[ r ]; p += 2, coefs += 2 ) {
						dec->decode_pair( conv_set, vals );
						for ( i = 0; i < 2; i++ ) if ( vals[i] > 0 ) {
							coefs[i] = ( vals[i] == 15 ) ? 15 + dec->read_bits( linbits ) : vals[i];
							if ( dec->read_bit() ) coefs[i] = - coefs[i];
						}
						if ( dec->get_count() > lmaxp ) {
							// high error tolerance!
							memset( coefs, 0, sizeof( short ) * 2 );
							break;
						}
					}
				}
			}
			
			// --- coefficients / small values ---
			conv_set = ( granule->select_htabB ) ? &htabB_dec : &htabA_dec;
			for ( ; ( dec->get_count() < lmaxp ) && ( p < 576 ); p += 4, coefs += 4 ) {
				dec->decode_quadruple( conv_set, vals );
				for ( i = 0; i < 4; i++ ) if ( vals[i] ) 
					coefs[i] = ( dec->read_bit() ) ? -1 : 1;
				if ( dec->get_count() > lmaxp ) {
					memset( coefs, 0, sizeof( short ) * 4 );
					break;
				}
			}
			
			// set sv_bound
			granule->sv_bound = p;
		}
	}
	
	
	
	return frame_data;
}
#endif // DEV_BUILD

/* ----------------------- End of MP3 specific functions -------------------------- */

/* ----------------------- Begin of PMP specific functions -------------------------- */


/* -----------------------------------------------
	writes the PMP file header
	----------------------------------------------- */	
INTERN inline bool pmp_write_header( iostream* str )
{
	unsigned char header[4] = { 0 };
	unsigned char nframes[4] = { 0 };
	
	
	// build the header[]
	// store necessary information, ignore unsupported
	// 1st byte: global samples and channels, and...
	// ...bitrate (zero if vbr/not global)
	header[0] |= i_samplerate << 6; // sample rate
	header[0] |= i_channels << 4; // channel mode
	header[0] |= ( (i_bitrate!=-1) ? i_bitrate : 0 ) << 0; // bitrate
	// 2nd byte: usage of padding, ms/int stereo, special blocks, subblock gain, ...
	// ... sharing, preemphasis and coarse scalefactors
	header[1] |= ( (i_padding==0) ? 0 : 1 ) << 7; // padding
	header[1] |= ( (i_stereo_ms==0 ) ? 0 : 1 ) << 6; // ms stereo
	header[1] |= ( (i_stereo_int==0 ) ? 0 : 1 ) << 5; // int stereo
	header[1] |= ( (i_sblocks==0 ) ? 0 : 1 ) << 4; // special blocks
	header[1] |= ( (i_sbgain==0) ? 0 : 1 ) << 3; // subblock gain
	header[1] |= ( (i_share==0) ? 0 : 1 ) << 2; // sharing
	header[1] |= ( (i_preemphasis==0) ? 0 : 1 ) << 1; // preemphasis
	header[1] |= ( (i_coarse==0) ? 0 : 1 ) << 0; // coarse scfs
	// 3rd byte: setting of protection, original, copyright and private bits, ...
	// ... emphasis setting and indicators for data before/after, special block diffs, bad first frames
	header[2] |= i_protection << 7; // protection
	header[2] |= i_original << 6; // original bit
	header[2] |= i_copyright << 5; // copyright bit
	header[2] |= i_privbit << 4; // private bit
	header[2] |= i_emphasis << 2; // emphasis
	header[2] |= ( (data_before_size>0) ? 1 : 0 ) << 1; // data before
	header[2] |= ( (data_after_size>0) ? 1 : 0 ) << 0; // data after
	// 4th byte: usage of bit reservoir, special block diffs, bad first frames
	// ... still 5 free (use for max comp)
	header[3] |= ( (i_bit_res==0) ? 0 : 1 ) << 7; // bit reservoir
	header[3] |= ( (i_sb_diff==0) ? 0 : 1 ) << 6; // special block diffs
	header[3] |= ( (n_bad_first>0) ? 1 : 0 ) << 5; // bad first frames
	// v2.1: embedded cover-art (APIC) recompression flag. Old (< v2.1)
	// readers ignore bit 4; v2.1+ reads it and, if set, an extra raw record
	// right after this header (see pmp_read_header/uncompress_pmp).
	header[3] |= ( apic_present ? 1 : 0 ) << 4; // APIC recompressed
	// v1.3: MPEG version (2 bits) in the previously-free low bits, so MPEG-2/2.5
	// Layer III can be reconstructed. Old v1.1/v1.2 readers ignore byte 3 low
	// bits; v1.3+ reads them only when the archive version says they exist.
	header[3] |= ( i_mpeg & 0x3 ) << 0; // 3=MPEG-1, 2=MPEG-2, 0=MPEG-2.5
	
	// store # of frames in nframes[] in little endian
	// yup, that's a waste, but - so what?
	nframes[0] = ( g_nframes >> 24 ) & 0xFF;
	nframes[1] = ( g_nframes >> 16 ) & 0xFF;
	nframes[2] = ( g_nframes >>  8 ) & 0xFF;
	nframes[3] = ( g_nframes >>  0 ) & 0xFF;
	
	// 8 bytes to write, together with magic # and version: 11 bytes
	str->write( (void*) header, 1, 4 );
	str->write( (void*) nframes, 1, 4 );
	
		
	return true;
}


/* -----------------------------------------------
	reads the PMP file header
	----------------------------------------------- */
INTERN inline bool pmp_read_header( iostream* str )
{
	unsigned char header[4] = { 0 };
	unsigned char nframes[4] = { 0 };
	
	mp3Frame* frame = NULL;
	int n;
	
	
	// read header and size from file
	str->read( (void*) header, 1, 4 );
	str->read( (void*) nframes, 1, 4 );
	if ( str->chkeof() ) {
		snprintf( errormessage, MSG_SIZE, "unexpected end of data" );
		errorlevel = 2;
		return false;
	}
	
	// extract information from header[]
	// 1st byte: global samples and channels, and...
	// ...bitrate (zero if vbr/not global)
	i_samplerate 	= (header[0]>>6)&0x3;
	i_channels 		= (header[0]>>4)&0x3;
	i_bitrate		= (header[0]>>0)&0xF;
	if ( i_bitrate == 0 ) i_bitrate =-1;
	// 2nd byte: usage of padding, ms/int stereo, special blocks, subblock gain, ...
	// ... sharing, preemphasis and coarse scalefactors
	i_padding 		= -((header[1]>>7)&0x1);
	i_stereo_ms		= -((header[1]>>6)&0x1);
	i_stereo_int	=  ((header[1]>>5)&0x1);
	i_sblocks		=  ((header[1]>>4)&0x1);
	i_sbgain 		= -((header[1]>>3)&0x1);
	i_share			= -((header[1]>>2)&0x1);
	i_preemphasis	= -((header[1]>>1)&0x1);
	i_coarse		= -((header[1]>>0)&0x1);
	// 3rd byte: setting of protection, original, copyright and private bits, ...
	// ... emphasis setting and indicators for data before/after, special block diffs, bad first frames
	i_protection	= (header[2]>>7)&0x1;
	i_original		= (header[2]>>6)&0x1;
	i_copyright		= (header[2]>>5)&0x1;
	i_privbit		= (header[2]>>4)&0x1;
	i_emphasis		= (header[2]>>2)&0x3;
	data_before_size	= (header[2]>>1)&0x1;
	data_after_size		= (header[2]>>0)&0x1;
	// 4th byte: usage of bit reservoir, special block diffs, bad first frames
	// ... still 5 free (use for max comp)
	i_bit_res		= -((header[3]>>7)&0x1);
	i_sb_diff		=  ((header[3]>>6)&0x1);
	n_bad_first		=  (header[3]>>5)&0x1;
	// v2.1+ archives may carry a recompressed cover-art record right after
	// this header; older readers/archives simply never see the bit set.
	apic_present	= ( pmp_archive_version >= 21 ) && ( (header[3]>>4)&0x1 );

	// set nframes known i_variables.
	// v1.3+ archives store the MPEG version in byte 3 low bits; older archives
	// (v1.1/v1.2) only ever held MPEG-1, so default to that for them.
	i_mpeg			= ( pmp_archive_version >= 13 ) ? (char)( header[3] & 0x3 ) : MP3_V1_0;
	i_layer			= LAYER_III;
	i_padbits		= 0;
	i_mixed			= 0;
	i_aux_h			= -2;
	
	// check for possible mistakes made by myself
	if ( ( i_bitrate == 0xF ) || ( i_samplerate == 0x3 ) ) {
		snprintf( errormessage, MSG_SIZE, "not a proper .pm3 file" );
		errorlevel = 2;
		return false;
	}
	
	// extract # of frames from nframes[] in little endian
	g_nframes  = 0;
	g_nframes |= nframes[0] << 24;
	g_nframes |= nframes[1] << 16;
	g_nframes |= nframes[2] <<  8;
	g_nframes |= nframes[3] <<  0;
		
	// build frames structure
	for ( n = 0; n < g_nframes; n++ ) {
		// read frame, check result
		frame = mp3_build_frame();
		// bad result -> use existing error message and exit
		if ( frame == NULL ) return false;
		// insert frame into the frame chain 
		mp3_append_frame( frame );
	}
	
	// make some safe assumptions
	g_nchannels = ( i_channels == MP3_MONO ) ? 1 : 2;  // number of channels
	g_samplerate = samplerate_table[(int)i_mpeg][(int)i_samplerate];  // sample rate (per MPEG version)
	g_bitrate = ( i_bitrate == -1 ) ? 0 : bitrate_table[(int)i_mpeg][LAYER_III][(int)i_bitrate]; // bit rate
	
	
	return true;
}


#if !defined( STORE_ID3 )
/* -----------------------------------------------
	encodes a stream of generic data (8bit)
	----------------------------------------------- */
INTERN bool pmp_encode_id3( aricoder* enc )
{
	// this will be used for id3 tags, other tags and garbage
	// little is known, so we'll just use a generic markov model
	model_s* model;
	int i;
	
	
	// arithmetic encode data
	model = INIT_MODEL_S( 256 + 1, 256, 0 );
	// data before main data
	if ( data_before_size > 0 ) {
		for ( i = 0; i < data_before_size; i++ )
			encode_ari( enc, model, data_before[ i ] );
		// encode end-of-data symbol (256)
		encode_ari( enc, model, 256 );
	}
	// data after main data
	if ( data_after_size > 0 ) {
		for ( i = 0; i < data_after_size; i++ )
			encode_ari( enc, model, data_after[ i ] );
		// encode end-of-data symbol (256)
		encode_ari( enc, model, 256 );
	}
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	decodes a stream of generic data (8 bit)
	----------------------------------------------- */
INTERN bool pmp_decode_id3( aricoder* dec )
{
	abytewriter* bwrt;
	model_s* model;
	int c;
	
	
	// decode max. 2 chunks of data, ending with 256 symbol
	model = INIT_MODEL_S( 256 + 1, 256, 0 );
	
	// data before main data
	if ( data_before_size > 0 ) {
		bwrt = new abytewriter( 1024 ); // start byte writer
		while ( true ) {
			c = decode_ari( dec, model );
			if ( c == 256 ) break;
			bwrt->write( (unsigned char) c );
		}	
		// check for out of memory
		if ( bwrt->error ) {
			delete bwrt; delete model;
			snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
			errorlevel = 2;
			return false;
		}
		// get data/true length and close byte writer
		data_before = bwrt->getptr();
		data_before_size = bwrt->getpos();
		delete bwrt;
	}
	
	// data after main data
	if ( data_after_size > 0 ) {
		bwrt = new abytewriter( 1024 ); // start byte writer
		while ( true ) {
			c = decode_ari( dec, model );
			if ( c == 256 ) break;
			bwrt->write( (unsigned char) c );
		}	
		// check for out of memory
		if ( bwrt->error ) {
			delete bwrt; delete model;
			snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
			errorlevel = 2;
			return false;
		}
		// get data/true length and close byte writer
		data_after = bwrt->getptr();
		data_after_size = bwrt->getpos();
		delete bwrt;
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}
#else
/* -----------------------------------------------
	stores a stream of generic data (8bit)
	----------------------------------------------- */
INTERN inline int pmp_store_data( iostream* str, unsigned char* data, int size )
{
	unsigned char ezis[4]; // little endian size
	
	
	// store size in little endian
	ezis[0] = ( size >> 24 ) & 0xFF;
	ezis[1] = ( size >> 16 ) & 0xFF;
	ezis[2] = ( size >>  8 ) & 0xFF;
	ezis[3] = ( size >>  0 ) & 0xFF;
	
	// the rest is as simple as it gets...
	str->write( ezis, sizeof( char ),    4 );
	str->write( data, sizeof( char ), size );
	
	
	return size;
}


/* -----------------------------------------------
	unstores a stream of generic data (8bit)
	----------------------------------------------- */
INTERN inline int pmp_unstore_data( iostream* str, unsigned char** data )
{
	unsigned char ezis[4]; // little endian size
	int size = 0; // integer size
	
	
	// unstore little endian size
	str->read( ezis, sizeof( char ), 4 );
	size |= ezis[0] << 24;
	size |= ezis[1] << 16;
	size |= ezis[2] <<  8;
	size |= ezis[3] <<  0;
	
	// alloc memory for data
	*data = (unsigned char*) calloc( size, sizeof( char ) );
	if ( *data == NULL ) {
		snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
		errorlevel = 2;
		return -1;
	}
	
	// read data to memory
	str->read( *data, sizeof( char ), size );
	
	// check for eof trouble
	if ( str->chkeof() ) {
		snprintf( errormessage, MSG_SIZE, "unexpected end of data" );
		errorlevel = 2;
		return -1;
	}
	
	
	return size;
}
#endif


#if !defined(BUILD_LIB)
/* ===========================================================================
	Embedded ID3v2 cover-art (APIC) recompression via packJPG.

	Purely a storage optimization on top of the existing generic byte-model
	id3 encoding above: if data_before (the ID3v2 tag) contains exactly one
	qualifying JPEG cover, we replace those image bytes with a smaller
	packJPG blob before handing the buffer to pmp_encode_id3 -- everything
	else in the tag (other frames, text metadata) is untouched and still
	flows through the same generic model as always. Any parsing surprise
	anywhere just means "don't recompress this one", never "produce wrong
	output" -- see the self-verify round-trip in apic_try_recompress.
   =========================================================================== */

// Length (bytes, including terminator) of a text string at p within a
// buffer of `remain` bytes, per the ID3v2 text-encoding byte (0=Latin1,
// 1=UTF16+BOM, 2=UTF16BE -> double 0x00 terminator, 2-byte aligned strides;
// 3=UTF8, and 0=Latin1 -> single 0x00). Returns -1 if not found in `remain`.
INTERN int id3_skip_text( const unsigned char* p, int remain, int encoding )
{
	if ( encoding == 1 || encoding == 2 ) {
		int i = 0;
		for ( ; i + 1 < remain; i += 2 )
			if ( p[i] == 0 && p[i+1] == 0 ) return i + 2;
		return -1;
	}
	for ( int i = 0; i < remain; i++ )
		if ( p[i] == 0 ) return i + 1;
	return -1;
}

// Case-insensitive ASCII compare, exactly len bytes (no null-termination
// assumed on `a`, which points into the raw tag buffer).
INTERN bool id3_ieq( const unsigned char* a, const char* b, int len )
{
	for ( int i = 0; i < len; i++ ) {
		unsigned char ca = a[i], cb = (unsigned char) b[i];
		if ( ca >= 'A' && ca <= 'Z' ) ca += 32;
		if ( cb >= 'A' && cb <= 'Z' ) cb += 32;
		if ( ca != cb ) return false;
	}
	return true;
}

// Finds a single qualifying JPEG cover frame in an ID3v2 tag (`tag`,
// `tag_size` bytes -- i.e. data_before). On success returns true with
// *img_off/*img_len set to the byte range of the raw JPEG data within `tag`.
// Bails (false) on anything outside the narrow, safe common case: any ID3v2
// major version other than 2/3/4, tag-level unsynchronisation, an extended
// header or footer (not accounted for in this frame walk), or a frame whose
// declared size would run past the tag body (real structural inconsistency
// -- distrust the whole parse, not just that frame). A frame with nonzero
// flags, wrong MIME/format, or non-JPEG payload is simply skipped, not a
// bail -- only the first frame that fully qualifies is ever used, everything
// else (including any other APIC/PIC frames) is left untouched.
INTERN bool id3_find_apic_jpeg( const unsigned char* tag, int tag_size, int* img_off, int* img_len )
{
	if ( tag_size < 10 || memcmp( tag, "ID3", 3 ) != 0 ) return false;
	int major = tag[3];
	if ( major != 2 && major != 3 && major != 4 ) return false;
	unsigned char tagflags = tag[5];
	if ( tagflags & 0x80 ) return false;              // unsynchronisation -- bail
	if ( major >= 3 && ( tagflags & 0x40 ) ) return false; // extended header -- bail
	if ( major == 4 && ( tagflags & 0x10 ) ) return false; // footer -- bail

	unsigned int body_size = ( (unsigned int)(tag[6]&0x7F) << 21 ) | ( (unsigned int)(tag[7]&0x7F) << 14 )
	                        | ( (unsigned int)(tag[8]&0x7F) <<  7 ) | (unsigned int)(tag[9]&0x7F);
	if ( body_size == 0 || 10 + (long long) body_size > tag_size ) return false;
	int end = 10 + (int) body_size;

	int id_len   = ( major == 2 ) ? 3 : 4;
	int size_len = ( major == 2 ) ? 3 : 4;
	int flag_len = ( major == 2 ) ? 0 : 2;
	int hdr_len  = id_len + size_len + flag_len;

	int pos = 10;
	while ( pos + hdr_len <= end ) {
		if ( tag[pos] == 0 ) break; // padding reached

		unsigned int fsize;
		if ( major == 2 )
			fsize = ( (unsigned int)tag[pos+3] << 16 ) | ( (unsigned int)tag[pos+4] << 8 ) | (unsigned int)tag[pos+5];
		else if ( major == 3 )
			fsize = ( (unsigned int)tag[pos+4] << 24 ) | ( (unsigned int)tag[pos+5] << 16 )
			      | ( (unsigned int)tag[pos+6] <<  8 ) | (unsigned int)tag[pos+7];
		else // major == 4, syncsafe
			fsize = ( (unsigned int)(tag[pos+4]&0x7F) << 21 ) | ( (unsigned int)(tag[pos+5]&0x7F) << 14 )
			      | ( (unsigned int)(tag[pos+6]&0x7F) <<  7 ) | (unsigned int)(tag[pos+7]&0x7F);

		int frame_body_off = pos + hdr_len;
		if ( (long long) frame_body_off + fsize > end ) return false; // structurally broken -- distrust whole tag

		bool is_pic = ( major == 2 ) ? ( memcmp( tag+pos, "PIC", 3 ) == 0 )
		                             : ( memcmp( tag+pos, "APIC", 4 ) == 0 );
		if ( is_pic && fsize > 0 ) {
			bool skip_this_frame = false;
			if ( flag_len == 2 ) {
				unsigned int fflags = ( (unsigned int)tag[pos+id_len+size_len] << 8 ) | tag[pos+id_len+size_len+1];
				if ( fflags != 0 ) skip_this_frame = true; // compressed/encrypted/grouped/etc -- skip, don't bail
			}
			if ( !skip_this_frame ) {
				const unsigned char* body = tag + frame_body_off;
				int blen = (int) fsize;
				int bp = 0;
				if ( blen >= 1 ) {
					int encoding = body[0];
					bp = 1;
					if ( encoding >= 0 && encoding <= 3 ) {
						bool is_jpeg_type = false;
						bool fmt_ok = true;
						if ( major == 2 ) {
							if ( bp + 3 <= blen ) { is_jpeg_type = id3_ieq( body+bp, "JPG", 3 ); bp += 3; }
							else fmt_ok = false;
						} else {
							int mime_len = id3_skip_text( body+bp, blen-bp, 0 ); // MIME: always single-null Latin1
							if ( mime_len < 0 ) fmt_ok = false;
							else {
								int mstr_len = mime_len - 1;
								is_jpeg_type = ( mstr_len == 10 && id3_ieq( body+bp, "image/jpeg", 10 ) )
								            || ( mstr_len == 9  && id3_ieq( body+bp, "image/jpg", 9 ) );
								bp += mime_len;
							}
						}
						if ( fmt_ok && bp + 1 <= blen ) {
							bp += 1; // picture-type byte
							int desc_len = id3_skip_text( body+bp, blen-bp, encoding );
							if ( desc_len >= 0 ) {
								bp += desc_len;
								int imglen_here = blen - bp;
								if ( is_jpeg_type && imglen_here >= 4
								     && body[bp] == 0xFF && body[bp+1] == 0xD8 ) {
									*img_off = frame_body_off + bp;
									*img_len = imglen_here;
									return true; // first qualifying frame wins, stop here
								}
							}
						}
					}
				}
			}
		}
		pos = frame_body_off + fsize;
	}
	return false;
}

// Encode side: on success, allocates *modified (caller frees with free()) --
// a copy of data_before with the found JPEG replaced by a smaller packJPG
// blob -- and sets apic_present/apic_offset/apic_orig_len/apic_pjg_len.
// Never modifies data_before itself. Leaves apic_present false and
// *modified/*modified_size untouched (NULL/0) on any failure to recompress.
INTERN bool apic_try_recompress( unsigned char** modified, int* modified_size )
{
	int img_off, img_len;
	if ( !id3_find_apic_jpeg( data_before, data_before_size, &img_off, &img_len ) ) return false;

	unsigned char* pjg_out = NULL; unsigned int pjg_len = 0;
	char msg[ PJG_MSG_SIZE ] = {0};
	bool ok;
	{
		std::lock_guard<std::mutex> lk( pjg_mutex );
		pjglib_init_streams( (void*)( data_before + img_off ), 1, img_len, NULL, 1 );
		ok = pjglib_convert_stream2mem( &pjg_out, &pjg_len, msg );
	}
	if ( !ok || pjg_out == NULL || (int) pjg_len >= img_len ) {
		if ( pjg_out != NULL ) free( pjg_out );
		return false; // packJPG failed, or didn't actually shrink it -- not worth it
	}

	// self-verify: decompress right back and compare bit-for-bit before
	// ever committing to using this -- never trust a single conversion.
	unsigned char* verify_out = NULL; unsigned int verify_len = 0;
	bool vok;
	{
		std::lock_guard<std::mutex> lk( pjg_mutex );
		pjglib_init_streams( (void*) pjg_out, 1, pjg_len, NULL, 1 );
		vok = pjglib_convert_stream2mem( &verify_out, &verify_len, msg );
	}
	bool roundtrip_ok = vok && verify_out != NULL && verify_len == (unsigned int) img_len
	                  && memcmp( verify_out, data_before + img_off, img_len ) == 0;
	if ( verify_out != NULL ) free( verify_out );
	if ( !roundtrip_ok ) { free( pjg_out ); return false; }

	int new_size = data_before_size - img_len + (int) pjg_len;
	unsigned char* buf = (unsigned char*) malloc( new_size > 0 ? new_size : 1 );
	if ( buf == NULL ) { free( pjg_out ); return false; }
	memcpy( buf, data_before, img_off );
	memcpy( buf + img_off, pjg_out, pjg_len );
	memcpy( buf + img_off + pjg_len, data_before + img_off + img_len, data_before_size - img_off - img_len );
	free( pjg_out );

	apic_present  = true;
	apic_offset   = img_off;
	apic_orig_len = img_len;
	apic_pjg_len  = (int) pjg_len;
	*modified = buf;
	*modified_size = new_size;
	return true;
}

// Decode side: reverses apic_try_recompress. Replaces data_before (and
// data_before_size) with a freshly allocated buffer where the packJPG blob
// at apic_offset is expanded back to the original JPEG bytes. Bounds-checks
// the (potentially corrupt/tampered) recipe fields before touching memory.
INTERN bool apic_reconstruct( void )
{
	if ( apic_offset < 0 || apic_pjg_len < 0 || apic_orig_len < 0
	     || (long long) apic_offset + apic_pjg_len > data_before_size ) {
		snprintf( errormessage, MSG_SIZE, "corrupt APIC record" );
		errorlevel = 2;
		return false;
	}

	unsigned char* jpg_out = NULL; unsigned int jpg_len = 0;
	char msg[ PJG_MSG_SIZE ] = {0};
	bool ok;
	{
		std::lock_guard<std::mutex> lk( pjg_mutex );
		pjglib_init_streams( (void*)( data_before + apic_offset ), 1, apic_pjg_len, NULL, 1 );
		ok = pjglib_convert_stream2mem( &jpg_out, &jpg_len, msg );
	}
	if ( !ok || jpg_out == NULL || jpg_len != (unsigned int) apic_orig_len ) {
		if ( jpg_out != NULL ) free( jpg_out );
		snprintf( errormessage, MSG_SIZE, "packJPG decode failed: %s", msg );
		errorlevel = 2;
		return false;
	}

	int new_size = data_before_size - apic_pjg_len + apic_orig_len;
	unsigned char* buf = (unsigned char*) malloc( new_size > 0 ? new_size : 1 );
	if ( buf == NULL ) {
		free( jpg_out );
		snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
		errorlevel = 2;
		return false;
	}
	memcpy( buf, data_before, apic_offset );
	memcpy( buf + apic_offset, jpg_out, apic_orig_len );
	memcpy( buf + apic_offset + apic_orig_len, data_before + apic_offset + apic_pjg_len,
	        data_before_size - apic_offset - apic_pjg_len );
	free( jpg_out );

	free( data_before );
	data_before = buf;
	data_before_size = new_size;
	return true;
}
#endif


/* -----------------------------------------------
	encode the padding bit (1 bit)
	----------------------------------------------- */
INTERN inline bool pmp_encode_padding( aricoder* enc )
{
	// padding bit:
	// - no correlation with others at all
	// - always comes in perfectly predictable run/length pairs
	// -> use run length encoding and arithmetic compression
	// (can still be improved upon)
	
	model_s* model;
	int run = 0;
	char bit = 0;
	
	
	// encoding start
	model = INIT_MODEL_S( PADDING_MAX_RUN+1, PADDING_MAX_RUN+1, 1 );
	for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next ) {
		if ( frame->padding != bit ) {
			bit ^= 0x1;
			while ( run >= PADDING_MAX_RUN+1 ) {
				encode_ari( enc, model, PADDING_MAX_RUN );
				model->shift_context( PADDING_MAX_RUN );
				encode_ari( enc, model, 0 );
				model->shift_context( 0 );
				run -= PADDING_MAX_RUN;
			}
			encode_ari( enc, model, run );
			model->shift_context( run );
			run = 0;
		}
		run++;
	}
	// encode last run
	if ( run > 0 ) {
		while ( run >= PADDING_MAX_RUN+1 ) {
			encode_ari( enc, model, PADDING_MAX_RUN );
			model->shift_context( PADDING_MAX_RUN );
			encode_ari( enc, model, 0 );
			model->shift_context( 0 );
			run -= PADDING_MAX_RUN;
		}
		encode_ari( enc, model, run );
	}
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	decode the padding bit (1 bit)
	----------------------------------------------- */
INTERN inline bool pmp_decode_padding( aricoder* dec )
{
	model_s* model;
	int run = 0;
	char bit = 0;
	
	
	// decoding start
	model = INIT_MODEL_S( PADDING_MAX_RUN+1, PADDING_MAX_RUN+1, 1 );
	// decode the first run
	run = decode_ari( dec, model );
	model->shift_context( run );
	// ... and all others
	for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next ) {
		while ( run == 0 ) {
			bit ^= 0x1;
			run = decode_ari( dec, model );
			model->shift_context( run );			
		}
		run--;
		frame->padding = bit;
	}
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	encode block types (1-3 bit)
	----------------------------------------------- */
INTERN inline bool pmp_encode_block_types( aricoder* enc )
{
	// block types:
	// - consist of two things: switching bit and block type spec
	// - safe for the first few frames, block type spec is predictable
	// - switching occurs periodically
	// - ch1 block types are often identical to ch0 block types
	// -> use run length encoding for switching and predict specs
	
	granuleInfo* granule;
	model_s* mod_sw;
	model_s* mod_bt;
	int run = 0;
	char bit = 0;
	int ctx;
	int c;
	
	
	// init models
	mod_sw = INIT_MODEL_S( SWITCHING_MAX_RUN+1, SWITCHING_MAX_RUN+1, 1 );
	mod_bt = INIT_MODEL_S( 4, 4, 1 );
	
	// encoding start
	for ( int ch = 0; ch < g_nchannels; ch++ ) {
		if ( ( i_sb_diff == 0 ) && ( ch == 1 ) ) break;	
		mod_sw->shift_context( 0 ); run = 0;
		// first step - encode the switching runs
		for ( bit = 0, granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next ) {
			// encode the switching run
			if ( granule->window_switching != bit ) {
				bit ^= 0x1;
				while ( run >= SWITCHING_MAX_RUN+1 ) {
					encode_ari( enc, mod_sw, SWITCHING_MAX_RUN );
					mod_sw->shift_context( SWITCHING_MAX_RUN );
					encode_ari( enc, mod_sw, 0 );
					mod_sw->shift_context( 0 );
					run -= SWITCHING_MAX_RUN;
				}
				encode_ari( enc, mod_sw, run );
				mod_sw->shift_context( run );
				run = 0;
			}
			run++;
		}
		if ( run > 0 ) { // encode last run
			while ( run >= SWITCHING_MAX_RUN+1 ) {
				encode_ari( enc, mod_sw, SWITCHING_MAX_RUN );
				mod_sw->shift_context( SWITCHING_MAX_RUN );
				encode_ari( enc, mod_sw, 0 );
				mod_sw->shift_context( 0 );
				run -= SWITCHING_MAX_RUN;
			}
			encode_ari( enc, mod_sw, run );
			mod_sw->shift_context( run );
		}		
		// second step - encode the block types
		c = STOP_BLOCK;
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next ) {
			if ( granule->window_switching ) {
				if ( c == STOP_BLOCK ) ctx = START_BLOCK;
				else if ( granule->next != NULL )
					ctx = ( granule->next->window_switching ) ? SHORT_BLOCK : STOP_BLOCK;
				else ctx = STOP_BLOCK;
				mod_bt->shift_context( ctx );
				c = granule->block_type;
				encode_ari( enc, mod_bt, c );				
			}
		}
	}

	// done, delete models
	delete( mod_sw );
	delete( mod_bt );
	
	
	return true;
}


/* -----------------------------------------------
	decode block types (1-3 bit)
	----------------------------------------------- */
INTERN inline bool pmp_decode_block_types( aricoder* dec )
{
	granuleInfo* granule0;
	granuleInfo* granule1;
	model_s* mod_sw;
	model_s* mod_bt;
	char bit = 0;
	int run;
	int ctx;
	int c;
	
	
	// init models
	mod_sw = INIT_MODEL_S( SWITCHING_MAX_RUN+1, SWITCHING_MAX_RUN+1, 1 );
	mod_bt = INIT_MODEL_S( 4, 4, 1 );
	
	// decoding start
	for ( int ch = 0; ch < g_nchannels; ch++ ) {
		if ( ( i_sb_diff == 0 ) && ( ch == 1 ) ) break;
		mod_sw->shift_context( 0 );
		// first step - decode the switching runs
		// decode the first run
		run = decode_ari( dec, mod_sw );
		mod_sw->shift_context( run );
		// ... and all others
		for ( bit = 0, granule0 = firstframe->granules[ch][0]; granule0 != NULL; granule0 = granule0->next ) {
			while ( run == 0 ) {
				bit ^= 0x1;
				run = decode_ari( dec, mod_sw );
				mod_sw->shift_context( run );			
			}
			run--;
			granule0->window_switching = bit;
		}
		// second step - decode the block types
		c = STOP_BLOCK;
		for ( granule0 = firstframe->granules[ch][0]; granule0 != NULL; granule0 = granule0->next ) {
			if ( granule0->window_switching ) {
				if ( c == STOP_BLOCK ) ctx = START_BLOCK;
				else if ( granule0->next != NULL )
					ctx = ( granule0->next->window_switching ) ? SHORT_BLOCK : STOP_BLOCK;
				else ctx = STOP_BLOCK;
				mod_bt->shift_context( ctx );
				c = decode_ari( dec, mod_bt );
				granule0->block_type = c;
			} else granule0->block_type = LONG_BLOCK;
		}
	}

	// done, delete model
	delete( mod_sw );
	delete( mod_bt );
	
	// copy ch1 block types to ch0 if identical
	if ( ( i_sb_diff == 0 ) && ( g_nchannels == 2 ) ) {
		granule1 = firstframe->granules[1][0];
		for ( granule0 = firstframe->granules[0][0]; granule0 != NULL; granule0 = granule0->next ) {
			granule1->window_switching = granule0->window_switching;
			granule1->block_type = granule0->block_type;
			granule1 = granule1->next;
		}
	}
	
	
	return true;
}


/* -----------------------------------------------
	encode the global gain (8 bit)
	----------------------------------------------- */
INTERN inline bool pmp_encode_global_gain( aricoder* enc )
{
	// global gain:
	// - high correlation with everything else
	// - high noise
	// - difficult to predict from each other
	// -> encode using differential coding and 0th order model
	// this will serve as context for everything else
	
	granuleInfo* granule0;
	granuleInfo* granule1;
	model_s* model;
	int last = 0;
	int c = 0;
	
	
	// set up model (will use one model for both channels)
	model = INIT_MODEL_S( 256, 0, 0 );
	
	// --- first channel ---
	// model->shift_context( 0 );
	for ( granule0 = firstframe->granules[0][0]; granule0 != NULL; granule0 = granule0->next ) {
		c = ( granule0->global_gain - last ) & 0xFF;
		last = granule0->global_gain;
		encode_ari( enc, model, c );
	}
	
	// --- second channel ---
	if ( g_nchannels == 2 ) {
		// model->shift_context( 0 );
		granule0 = firstframe->granules[0][0];
		for ( granule1 = firstframe->granules[1][0]; granule1 != NULL; granule1 = granule1->next ) {
			c = ( granule1->global_gain - granule0->global_gain ) & 0xFF;
			granule0 = granule0->next;
			encode_ari( enc, model, c );
		}
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	decode the global gain (8 bit)
	----------------------------------------------- */
INTERN inline bool pmp_decode_global_gain( aricoder* dec )
{
	granuleInfo* granule0;
	granuleInfo* granule1;
	model_s* model;
	int last = 0;
	int c = 0;
	
	
	// set up model (will use one model for both channels)
	model = INIT_MODEL_S( 256, 0, 0 );
	
	// --- first channel ---
	// model->shift_context( 0 );
	for ( granule0 = firstframe->granules[0][0]; granule0 != NULL; granule0 = granule0->next ) {
		c = decode_ari( dec, model );
		// model->shift_context( c );
		last = ( c + last ) & 0xFF;
		granule0->global_gain = last;
	}
	
	// --- second channel ---
	if ( g_nchannels == 2 ) {
		// model->shift_context( 0 );
		granule0 = firstframe->granules[0][0];
		for ( granule1 = firstframe->granules[1][0]; granule1 != NULL; granule1 = granule1->next ) {
			c = decode_ari( dec, model );
			// model->shift_context( c );
			granule1->global_gain = ( c + granule0->global_gain ) & 0xFF;
			granule0 = granule0->next;
		}
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	encode the slength (4 bit)
	----------------------------------------------- */
INTERN inline bool pmp_encode_slength( aricoder* enc )
{
	// slength:
	// - high correlation with global gain
	// - different correlation for ch0 and ch1
	// - predictable from each other to some degree
	// -> encode using gg_context and markov model
	
	unsigned char* gg_ctx;
	granuleInfo* granule;
	model_s* model;
	int ch;
	int c;
	
	
	// set up model. LSF stores the 9-bit scalefac_compress (0..511); MPEG-1 the
	// 4-bit slength (0..15). One model for both channels, flush in between.
	const bool lsf = ( i_mpeg != MP3_V1_0 );
	const int  nsym = lsf ? 512 : 16;
	model = INIT_MODEL_S( nsym, (GG_CONTEXT_SIZE > nsym ) ? GG_CONTEXT_SIZE : nsym, 2 );

	// encoding start
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// reset and flush model
		model->flush_model( 1 );
		gg_ctx = gg_context[ ch ]; c = 0;
		// encode one channel
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next ) {
			shift_model( model, *(gg_ctx++), c );
			c = lsf ? granule->scalefac_compress : granule->slength;
			encode_ari( enc, model, c );
		}
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	decode the slength (4 bit)
	----------------------------------------------- */
INTERN inline bool pmp_decode_slength( aricoder* dec )
{
	unsigned char* gg_ctx;
	granuleInfo* granule;
	model_s* model;
	int ch;
	int c;
	
	
	// set up model (mirror of encoder: 512 symbols for LSF scalefac_compress).
	const bool lsf = ( i_mpeg != MP3_V1_0 );
	const int  nsym = lsf ? 512 : 16;
	model = INIT_MODEL_S( nsym, (GG_CONTEXT_SIZE > nsym ) ? GG_CONTEXT_SIZE : nsym, 2 );

	// decoding start
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// reset and flush model
		model->flush_model( 1 );
		gg_ctx = gg_context[ ch ]; c = 0;
		// decode one channel
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next ) {
			shift_model( model, *(gg_ctx++), c );
			c = decode_ari( dec, model );
			if ( lsf ) granule->scalefac_compress = c; else granule->slength = c;
		}
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	encode ms stereo setting (1 bit)
	----------------------------------------------- */
INTERN inline bool pmp_encode_stereo_ms( aricoder* enc )
{
	// stereo ms bit:
	// - only occurs in joint stereo mode
	// - predictable from each other to some degree
	// -> encode using markov model
	
	model_b* model;
	unsigned char ctx = 0;
	
	
	// encoding start
	model = INIT_MODEL_B( 16, 1 );
	for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next ) {
		model->shift_context( ctx );
		encode_ari( enc, model, frame->stereo_ms );
		ctx = ( ( ctx << 1 ) | frame->stereo_ms ) & 0xF;
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	decode ms stereo setting (1 bit)
	----------------------------------------------- */
INTERN inline bool pmp_decode_stereo_ms( aricoder* dec )
{
	model_b* model;
	unsigned char ctx = 0;
	
	
	// decoding start
	model = INIT_MODEL_B( 16, 1 );
	for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next ) {
		model->shift_context( ctx );
		frame->stereo_ms = decode_ari( dec, model );
		ctx = ( ( ctx << 1 ) | frame->stereo_ms ) & 0xF;
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	encode region bounds and tables (32 bit)
	----------------------------------------------- */
INTERN inline bool pmp_encode_region_data( aricoder* enc )
{
	// region bounds:
	// - little correlation with global gain
	// - high correlation among each others
	// - no good context for bv bounds at all
	// - sv bounds have to be processed elsewhere
	// -> different treatment for r0, r1 and bv:
	// -> r0: markov model and bv context
	// -> r1: r0 and bv context
	// -> bv: block type context
	//
	// huffman table selection:
	// - high correlation with global gain
	// - high correlation between neighbors and tables
	// -> encode using gg_context and markov model
	// -> each region gets its own model

	
	const int* bw_conv = mp3_bandwidth_conv[ (int) i_samplerate ];
	unsigned char* gg_ctx;
	granuleInfo* granule;
	model_s* mod_t0;
	model_s* mod_t1;
	model_s* mod_t2;
	model_b* mod_ts;
	model_s* mod_s0;
	model_s* mod_s1;
	model_s* mod_bv;
	int t_r0, t_r1, t_r2, t_sv;
	int s_r0, s_r2;
	// int s_r0, s_r1, s_r2;
	int ctx_sv;
	int ch;
	
	
	// set up models (one for each region table and size)
	mod_t0 = INIT_MODEL_S( 32, (GG_CONTEXT_SIZE > 32 ) ? GG_CONTEXT_SIZE : 32, 2 );
	mod_t1 = INIT_MODEL_S( 32, 32, 2 );
	mod_t2 = INIT_MODEL_S( 32, 32, 2 );
	mod_ts = INIT_MODEL_B( 16, 1 );
	mod_s0 = INIT_MODEL_S( 16, 22, 2 );
	mod_s1 = INIT_MODEL_S( 8, 22, 2 );
	mod_bv = INIT_MODEL_S( (576/2)+1, 1+1, 1 );
	
	// encoding start
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// reset and flush models
		mod_s0->flush_model( 1 );
		mod_s1->flush_model( 1 );
		mod_bv->flush_model( 1 );
		// reset context
		gg_ctx = gg_context[ ch ]; ctx_sv = 0;
		t_r0 = 0; t_r1 = 0; t_r2 = 0; t_sv = 0;
		// s_r0 = 0; s_r1 = 0; s_r2 = 0;
		s_r0 = 0; s_r2 = 0;
		// encode all region bounds and tables for one channel
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next ) {
			// --- region bounds (big_val_pairs, region0_size, region2_size) ---
			if ( granule->big_val_pairs > 576 / 2 ) {
				snprintf( errormessage, MSG_SIZE, "big value pairs out of bounds (%ch>%ch)", granule->big_val_pairs, 576 / 2 );
				errorlevel = 2;
				return false;
			}
			if ( granule->window_switching ) {
				// region 2 size (# big value pairs)
				mod_bv->shift_context( ( granule->block_type == SHORT_BLOCK ) ? 1 : 0 );
				encode_ari( enc, mod_bv, granule->big_val_pairs );
				s_r0 = 0; // s_r1 = 0;
			} else {
				s_r2 = bw_conv[ granule->big_val_pairs << 1 ];
				// region 2 size (# big value pairs)
				mod_bv->shift_context( 0 );
				encode_ari( enc, mod_bv, granule->big_val_pairs );
				// region 0 size
				shift_model( mod_s0, s_r0, s_r2 );
				s_r0 = granule->region0_size;
				encode_ari( enc, mod_s0, s_r0 );
				// region 1 size
				shift_model( mod_s1, s_r0, s_r2 );
				encode_ari( enc, mod_s1, granule->region1_size );
				// context customizations
				s_r0++; // s_r1 = s_r0 + granule->region1_size + 1;
			}
			// --- region tables (region0/1/2_table, select_htabB) ---
			// region 0 table
			shift_model( mod_t0, *gg_ctx, t_r0 );
			// shift_model( mod_t0, s_r0, t_r0 );
			t_r0 = granule->region_table[0];
			encode_ari( enc, mod_t0, t_r0 );
			// region 1 table
			shift_model( mod_t1, t_r0, s_r0 );
			t_r1 = granule->region_table[1];
			encode_ari( enc, mod_t1, t_r1 );
			// region 2 table
			if ( !granule->window_switching ) {
				shift_model( mod_t2, t_r0, t_r1 );
				t_r2 = granule->region_table[2];
				encode_ari( enc, mod_t2, t_r2 );
			}
			// small values table
			mod_ts->shift_context( ctx_sv );
			t_sv = granule->select_htabB;
			encode_ari( enc, mod_ts, t_sv );
			ctx_sv = ( ( ctx_sv << 1 ) | t_sv ) & 0xF;
			// advance context
			gg_ctx++;
		}
	}
	
	// done, delete models
	delete( mod_t0 );
	delete( mod_t1 );
	delete( mod_t2 );
	delete( mod_ts );
	delete( mod_s0 );
	delete( mod_s1 );
	delete( mod_bv );
	
	
	return true;
}


/* -----------------------------------------------
	decode region sizes and table selection (32 bit)
	----------------------------------------------- */
INTERN inline bool pmp_decode_region_data( aricoder* dec )
{	
	const int* bw_conv = mp3_bandwidth_conv[ (int) i_samplerate ];
	unsigned char* gg_ctx;
	granuleInfo* granule;
	model_s* mod_t0;
	model_s* mod_t1;
	model_s* mod_t2;
	model_b* mod_ts;
	model_s* mod_s0;
	model_s* mod_s1;
	model_s* mod_bv;
	int t_r0, t_r1, t_r2, t_sv;
	int s_r0, s_r2;
	// int s_r0, s_r1, s_r2;
	int ctx_sv;
	int ch;
	
	
	// set up models (one for each region table and size)
	mod_t0 = INIT_MODEL_S( 32, (GG_CONTEXT_SIZE > 32 ) ? GG_CONTEXT_SIZE : 32, 2 );
	mod_t1 = INIT_MODEL_S( 32, 32, 2 );
	mod_t2 = INIT_MODEL_S( 32, 32, 2 );
	mod_ts = INIT_MODEL_B( 16, 1 );
	mod_s0 = INIT_MODEL_S( 16, 22, 2 );
	mod_s1 = INIT_MODEL_S( 8, 22, 2 );
	mod_bv = INIT_MODEL_S( (576/2)+1, 1+1, 1 );
	
	// decoding start
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// reset and flush model
		mod_s0->flush_model( 1 );
		mod_s1->flush_model( 1 );
		mod_bv->flush_model( 1 );
		// reset context
		gg_ctx = gg_context[ ch ]; ctx_sv = 0;
		t_r0 = 0; t_r1 = 0; t_r2 = 0; t_sv = 0;
		// s_r0 = 0; s_r1 = 0; s_r2 = 0;
		s_r0 = 0; s_r2 = 0;
		// decode all region tables for one channel
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next ) {
			// --- region bounds (big_val_pairs, region0_size, region2_size) ---
			if ( !granule->window_switching ) {
				// region 2
				mod_bv->shift_context( 0 );
				granule->big_val_pairs = decode_ari( dec, mod_bv );
				s_r2 = bw_conv[ granule->big_val_pairs << 1 ];
				// region 0
				shift_model( mod_s0, s_r0, s_r2 );
				s_r0 = decode_ari( dec, mod_s0 );
				granule->region0_size = s_r0;
				// region 1
				shift_model( mod_s1, s_r0, s_r2 );
				granule->region1_size = decode_ari( dec, mod_s1 );
				// set region 1/2 bounds
				granule->region_bound[0] =
					bandwidth_bounds[(int)i_mpeg][(int)i_samplerate][s_r0+1];
				granule->region_bound[1] =
					bandwidth_bounds[(int)i_mpeg][(int)i_samplerate][s_r0+granule->region1_size+2];
				// set bv bound and check other bounds
				granule->region_bound[2] = CLAMPED( 0, 576, granule->big_val_pairs << 1 );
				if ( granule->region_bound[0] > granule->region_bound[2] ) {
					granule->region_bound[0] = granule->region_bound[2];
					granule->region_bound[1] = granule->region_bound[2];
				} else if ( granule->region_bound[1] > granule->region_bound[2] )
					granule->region_bound[1] = granule->region_bound[2];
				// context customizations
				s_r0++; // s_r1 = s_r0 + granule->region1_size + 1;
			} else if ( granule->block_type != SHORT_BLOCK ) {
				// region sizes for different block types
				granule->region0_size = 8;
				granule->region1_size = 0;
				// set region bound
				granule->region_bound[0] =
					bandwidth_bounds[(int)i_mpeg][(int)i_samplerate][8];
				// decode bv pairs, set bound and check r0 bound
				mod_bv->shift_context( 0 );
				granule->big_val_pairs = decode_ari( dec, mod_bv );
				granule->region_bound[1] = CLAMPED( 0, 576, granule->big_val_pairs << 1 );
				if ( granule->region_bound[0] > granule->region_bound[1] )
					granule->region_bound[0] = granule->region_bound[1];
				granule->region_bound[2] = granule->region_bound[1];
				// context setting
				s_r0 = 0; // s_r1 = 0;
			} else { // special treatment for mixed blocks needed (!)
				granule->region0_size = 9;
				granule->region1_size = 0;
				// set region bound
				granule->region_bound[0] =
					bandwidth_bounds_short[(int)i_mpeg][(int)i_samplerate][9/3] * 3;
				// decode bv pairs, set bound and check r0 bound
				mod_bv->shift_context( 1 );
				granule->big_val_pairs = decode_ari( dec, mod_bv );
				granule->region_bound[1] = CLAMPED( 0, 576, granule->big_val_pairs << 1 );
				if ( granule->region_bound[0] > granule->region_bound[1] )
					granule->region_bound[0] = granule->region_bound[1];
				granule->region_bound[2] = granule->region_bound[1];
				// context setting
				s_r0 = 0; // s_r1 = 0;
			}
			// --- region tables (region0/1/2_table, select_htabB) ---
			// region 0 table
			shift_model( mod_t0, *gg_ctx, t_r0 );
			t_r0 = decode_ari( dec, mod_t0 );
			granule->region_table[0] = t_r0;
			// region 1 table
			shift_model( mod_t1, t_r0, s_r0 );
			t_r1 = decode_ari( dec, mod_t1 );
			granule->region_table[1] = t_r1;
			// region 2 table
			if ( !granule->window_switching ) {
				shift_model( mod_t2, t_r0, t_r1 );
				t_r2 = decode_ari( dec, mod_t2 );
				granule->region_table[2] = t_r2;
			} else granule->region_table[2] = 0;
			// small values table
			mod_ts->shift_context( ctx_sv );
			t_sv = decode_ari( dec, mod_ts );
			granule->select_htabB = t_sv;
			ctx_sv = ( ( ctx_sv << 1 ) | t_sv ) & 0xF;
			// advance context
			gg_ctx++;
		}
	}
	
	// done, delete models
	delete( mod_t0 );
	delete( mod_t1 );
	delete( mod_t2 );
	delete( mod_ts );
	delete( mod_s0 );
	delete( mod_s1 );
	delete( mod_bv );
	
	
	return true;
}

/* -----------------------------------------------
	encode scalefactor sharing (2/4 bit)
	----------------------------------------------- */
INTERN inline bool pmp_encode_sharing( aricoder* enc )
{
	// scalefactor sharing:
	// - comes in packs of four
	// - only every second granule matters
	// - high correlation with slength
	// - high correlation among fourpacks
	// -> encode using markov model + slength context
	
	granuleInfo* granule;
	model_s* model;
	int ch;
	int c;
	
	
	// set up model (one model for both channels, flush in between)
	model = INIT_MODEL_S( 16, 16, 3 );
	
	// encoding start
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// reset and flush model
		model->flush_model( 1 );
		c = 0;
		// encode one channel
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next->next ) {
			shift_model( model, c, granule->slength, granule->next->slength );
			c = granule->share;
			encode_ari( enc, model, c );
		}
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	decode scalefactor sharing (2/4 bit)
	----------------------------------------------- */
INTERN inline bool pmp_decode_sharing( aricoder* dec )
{
	granuleInfo* granule;
	model_s* model;
	int ch;
	int c;
	
	
	// set up model (one model for both channels, flush in between)
	model = INIT_MODEL_S( 16, 16, 3 );
	
	// decoding start
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// reset and flush model
		model->flush_model( 1 );
		c = 0;
		// decode one channel
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next->next ) {
			shift_model( model, c, granule->slength, granule->next->slength );
			c = decode_ari( dec, model );
			granule->share = c;
		}
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	encode the preemphasis setting (1 bit)
	----------------------------------------------- */
INTERN inline bool pmp_encode_preemphasis( aricoder* enc )
{
	// preemphasis:
	// - little correlation with global gain
	// - near impossible to predict from each other
	// - high differences between ch0 and ch1 (joint)
	// -> encode using simple markov model
	// -> improve later
	
	granuleInfo* granule;
	model_b* model;
	int ctx;
	int ch;
	int c;
	
	
	// set up model (one model for both channels, flush in between)
	model = INIT_MODEL_B( 16, 1 );
	
	// encoding start
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// reset and flush model
		model->flush_model( 1 );
		ctx = 0;
		// encode one channel
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next ) {
			model->shift_context( ctx );
			c = granule->preemphasis;
			encode_ari( enc, model, c );
			ctx = ( ( ctx << 1 ) | c ) & 0xF;
		}
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	decode the preemphasis setting (1 bit)
	----------------------------------------------- */
INTERN inline bool pmp_decode_preemphasis( aricoder* dec )
{
	granuleInfo* granule;
	model_b* model;
	int ctx;
	int ch;
	int c;
	
	
	// set up model (one model for both channels, flush in between)
	model = INIT_MODEL_B( 16, 1 );
	
	// decoding start
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// reset and flush model
		model->flush_model( 1 );
		ctx = 0;
		// decode one channel
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next ) {
			model->shift_context( ctx );
			c = decode_ari( dec, model );
			granule->preemphasis = c;
			ctx = ( ( ctx << 1 ) | c ) & 0xF;
		}
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	encode the coarse sf setting (1 bit)
	----------------------------------------------- */
INTERN inline bool pmp_encode_coarse_sf( aricoder* enc )
{
	// coarse scalefactors
	// - little correlation with global gain
	// - near impossible to predict from each other
	// - differences between ch0 and ch1 (joint)
	// -> encode using simple markov model
	// -> improve later
	
	granuleInfo* granule;
	model_b* model;
	int ctx;
	int ch;
	int c;
	
	
	// set up model (one model for both channels, flush in between)
	model = INIT_MODEL_B( 16, 1 );
	
	// encoding start
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// reset and flush model
		model->flush_model( 1 );
		ctx = 0;
		// encode one channel
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next ) {
			model->shift_context( ctx );
			c = granule->coarse_scalefactors;
			encode_ari( enc, model, c );
			ctx = ( ( ctx << 1 ) | c ) & 0xF;
		}
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	decode the coarse sf setting (1 bit)
	----------------------------------------------- */
INTERN inline bool pmp_decode_coarse_sf( aricoder* dec )
{
	granuleInfo* granule;
	model_b* model;
	int ctx;
	int ch;
	int c;
	
	
	// set up model (one model for both channels, flush in between)
	model = INIT_MODEL_B( 16, 1 );
	
	// decoding start
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// reset and flush model
		model->flush_model( 1 );
		ctx = 0;
		// decode one channel
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next ) {
			model->shift_context( ctx );
			c = decode_ari( dec, model );
			granule->coarse_scalefactors = c;
			ctx = ( ( ctx << 1 ) | c ) & 0xF;
		}
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	encode the subblock gain (9 bit)
	----------------------------------------------- */
INTERN inline bool pmp_encode_subblock_gain( aricoder* enc )
{
	// subblock gain:
	// - only occurs for short blocks
	// - little correlation with each other
	// - no visible correlation with anything else
	// -> use simple markov model
	
	granuleInfo* granule;
	model_s* model;
	int ch, sb;
	
	
	// set up model (one model for both channels, flush in between)
	model = INIT_MODEL_S( 8, 8, 1 );
	
	// decoding start
	model->shift_context( 0 );
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// encode one channel
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next ) {
			if ( granule->window_switching ) {
				for ( sb = 0; sb < 3; sb++ ) {
					encode_ari( enc, model, granule->sb_gain[sb] );
					model->shift_context( granule->sb_gain[sb] );
				}
			}
		}
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	decode the subblock gain (9 bit)
	----------------------------------------------- */
INTERN inline bool pmp_decode_subblock_gain( aricoder* dec )
{
	granuleInfo* granule;
	model_s* model;
	int ch, sb;
	
	
	// set up model (one model for both channels, flush in between)
	model = INIT_MODEL_S( 8, 8, 1 );
	
	// decoding start
	model->shift_context( 0 );
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// decode one channel
		for ( granule = firstframe->granules[ch][0]; granule != NULL; granule = granule->next ) {
			if ( granule->window_switching ) {
				for ( sb = 0; sb < 3; sb++ ) {
					granule->sb_gain[sb] = decode_ari( dec, model );
					model->shift_context( granule->sb_gain[sb] );
				}
			} else memset( granule->sb_gain, 0, sizeof( char ) * 3 );
		}
	}
	
	// done, delete model
	delete( model );
	
	
	return true;
}


/* -----------------------------------------------
	encode the main data
	----------------------------------------------- */
INTERN inline bool pmp_encode_main_data( aricoder* enc )
{
	// main data:
	// consists of scalefactors & coefficients
	// several other data is also encoded here:
	// - sv bound (for performance reasons)
	// - aux data, aux data size & padding bits
	// - reconstruction information (stuffing bits)
	// - bitrate (for performance reasons)
	//
	// scalefactors:
	// correlation with temporal and local neighbours
	// -> encode with neighborhood context
	//
	// coefficients:
	// good routines are already included in mp3 standard,
	// but no temporal context is used there
	// -> remodel mp3 coding with arithmetic coding
	// -> use neighborhood context
	//
	// stuffing bits (between granules):
	// this data either follows a pattern or is garbage
	// -> use 4 bit prev bits context
	//
	// padding bits/aux data (at end of frame):
	// usually follows a specific pattern
	// -> try prediction
	// -> use 8 bit prev bits context
	//
	// sv bound:
	// should be predictable from neighborhood
	// and bv bound
	// -> encode diff with bv bound
	// -> use prev and bv bound context
	//
	// bitrate:
	// high correlation with main size
	// -> encode using main size prediction as context
	
	// context / storage
	static thread_local unsigned char* pad_and_aux= ( unsigned char* ) calloc( 2048, 1 ); // !!! (length)
	mp3Frame* frame;
	granuleInfo* granule;
	unsigned char* scf_c[2];
	unsigned char* scf_l_long[2];
	unsigned char* scf_l_short[2];
	unsigned char* abs_c[2];
	unsigned char* sgn_c[2];
	unsigned char* len_c[2];
	unsigned short* lbt_c[2];
	unsigned char* absl_ctx_h[2];
	unsigned char* abss_ctx_h[2];
	unsigned char* sgnl_ctx_h[2];
	unsigned char* sgns_ctx_h[2];
	unsigned char* lenl_ctx_h[2];
	unsigned char* lens_ctx_h[2];
	unsigned char* scf_prev;
	unsigned char* ctx_h_abs;
	unsigned char* ctx_h_sgn;
	unsigned char* ctx_h_len;
	unsigned char* scf;
	unsigned char* abs;
	unsigned char* sgn;
	unsigned char* len;
	unsigned short* lbt;
	unsigned char* swap;
	unsigned char* pna_c;
	unsigned char ctx_scf;
	unsigned char ctx_abs;
	unsigned char ctx_pat;
	unsigned char ctx_svb[2] = { 0, 0 };
	unsigned char ctx_nst = 0;
	unsigned char ctx_bst = 0;
	unsigned char ctx_aux = 0;
	unsigned char ctx_pad = 0;
	// statistical models
	model_s* mod_scf[2][5][10]; // scalefactors
	model_s* mod_abv[2][8][32]; // absolutes big values <= 15
	model_b* mod_asv[2][8][2]; // absolulte small values
	model_b* mod_sgn[2][8]; // signs
	model_s* mod_len[2][14]; // residual bitlengths
	model_b* mod_res; // residual bits
	model_s* mod_svb; // small values bound
	model_s* mod_bvf; // fix for sv bound (usually none)
	model_s* mod_nst; // number of stuffing bits (usually zero)
	model_b* mod_bst; // stuffing bits
	model_b* mod_pad; // padding and ancillary bits
	model_b* mod_pap; // predcition for p&a bits
	model_s* mod_aux; // byte size of ancillary data
	model_s* mod_btr; // frame bitrate
	model_s* mod_abc; // current absolute big values (shortcut)
	model_b* mod_asc; // current absolute small values (shortcut)
	model_b* mod_sgc; // current signs (shortcut)
	model_s* mod_lnc; // current bitlengths (shortcut)
	model_s* mod_sfc; // current scalefactors (shortcut)
	// lookup tables
	const int* bitrate_pred = mp3_bitrate_pred[(int)i_samplerate];
	// huffman decoder
	huffman_reader* huffman;
	// mp3 decoding settings
	short* region_bounds;
	char* region_tables;
	huffman_dec_table* bv_table;
	huffman_conv_set* conv_set;
	int linbits;
	const int* slen;
	int rlb, sl;	
	// general settings
	int bitp = 0;
	int bitc = 0;
	int lmaxp = 0;
	int sbl = 0;
	int flags = 0;
	bool j_coding;
	bool bad_coding = false;
	char shared[2][4];
	// counters
	int ch, gr;
	int p, g, r;
	int i, c, n;
	
	
	// --- PRE-PROCESSING PREPARATIONS: INIT STATISTICAL MODELS/HUFFMAN DECODER/CONTEXT ---
	// decide if joint context will be used (will be used for joint & standard stereo)
	j_coding = ( i_channels == MP3_STEREO ) || ( i_channels == MP3_JOINT_STEREO );
	// init huffman decoder/reader
	huffman = new huffman_reader( main_data, main_data_size );
	// reset ancillary predictor
	pmp_predict_lame_anc( -1, NULL );
	// init statistical models
	mod_svb = INIT_MODEL_S( ( 576 / 4 ) + 1 + 1, ( 576 / 4 ) + 1 + 1, 1 );
	mod_bvf = INIT_MODEL_S( ( 576 / 2 ) + 1, 0, 0 );
	mod_nst = INIT_MODEL_S( STUFFING_STEP + 1, STUFFING_STEP + 1, 1 );
	mod_bst = INIT_MODEL_B( 16, 1 );
	mod_pad = INIT_MODEL_B( 256, 1 );
	mod_pap = INIT_MODEL_B( 0, 0 );
	mod_aux = INIT_MODEL_S( AUX_DATA_STEP + 1, AUX_DATA_STEP + 1, 1 );
	mod_btr = INIT_MODEL_S( 16, 16, 1 );
	mod_res = INIT_MODEL_B( 13 + 1, 2 );
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( g = 0; g < 10; g++ ) {
			mod_scf[ch][0][g] = INIT_MODEL_S(  2, 16, 2 );
			mod_scf[ch][1][g] = INIT_MODEL_S(  4, 16, 2 );
			mod_scf[ch][2][g] = INIT_MODEL_S(  8, 16, 2 );
			mod_scf[ch][3][g] = INIT_MODEL_S( 16, 16, 2 );
				mod_scf[ch][4][g] = INIT_MODEL_S( 32, 16, 2 ); // LSF intensity-stereo scf_size==5
		}
		for ( flags = 0x0; flags < ( (j_coding&&ch) ? 0x8 : 0x2 ); flags++ ) {
			mod_abv[ch][flags][ 0] = NULL;
			mod_abv[ch][flags][ 1] = INIT_MODEL_S(  1 + 1, 16, 2 );
			mod_abv[ch][flags][ 2] = INIT_MODEL_S(  2 + 1, 16, 2 );
			mod_abv[ch][flags][ 3] = INIT_MODEL_S(  2 + 1, 16, 2 );
			mod_abv[ch][flags][ 4] = NULL;
			mod_abv[ch][flags][ 5] = INIT_MODEL_S(  3 + 1, 16, 2 );
			mod_abv[ch][flags][ 6] = INIT_MODEL_S(  3 + 1, 16, 2 );
			mod_abv[ch][flags][ 7] = INIT_MODEL_S(  5 + 1, 16, 2 );
			mod_abv[ch][flags][ 8] = INIT_MODEL_S(  5 + 1, 16, 2 );
			mod_abv[ch][flags][ 9] = INIT_MODEL_S(  5 + 1, 16, 2 );
			mod_abv[ch][flags][10] = INIT_MODEL_S(  7 + 1, 16, 2 );
			mod_abv[ch][flags][11] = INIT_MODEL_S(  7 + 1, 16, 2 );
			mod_abv[ch][flags][12] = INIT_MODEL_S(  7 + 1, 16, 2 );
			mod_abv[ch][flags][13] = INIT_MODEL_S( 15 + 1, 16, 2 );
			mod_abv[ch][flags][14] = NULL;
			mod_abv[ch][flags][15] = INIT_MODEL_S( 15 + 1, 16, 2 );
			mod_abv[ch][flags][16] = INIT_MODEL_S( 15 + 1, 16, 2 );
			mod_abv[ch][flags][17] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][18] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][19] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][20] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][21] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][22] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][23] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][24] = INIT_MODEL_S( 15 + 1, 16, 2 );
			mod_abv[ch][flags][25] = mod_abv[ch][flags][24];
			mod_abv[ch][flags][26] = mod_abv[ch][flags][24];
			mod_abv[ch][flags][27] = mod_abv[ch][flags][24];
			mod_abv[ch][flags][28] = mod_abv[ch][flags][24];
			mod_abv[ch][flags][29] = mod_abv[ch][flags][24];
			mod_abv[ch][flags][30] = mod_abv[ch][flags][24];
			mod_abv[ch][flags][31] = mod_abv[ch][flags][24];
			mod_asv[ch][flags][0] = INIT_MODEL_B( 16, 2 );
			mod_asv[ch][flags][1] = INIT_MODEL_B( 16, 2 );
			mod_sgn[ch][flags] = INIT_MODEL_B( 16, 2 );
		}
		mod_len[ch][0] = NULL;
		for ( i = 1; i <= 13; i++ )
			mod_len[ch][i] = INIT_MODEL_S( i + 1, 13 + 1, 1 );
	}
	
	
	// --- PRE-PROCESSING PREPARATIONS: ALLOCATION OF MEMORY ---
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// alloc memory
		scf_c[ch]       = ( unsigned char* ) calloc(  40, sizeof( char ) ); // 40: LSF short needs up to 36
		scf_l_long[ch]  = ( unsigned char* ) calloc(  40, sizeof( char ) );
		scf_l_short[ch] = ( unsigned char* ) calloc(  40, sizeof( char ) );
		abs_c[ch]       = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		sgn_c[ch]       = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		len_c[ch]       = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		lbt_c[ch]      = ( unsigned short* ) calloc( 576, sizeof( short ) );
		absl_ctx_h[ch]  = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		abss_ctx_h[ch]  = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		sgnl_ctx_h[ch]  = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		sgns_ctx_h[ch]  = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		lenl_ctx_h[ch]  = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		lens_ctx_h[ch]  = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		// check for problems
		if ( ( scf_c[ch] == NULL ) || ( scf_l_long[ch] == NULL ) || ( scf_l_short[ch] == NULL ) ||
			 ( abs_c[ch] == NULL ) || ( absl_ctx_h[ch] == NULL ) || ( abss_ctx_h[ch] == NULL ) ||
			 ( sgn_c[ch] == NULL ) || ( sgnl_ctx_h[ch] == NULL ) || ( sgns_ctx_h[ch] == NULL ) ||
			 ( len_c[ch] == NULL ) || ( lenl_ctx_h[ch] == NULL ) || ( lens_ctx_h[ch] == NULL ) ||
			 ( lbt_c[ch] == NULL ) ) {
			snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
			errorlevel = 2;
			return false;
		}
	}
	
	
	// --- MAIN PROCESSING LOOP: ENCODING AND DECODING ---
	for ( frame = firstframe; frame != NULL; frame = frame->next, bitp = 0 ) {
		for ( gr = 0; gr < mp3_ngr( frame->mpeg ); gr++ ) {
			for ( ch = 0; ch < g_nchannels; ch++ ) {

				// --- MAIN DATA DECODING: PREPARATIONS ---
				// initialize shortcuts
				granule = frame->granules[ch][gr];
				sbl = ( granule->block_type == SHORT_BLOCK ) ? 1 : 0;
				lmaxp = granule->main_data_bit;
				slen = slength_table[ (int) granule->slength ];
				region_bounds = granule->region_bound;
				region_tables = granule->region_table;
				abs = abs_c[ch] + 1;
				sgn = sgn_c[ch] + 1;
				len = len_c[ch] + 1;
				lbt = lbt_c[ch];
				// set compression flags
				flags = ( !j_coding || ( ch == 0 ) ) ? sbl :
					( sbl << 0 ) | // long (0) or short (1) block
					( frame->stereo_ms << 1 ) | // stereo ms on/off
					( ( flags ^ sbl ) << 2 ); // ch0/ch1 block type diffs y/n
				// store sharing info
				if ( gr == 0 ) {
					shared[ch][0] = ( granule->share >> 3 ) & 0x1;
					shared[ch][1] = ( granule->share >> 2 ) & 0x1;
					shared[ch][2] = ( granule->share >> 1 ) & 0x1;
					shared[ch][3] = ( granule->share >> 0 ) & 0x1;
				}
				// reset counter
				huffman->reset_counter();
				bitc = 0;
				bad_coding = false;
				
				
				// ---> SCALEFACTOR PROCESSING <---
				if ( frame->mpeg != MP3_V1_0 ) {

					// --- SCALEFACTOR READING/ENCODING: MPEG-2/2.5 (LSF) ---
					// No sharing, no long/short distinction here: scfcnt[g] values of
					// scfsz[g] bits, linearly. Reuses mod_scf (sl 1-4); sl=5 only
					// occurs under intensity stereo, not yet supported.
					int scfsz[ 4 ], scfcnt[ 4 ], j;
					mp3_lsf_scf_params( frame, granule, ch, scfsz, scfcnt );
					scf = scf_c[ch];
					scf_prev = scf_l_long[ch];
					ctx_scf = 0;
					for ( g = 0, p = 0; g < 4; g++ ) {
						sl = scfsz[ g ];
						if ( sl == 0 ) {
							for ( j = 0; j < scfcnt[ g ]; j++, p++ ) scf[ p ] = 0;
						} else {
							mod_sfc = mod_scf[ch][ sl-1 ][ g ];
							for ( j = 0; j < scfcnt[ g ]; j++, p++ ) {
								scf[ p ] = huffman->read_bits( sl );
								shift_model( mod_sfc, ctx_scf, scf_prev[ p ] );
								encode_ari( enc, mod_sfc, scf[ p ] );
								ctx_scf = scf[ p ];
							}
						}
					}
					swap = scf_c[ch]; scf_c[ch] = scf_l_long[ch]; scf_l_long[ch] = swap;

				} else if ( !sbl ) {

					// --- SCALEFACTOR READING/ENCODING: LONG BLOCKS ---
					scf = scf_c[ch];
					scf_prev = scf_l_long[ch];
					ctx_scf = 0;
					
					// read/encode 21 scalefactors with/without sharing
					for ( g = 0, p = 0; g < 4; g++ ) {
						sl = slen[ ( g < 2 ) ? 0 : 1 ];
						if ( ( gr ) & ( shared[ch][g] ) ) { // shared
							// loop invariant conditions (-funswitch-loops)
							memcpy( scf + p, scf_prev + p, scf_width[ g ] );
							p = scf_bounds[ g ];
							ctx_scf = scf[p-1];
						} else if ( sl == 0 ) { // zero slength
							memset( scf + p, 0, scf_width[ g ] );
							p = scf_bounds[ g ];
							ctx_scf = 0;
						} else {
							mod_sfc = mod_scf[ch][sl-1][(shared[ch][g])?g|4:g];
							for ( ; p < scf_bounds[ g ]; p++ ) { // non shared
								scf[p] = huffman->read_bits( sl );
								shift_model( mod_sfc, ctx_scf, scf_prev[p] );
								encode_ari( enc, mod_sfc, scf[p] );
								ctx_scf = scf[p];
							}
						}
					}
					
					// --- SCALEFACTORS FINISHED: SWAP DATA ---
					swap = scf_c[ch]; scf_c[ch] = scf_l_long[ch]; scf_l_long[ch] = swap;
					
				} else {
					
					// --- SCALEFACTOR READING/ENCODING: SHORT BLOCKS ---
					// encode (only non shared)
					for ( i = 0; i < 3; i++ ) { // 3 subblocks
						scf = scf_c[ch];
						scf_prev = scf_l_short[ch];
						ctx_scf = 0;
						for ( g = 0, p = 0; g < 2; g++ ) { // lo/hi groups
							sl = slen[ g ];
							if ( sl == 0 ) { // zero slength
								memset( scf + p, 0, scf_lh_width_short[ g ] );
								p = scf_lh_bounds_short[ g ];
							} else {
								mod_sfc = mod_scf[ch][sl-1][g|0x8];
								for ( ; p < scf_lh_bounds_short[ g ]; p++ ) {
									scf[p] = huffman->read_bits( sl );
									shift_model( mod_sfc, ctx_scf, scf_prev[p] );
									encode_ari( enc, mod_sfc, scf[p] );
									ctx_scf = scf[p];
								}
							}
						}
						// --- SWAP DATA ---
						swap = scf_c[ch]; scf_c[ch] = scf_l_short[ch]; scf_l_short[ch] = swap;
					}
				}
				
				// sanity check
				if ( huffman->get_count() > lmaxp ) { // main data not big enough - no rollback for scfs
					snprintf( errormessage, MSG_SIZE, "huffman decoding error (in frame #%i)", frame->n );
					errorlevel = 2;
					return false;
				}
				
				
				// ---> COEFFICIENTS PROCESSING <---
				
				// --- COEFFICIENT DECODING: BIG VALUES ---
				for ( p = 0, r = 0; ( r < 3 ) && ( !bad_coding ); r++ ) {
					if ( region_tables[ r ] == 0 ) { // tbl0 skipping
						p = region_bounds[ r ];			
						continue;
					}
					// set table and linbits
					bv_table = bv_dec_table + region_tables[ r ];
					if ( bv_table->h == NULL ) { // illegal table?
						snprintf( errormessage, MSG_SIZE, "bad huffman table (%i) used (in frame #%i)",
							region_tables[ r ], frame->n );
						errorlevel = 2;
						return false;
					}
					conv_set = bv_table->h;
					linbits = bv_table->linbits;
					
					// decoding with/without linbits
					for ( ; p < region_bounds[ r ]; bitc = huffman->get_count() ) {
						huffman->decode_pair( conv_set, abs + p );
						for ( i = 0; i < 2; i++, p++ ) if ( abs[p] > 0 ) {
							if ( linbits > 0 ) if ( abs[p] == 15 ) {
								// loop invariant condition (-unswitch-loops)
								lbt[p] = huffman->read_bits( linbits );
								len[p] = BITLEN8192P(lbt[p]);
							}
							sgn[p] = huffman->read_bit();
						}
						if ( huffman->get_count() > lmaxp ) { // bad coding rollback
							bad_coding = true;
							abs[--p] = 0; sgn[p] = 0; len[p] = 0;
							abs[--p] = 0; sgn[p] = 0; len[p] = 0;
							huffman->rewind_bits( huffman->get_count() - bitc );
							break; // rollback complete
						}
					}
				}
				
				// --- COEFFICIENT DECODING: SMALL VALUES ---
				if ( !bad_coding ) {
					conv_set = ( granule->select_htabB ) ? &htabB_dec : &htabA_dec;
					for ( bitc = huffman->get_count(); p < 576; bitc = huffman->get_count() ) {
						if ( bitc == lmaxp ) break;
						huffman->decode_quadruple( conv_set, abs + p );
						for ( i = 0; i < 4; i++, p++ ) if ( abs[p] > 0 )
							sgn[p] = huffman->read_bit();
						if ( huffman->get_count() > lmaxp ) { // bad coding rollback
							bad_coding = true;
							abs[--p] = 0; sgn[p] = 0;
							abs[--p] = 0; sgn[p] = 0;
							abs[--p] = 0; sgn[p] = 0;
							abs[--p] = 0; sgn[p] = 0;
							huffman->rewind_bits( huffman->get_count() - bitc );
							break; // rollback complete
						}
					}
				}
				
				
				// --- SETTING AND ENCODING OF SV BOUND ---
				granule->sv_bound = p; // set sv_bound
				if ( !sbl )
					mod_svb->shift_context( ctx_svb[ch] );
				else mod_svb->shift_context( 144 + 1 );
				if ( p >= granule->region_bound[ 2 ] ) { // for proper files: encode actual sv bound		
					c = granule->sv_bound >> 2;
					encode_ari( enc, mod_svb, c );
					if ( !sbl ) ctx_svb[ch] = c; 
				} else { // for broken files: encode negative diff with r2 bound
					encode_ari( enc, mod_svb, 144 + 1 );
					c = ( granule->region_bound[ 2 ] - granule->sv_bound ) >> 1;
					encode_ari( enc, mod_bvf, c );
					// fix bounds
					granule->region_bound[ 2 ] = p;
					for ( i = 1; i >= 0; i-- ) if ( p < granule->region_bound[i] )
						granule->region_bound[i] = p;
					else break;
				}
				
				// --- COEFFICIENT ENCODING: PREPARATIONS ---
				if ( !sbl ) {
					ctx_h_abs = absl_ctx_h[ch]+1;
					ctx_h_sgn = sgnl_ctx_h[ch]+1;
					ctx_h_len = lenl_ctx_h[ch]+1;					
				} else {
					ctx_h_abs = abss_ctx_h[ch]+1;
					ctx_h_sgn = sgns_ctx_h[ch]+1;
					ctx_h_len = lens_ctx_h[ch]+1;
				}
				ctx_abs = 0; ctx_pat = 0;
				rlb = region_bounds[2];
				mod_sgc = mod_sgn[ch][flags];
				
				// --- COEFFICIENT ENCODING: SMALL VALUES ---
				mod_asc = mod_asv[ch][flags][(int) granule->select_htabB];
				for ( p = granule->sv_bound-1; p >= rlb; p-- ) {
					shift_model( mod_asc, ctx_pat, ctx_h_abs[p] );
					encode_ari( enc, mod_asc, abs[p] ); // absolutes
					ctx_pat = ( (ctx_pat<<1) | abs[p] ) & 0xF;
					ctx_abs = ( 2 * abs[p] + 5 * ctx_abs + 3 ) / 7;
					if ( abs[p] == 1 ) {
						shift_model( mod_sgc, ctx_h_abs[p], ctx_h_sgn[p] );
						encode_ari( enc, mod_sgc, sgn[p] ); // signs
					}
				}
				
				// --- COEFFICIENT ENCODING: BIG VALUES ---
				for ( r = 2; r >= 0; r-- ) {
					rlb = (r==0) ? 0 : region_bounds[ r-1 ];
					if ( p < rlb ) continue;
					if ( region_tables[ r ] == 0 ) { // tbl0 skipping
						memset( abs + rlb, 0, p - rlb );
						p = rlb; ctx_abs = 0;
						continue;
					}
					// set table and linbits
					bv_table = bv_dec_table + region_tables[ r ];
					linbits = bv_table->linbits;
					mod_abc = mod_abv[ch][flags][(int) region_tables[ r ]];
					mod_lnc = mod_len[ch][linbits];
					
					// encoding with/without linbits
					for ( ; p >= rlb; p-- ) {
						shift_model( mod_abc, ctx_abs, ctx_h_abs[p] );
						encode_ari( enc, mod_abc, abs[p] ); // absolutes
						ctx_abs = ( 2 * abs[p] + 5 * ctx_abs + 3 ) / 7;
						if ( abs[p] > 0 ) {
							shift_model( mod_sgc, ctx_h_abs[p], ctx_h_sgn[p] );
							encode_ari( enc, mod_sgc, sgn[p] ); // signs
							if ( linbits > 0 ) if ( abs[p] == 15 ) {
								// loop invariant condition (-funswitch-loops)
								mod_lnc->shift_context( ctx_h_len[p] );
								encode_ari( enc, mod_lnc, len[p] ); // bitlengths
								for ( i = len[p] - 2; i >= 0; i-- ) {
									shift_model( mod_res, len[p], i ); // bit residuals
									encode_ari( enc, mod_res, BITN( lbt[p], i ) );
								}
							}
						}						
					}
				}
				
				// --- COEFFICIENTS FINISHED: UPDATE CONTEXT ---
				p = granule->sv_bound;
				if ( p < 578 ) memset( abs + p, 0, 578 - p );
				// loop invariant condition
				if ( !j_coding || ( ch == 0 ) ) { // !!!
					// channel 0 context
					for ( i = 578-1; i >= 0; i-- ) {
						ctx_h_abs[i] = ( 2 * abs[i] + abs[i-1] + abs[i+1] + 3 * ctx_h_abs[i] + 3 ) / 7;
						if ( abs[i] > 0 ) {
							ctx_h_sgn[i] = ( ( ctx_h_sgn[i] << 1 ) | ( sgn[i] & 0x1 ) ) & 0xF;
							if ( abs[i] == 15 ) ctx_h_len[i] = ( 2 * len[i] + ctx_h_len[i] + 2 ) / 3;
							else ctx_h_len[i] >>= 1;
						} else {
							ctx_h_sgn[i] = ( ctx_h_sgn[i] << 1 ) & 0xF;
							ctx_h_len[i] >>= 1;
						}
					}
					// loop invariant condition
					if ( j_coding ) {
						// channel 1 context
						if ( !sbl ) {
							ctx_h_abs = absl_ctx_h[1]+1;
							ctx_h_sgn = sgnl_ctx_h[1]+1;
							ctx_h_len = lenl_ctx_h[1]+1;
						} else {
							ctx_h_abs = abss_ctx_h[1]+1;
							ctx_h_sgn = sgns_ctx_h[1]+1;
							ctx_h_len = lens_ctx_h[1]+1;
						}
						if ( p < 578 ) {
							memset( ctx_h_abs + p, 0, 578 - p );
							memset( ctx_h_sgn + p, 2, 578 - p ); // !!!
							memset( ctx_h_len + p, 0, 578 - p );
						}
						for ( i = p-1; i >= 0; i-- ) {
							ctx_h_abs[i] = abs[i];
							if ( abs[i] > 0 ) {
								ctx_h_sgn[i] = sgn[i];
								if ( abs[i] == 15 ) ctx_h_len[i] = len[i];
								else ctx_h_len[i] = 0;
							} else {
								ctx_h_sgn[i] = 2;
								ctx_h_len[i] = 0;
							}
						}
					}
				}
			
				
				// ---> RECONSTRUCTION INFORMATION: STUFFING BITS <---
				n = lmaxp - bitc; // # of stuffing bits
				if ( n == 0 ) { // the prefered case
					mod_nst->shift_context( ctx_nst );
					encode_ari( enc, mod_nst, 0 );
					ctx_nst = 0;
				} else { // encode # of stuff bits ( should be zero! )
					for ( ; n >= STUFFING_STEP; n -= STUFFING_STEP ) {
						mod_nst->shift_context( ctx_nst );
						encode_ari( enc, mod_nst, STUFFING_STEP );
						ctx_nst = STUFFING_STEP;
					} 
					mod_nst->shift_context( ctx_nst );
					encode_ari( enc, mod_nst, n );
					ctx_nst = n;
					// encode stuffing bits
					for ( ; bitc < lmaxp; bitc++ ) {
						mod_bst->shift_context( ctx_bst );
						c = huffman->read_bit();
						encode_ari( enc, mod_bst, c );
						ctx_bst = ( (ctx_bst<<1) | c ) & 0xF;
					}
				}
				
				
				// ---> GRANULE FINISHED! <---
				bitp += lmaxp;
				/*if ( ( granule->n >= 0 ) && ( granule->n <= 0 ) ) {
					fprintf( stderr, "\ngranule %i channel %i block_type: %i flags: %i\n", granule->n, ch, granule->block_type, flags );
					fprintf( stderr, "\ngranule %i channel %i nst: %i bst: %i aus: %i aux: %i pad: %i abs: %i\n", granule->n, ch, ctx_nst, ctx_bst, ctx_aux, ctx_aux, ctx_pad, ctx_abs );
					fprintf( stderr, "\ngranule %i channel %i bounds: %i/%i/%i/%i/%i\n", granule->n, ch, granule->region_bound[0], granule->region_bound[1], granule->region_bound[2], granule->sv_bound, granule->main_data_bit );
					fprintf( stderr, "\ngranule %i channel %i tables: %i/%i/%i/%i\n", granule->n, ch, granule->region_table[0], granule->region_table[1], granule->region_table[2], granule->select_htabB );
					fprintf( stderr, "\ngranule %i channel %i pos: %i\n", granule->n, ch, huffman->getpos() );
					fprintf( stderr, "\ngranule %i channel %i bad_coding: %s\n", granule->n, ch, bad_coding ? "yes" : "no" );
					fprintf( stderr, "\ngranule %i channel %i scfs: (%i/%i)\n", granule->n, ch, granule->share, granule->slength );
					for ( i = 0; i < 21; i++ ) fprintf( stderr, "%i, ", scf[i] );
					fprintf( stderr, "\n" );
					fprintf( stderr, "\ngranule %i channel %i coefs:\n", granule->n, ch );
					for ( i = 0; i < 578; i++ ) fprintf( stderr, "%i, ", abs[i] );
					fprintf( stderr, "\n" );
					for ( i = 0; i < 576; i++ ) if ( abs[i] == 15 ) fprintf( stderr, "%i, ", lbt[i] );
					fprintf( stderr, "\n" );
					for ( i = 0; i < 578; i++ ) if ( abs[i] > 0 ) fprintf( stderr, "%s", (sgn[i]) ? "+" : "-" );
					fprintf( stderr, "\n" );
					// fprintf( stderr, "\ngranule %i channel %i abs_ctx:\n", granule->n, ch );
					// for ( i = 0; i < 578; i++ ) fprintf( stderr, "%i, ", ctx_h_abs[i] );
					// fprintf( stderr, "\n" );
					// fprintf( stderr, "\ngranule %i channel %i sgn_ctx:\n", granule->n, ch );
					// for ( i = 0; i < 578; i++ ) fprintf( stderr, "%i, ", ctx_h_sgn[i] );
					// fprintf( stderr, "\n" );
					// fprintf( stderr, "\ngranule %i channel %i len_ctx:\n", granule->n, ch );
					// for ( i = 0; i < 578; i++ ) fprintf( stderr, "%i, ", ctx_h_len[i] );
					// fprintf( stderr, "\n" );
				}*/
			}
		}
		
		
		// ---> ENCODE BITRATE <---
		if ( i_bitrate == -1 ) {
			mod_btr->shift_context( bitrate_pred[ frame->main_size ] ); // !!! (bit-reservoir?)
			encode_ari( enc, mod_btr, frame->bits );
		}
		
		
		// ---> RECONSTRUCTION INFORMATION: AUX DATA AND PADDING <---
		if ( ( frame != lastframe ) && ( i_bit_res != 0 ) ) {
			if ( frame->aux_size + frame->next->bit_reservoir < 511 )
				n = frame->aux_size; // byte size of aux data
			else n = 511 - frame->next->bit_reservoir; // space taken from bit reservoir
			for ( ; n >= AUX_DATA_STEP; n -= AUX_DATA_STEP ) {
				mod_aux->shift_context( ctx_aux );
				encode_ari( enc, mod_aux, AUX_DATA_STEP );
				ctx_aux = AUX_DATA_STEP;
			}
			mod_aux->shift_context( ctx_aux );
			encode_ari( enc, mod_aux, n );
			ctx_aux = n;
		}
		// # of padding and aux data bits
		n = ( ( frame->main_size + frame->aux_size ) * 8 ) - bitp;
		// locally store padding and aux data
		for ( pna_c = pad_and_aux, i = n; i >= 8; i -= 8 )
			*(pna_c++) = huffman->read_bits( 8 );
		*pna_c = huffman->read_bits( i );
		// make and check prediction
		if ( pmp_predict_lame_anc( n, pad_and_aux ) == NULL )
			// prediction matches
			encode_ari( enc, mod_pap, 1 );
		else { // prediction doesn't match
			encode_ari( enc, mod_pap, 0 );
			huffman->rewind_bits( n );
			for ( ctx_pad = 0xFF; n > 0; n-- ) {
				mod_pad->shift_context( ctx_pad );
				c = huffman->read_bit();
				encode_ari( enc, mod_pad, c );
				ctx_pad = ( (ctx_pad<<1) | c ) & 0xFF;
			}
		}
		// encode padding bits(convert to full bytes)
		/*for ( c = 0xFF; n >= 8; n -= 8 ) {
			mod_pad->shift_context( c );
			c = huffman->read_bits( 8 );
			encode_ari( enc, mod_pad, c );
		}
		if ( n > 0 ) {
			mod_pad->shift_context( 0xFF + n );
			c = huffman->read_bits( n );
			encode_ari( enc, mod_pad, c );
		}*/
		
		// ---> FRAME FINISHED! <----
	}
	
	
	// ---> AFTER ENCODING: CLEAN UP <---
	
	// --- CLEAN UP: MODELS AND HUFFMAN CODER ---
	delete( huffman );
	delete( mod_svb );
	delete( mod_bvf );
	delete( mod_nst );
	delete( mod_bst );
	delete( mod_pad );
	delete( mod_pap );
	delete( mod_aux );
	delete( mod_btr );
	delete( mod_res );
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( sl = 0; sl < 5; sl++ )
			for ( g = 0; g < 10; g++ )
				delete( mod_scf[ch][sl][g] );
		for ( flags = 0x0; flags < ( (j_coding&&ch) ? 0x8 : 0x2 ); flags++ ) {
			delete( mod_abv[ch][flags][ 1] );
			delete( mod_abv[ch][flags][ 2] );
			delete( mod_abv[ch][flags][ 3] );
			delete( mod_abv[ch][flags][ 5] );
			delete( mod_abv[ch][flags][ 6] );
			delete( mod_abv[ch][flags][ 7] );
			delete( mod_abv[ch][flags][ 8] );
			delete( mod_abv[ch][flags][ 9] );
			delete( mod_abv[ch][flags][10] );
			delete( mod_abv[ch][flags][11] );
			delete( mod_abv[ch][flags][12] );
			delete( mod_abv[ch][flags][13] );
			delete( mod_abv[ch][flags][15] );
			delete( mod_abv[ch][flags][16] );
			delete( mod_abv[ch][flags][24] );
			delete( mod_asv[ch][flags][0] );
			delete( mod_asv[ch][flags][1] );
			delete( mod_sgn[ch][flags] );
		}
		for ( i = 0; i <= 13; i++ )
			delete( mod_len[ch][i] );
	}
	
	// --- CLEAN UP: MEMORY DEALLOCATION ---
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		free( scf_c[ch] ); free( scf_l_long[ch] ); free( scf_l_short[ch] );
		free( abs_c[ch] ); free( absl_ctx_h[ch] ); free( abss_ctx_h[ch] );
		free( sgn_c[ch] ); free( sgnl_ctx_h[ch] ); free( sgns_ctx_h[ch] );
		free( len_c[ch] ); free( lenl_ctx_h[ch] ); free( lens_ctx_h[ch] );
		free( lbt_c[ch] );
	}
	
	
	return true;
}


/* -----------------------------------------------
	decode the main data
	----------------------------------------------- */
INTERN inline bool pmp_decode_main_data( aricoder* dec )
{
	// context / storage
	static thread_local unsigned char* pad_and_aux= ( unsigned char* ) calloc( 2048, 1 );
	mp3Frame* frame;
	granuleInfo* granule;
	unsigned char* scf_c[2];
	unsigned char* scf_l_long[2];
	unsigned char* scf_l_short[2];
	unsigned char* abs_c[2];
	unsigned char* sgn_c[2];
	unsigned char* len_c[2];
	unsigned short* lbt_c[2];
	unsigned char* absl_ctx_h[2];
	unsigned char* abss_ctx_h[2];
	unsigned char* sgnl_ctx_h[2];
	unsigned char* sgns_ctx_h[2];
	unsigned char* lenl_ctx_h[2];
	unsigned char* lens_ctx_h[2];
	unsigned char* scf_prev;
	unsigned char* ctx_h_abs;
	unsigned char* ctx_h_sgn;
	unsigned char* ctx_h_len;
	unsigned char* scf;
	unsigned char* abs;
	unsigned char* sgn;
	unsigned char* len;
	unsigned short* lbt;
	unsigned char* swap;
	unsigned char* pna_c;
	unsigned char ctx_scf;
	unsigned char ctx_abs;
	unsigned char ctx_pat;
	unsigned char ctx_svb[2] = { 0, 0 };
	unsigned char ctx_nst = 0;
	unsigned char ctx_bst = 0;
	unsigned char ctx_aux = 0;
	unsigned char ctx_pad = 0;
	// statistical models
	model_s* mod_scf[2][5][10];
	model_s* mod_abv[2][8][32];
	model_b* mod_asv[2][8][2];
	model_b* mod_sgn[2][8];
	model_s* mod_len[2][14];
	model_b* mod_res;
	model_s* mod_svb;
	model_s* mod_bvf;
	model_s* mod_nst;
	model_b* mod_bst;
	model_b* mod_pad;
	model_b* mod_pap;
	model_s* mod_aux;
	model_s* mod_btr;
	model_s* mod_abc;
	model_b* mod_asc;
	model_b* mod_sgc;
	model_s* mod_lnc;
	model_s* mod_sfc;
	// lookup tables
	const int* bitrate_pred = mp3_bitrate_pred[(int)i_samplerate];
	// huffman encoder
	huffman_writer* huffman;
	// mp3 encoding settings
	short* region_bounds;
	char* region_tables;
	huffman_enc_table* bv_table;
	huffman_code** hcodes;
	huffman_code* hcode;
	int bitres = 0;
	int linbits;
	const int* slen;
	int rlb, sl;	
	// general settings
	int bitp = 0;
	int sbl = 0;
	int flags = 0;
	bool j_coding;
	char shared[2][4];
	// counters
	int ch, gr;
	int p, g, r;
	int i, c, n;
	
	
	
	// --- PRE-PROCESSING PREPARATIONS: INIT STATISTICAL MODELS/HUFFMAN DECODER/CONTEXT ---
	// decide if joint context will be used (will be used for joint & standard stereo)
	j_coding = ( i_channels == MP3_STEREO ) || ( i_channels == MP3_JOINT_STEREO );
	// init huffman encoder/writer
	huffman = new huffman_writer( 0 );
	// reset ancillary predictor
	pmp_predict_lame_anc( -1, NULL );
	// init statistical models
	mod_svb = INIT_MODEL_S( ( 576 / 4 ) + 1 + 1, ( 576 / 4 ) + 1 + 1, 1 );
	mod_bvf = INIT_MODEL_S( ( 576 / 2 ) + 1, 0, 0 );
	mod_nst = INIT_MODEL_S( STUFFING_STEP + 1, STUFFING_STEP + 1, 1 );
	mod_bst = INIT_MODEL_B( 16, 1 );
	mod_pad = INIT_MODEL_B( 256, 1 );
	mod_pap = INIT_MODEL_B( 0, 0 );
	mod_aux = INIT_MODEL_S( AUX_DATA_STEP + 1, AUX_DATA_STEP + 1, 1 );
	mod_btr = INIT_MODEL_S( 16, 16, 1 );
	mod_res = INIT_MODEL_B( 13 + 1, 2 );
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( g = 0; g < 10; g++ ) {
			mod_scf[ch][0][g] = INIT_MODEL_S( 2, 16, 2 );
			mod_scf[ch][1][g] = INIT_MODEL_S( 4, 16, 2 );
			mod_scf[ch][2][g] = INIT_MODEL_S( 8, 16, 2 );
			mod_scf[ch][3][g] = INIT_MODEL_S( 16, 16, 2 );
				mod_scf[ch][4][g] = INIT_MODEL_S( 32, 16, 2 ); // LSF intensity-stereo scf_size==5
		}
		for ( flags = 0x0; flags < ( (j_coding&&ch) ? 0x8 : 0x2 ); flags++ ) {
			mod_abv[ch][flags][ 0] = NULL;
			mod_abv[ch][flags][ 1] = INIT_MODEL_S(  1 + 1, 16, 2 );
			mod_abv[ch][flags][ 2] = INIT_MODEL_S(  2 + 1, 16, 2 );
			mod_abv[ch][flags][ 3] = INIT_MODEL_S(  2 + 1, 16, 2 );
			mod_abv[ch][flags][ 4] = NULL;
			mod_abv[ch][flags][ 5] = INIT_MODEL_S(  3 + 1, 16, 2 );
			mod_abv[ch][flags][ 6] = INIT_MODEL_S(  3 + 1, 16, 2 );
			mod_abv[ch][flags][ 7] = INIT_MODEL_S(  5 + 1, 16, 2 );
			mod_abv[ch][flags][ 8] = INIT_MODEL_S(  5 + 1, 16, 2 );
			mod_abv[ch][flags][ 9] = INIT_MODEL_S(  5 + 1, 16, 2 );
			mod_abv[ch][flags][10] = INIT_MODEL_S(  7 + 1, 16, 2 );
			mod_abv[ch][flags][11] = INIT_MODEL_S(  7 + 1, 16, 2 );
			mod_abv[ch][flags][12] = INIT_MODEL_S(  7 + 1, 16, 2 );
			mod_abv[ch][flags][13] = INIT_MODEL_S( 15 + 1, 16, 2 );
			mod_abv[ch][flags][14] = NULL;
			mod_abv[ch][flags][15] = INIT_MODEL_S( 15 + 1, 16, 2 );
			mod_abv[ch][flags][16] = INIT_MODEL_S( 15 + 1, 16, 2 );
			mod_abv[ch][flags][17] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][18] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][19] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][20] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][21] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][22] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][23] = mod_abv[ch][flags][16];
			mod_abv[ch][flags][24] = INIT_MODEL_S( 15 + 1, 16, 2 );
			mod_abv[ch][flags][25] = mod_abv[ch][flags][24];
			mod_abv[ch][flags][26] = mod_abv[ch][flags][24];
			mod_abv[ch][flags][27] = mod_abv[ch][flags][24];
			mod_abv[ch][flags][28] = mod_abv[ch][flags][24];
			mod_abv[ch][flags][29] = mod_abv[ch][flags][24];
			mod_abv[ch][flags][30] = mod_abv[ch][flags][24];
			mod_abv[ch][flags][31] = mod_abv[ch][flags][24];
			mod_asv[ch][flags][0] = INIT_MODEL_B( 16, 2 );
			mod_asv[ch][flags][1] = INIT_MODEL_B( 16, 2 );
			mod_sgn[ch][flags] = INIT_MODEL_B( 16, 2 );
		}
		mod_len[ch][0] = NULL;
		for ( i = 1; i <= 13; i++ )
			mod_len[ch][i] = INIT_MODEL_S( i + 1, 13 + 1, 1 );
	}
	
	
	// --- PRE-PROCESSING PREPARATIONS: ALLOCATION OF MEMORY ---
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// alloc memory
		scf_c[ch]       = ( unsigned char* ) calloc(  40, sizeof( char ) ); // 40: LSF short needs up to 36
		scf_l_long[ch]  = ( unsigned char* ) calloc(  40, sizeof( char ) );
		scf_l_short[ch] = ( unsigned char* ) calloc(  40, sizeof( char ) );
		abs_c[ch]       = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		sgn_c[ch]       = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		len_c[ch]       = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		lbt_c[ch]      = ( unsigned short* ) calloc( 576, sizeof( short ) );
		absl_ctx_h[ch]  = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		abss_ctx_h[ch]  = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		sgnl_ctx_h[ch]  = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		sgns_ctx_h[ch]  = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		lenl_ctx_h[ch]  = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		lens_ctx_h[ch]  = ( unsigned char* ) calloc( 578+1+1, sizeof( char ) );
		// check for problems
		if ( ( scf_c[ch] == NULL ) || ( scf_l_long[ch] == NULL ) || ( scf_l_short[ch] == NULL ) ||
			 ( abs_c[ch] == NULL ) || ( absl_ctx_h[ch] == NULL ) || ( abss_ctx_h[ch] == NULL ) ||
			 ( sgn_c[ch] == NULL ) || ( sgnl_ctx_h[ch] == NULL ) || ( sgns_ctx_h[ch] == NULL ) ||
			 ( len_c[ch] == NULL ) || ( lenl_ctx_h[ch] == NULL ) || ( lens_ctx_h[ch] == NULL ) ||
			 ( lbt_c[ch] == NULL ) ) {
			snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
			errorlevel = 2;
			return false;
		}
	}	
	
	
	// --- MAIN PROCESSING LOOP: DECODING AND ENCODING ---
	for ( frame = firstframe; frame != NULL; frame = frame->next, bitp = 0 ) {
		// record main index
		frame->main_index = huffman->getpos();
		for ( gr = 0; gr < mp3_ngr( frame->mpeg ); gr++ ) {
			for ( ch = 0; ch < g_nchannels; ch++ ) {

				// --- MAIN DATA DECODING: PREPARATIONS ---
				// initialize shortcuts
				granule = frame->granules[ch][gr];
				sbl = ( granule->block_type == SHORT_BLOCK ) ? 1 : 0;
				slen = slength_table[ (int) granule->slength ];
				region_bounds = granule->region_bound;
				region_tables = granule->region_table;
				abs = abs_c[ch] + 1;
				sgn = sgn_c[ch] + 1;
				len = len_c[ch] + 1;
				lbt = lbt_c[ch];
				// set compression flags
				flags = ( !j_coding || ( ch == 0 ) ) ? sbl :
					( sbl << 0 ) | // long (0) or short (1) block
					( frame->stereo_ms << 1 ) | // stereo ms on/off
					( ( flags ^ sbl ) << 2 ); // ch0/ch1 block type diffs y/n
				// store sharing info
				if ( gr == 0 ) {
					shared[ch][0] = ( granule->share >> 3 ) & 0x1;
					shared[ch][1] = ( granule->share >> 2 ) & 0x1;
					shared[ch][2] = ( granule->share >> 1 ) & 0x1;
					shared[ch][3] = ( granule->share >> 0 ) & 0x1;
				}
				// reset counter
				huffman->reset_counter();
				// store bit reservoir state
				frame->bit_reservoir = bitres;
				
				
				// ---> SCALEFACTOR PROCESSING <---
				if ( frame->mpeg != MP3_V1_0 ) {

					// --- SCALEFACTOR DECODING/WRITING: MPEG-2/2.5 (LSF) ---
					// Mirror of the encoder: scfcnt[g] values of scfsz[g] bits, linear.
					int scfsz[ 4 ], scfcnt[ 4 ], j;
					mp3_lsf_scf_params( frame, granule, ch, scfsz, scfcnt );
					scf = scf_c[ch];
					scf_prev = scf_l_long[ch];
					ctx_scf = 0;
					for ( g = 0, p = 0; g < 4; g++ ) {
						sl = scfsz[ g ];
						if ( sl == 0 ) {
							for ( j = 0; j < scfcnt[ g ]; j++, p++ ) scf[ p ] = 0;
						} else {
							mod_sfc = mod_scf[ch][ sl-1 ][ g ];
							for ( j = 0; j < scfcnt[ g ]; j++, p++ ) {
								shift_model( mod_sfc, ctx_scf, scf_prev[ p ] );
								scf[ p ] = decode_ari( dec, mod_sfc );
								huffman->write_bits( scf[ p ], sl );
								ctx_scf = scf[ p ];
							}
						}
					}
					swap = scf_c[ch]; scf_c[ch] = scf_l_long[ch]; scf_l_long[ch] = swap;

				} else if ( !sbl ) {

					// --- SCALEFACTOR DECODING/WRITING: LONG BLOCKS ---
					scf = scf_c[ch];
					scf_prev = scf_l_long[ch];
					ctx_scf = 0;

					// decode/write 21 scalefactors with/without sharing
					for ( g = 0, p = 0; g < 4; g++ ) {
						sl = slen[ ( g < 2 ) ? 0 : 1 ];
						if ( ( gr ) & ( shared[ch][g] ) ) { // shared
							// loop invariant condition (-funswitch-loops)
							memcpy( scf + p, scf_prev + p, scf_width[ g ] );
							p = scf_bounds[ g ];
							ctx_scf = scf[p-1];
						} else if ( sl == 0 ) { // zero slength
							memset( scf + p, 0, scf_width[ g ] );
							p = scf_bounds[ g ];
							ctx_scf = 0;
						} else {
							mod_sfc = mod_scf[ch][sl-1][(shared[ch][g])?g|0x4:g];
							for ( ; p < scf_bounds[ g ]; p++ ) { // non shared
								shift_model( mod_sfc, ctx_scf, scf_prev[p] );
								scf[p] = decode_ari( dec, mod_sfc );
								huffman->write_bits( scf[p], sl );
								ctx_scf = scf[p];
							}
						}
					}
					
					// --- SCALEFACTORS FINISHED: SWAP DATA ---
					swap = scf_c[ch]; scf_c[ch] = scf_l_long[ch]; scf_l_long[ch] = swap;
					
				} else {
					
					// --- SCALEFACTOR DECODING/WRITING: SHORT BLOCKS ---
					// encode (only non shared)
					for ( i = 0; i < 3; i++ ) { // 3 subblocks
						scf = scf_c[ch];
						scf_prev = scf_l_short[ch];
						ctx_scf = 0;
						for ( g = 0, p = 0; g < 2; g++ ) { // lo&hi groups
							sl = slen[ g ];
							if ( sl == 0 ) { // zero slength
								memset( scf + p, 0, scf_lh_width_short[ g ] );
								p = scf_lh_bounds_short[ g ];
							} else {
								mod_sfc = mod_scf[ch][sl-1][g|0x8];
								for ( ; p < scf_lh_bounds_short[ g ]; p++ ) { // scfs
									shift_model( mod_sfc, ctx_scf, scf_prev[p] );
									scf[p] = decode_ari( dec, mod_sfc );
									huffman->write_bits( scf[p], sl );
									ctx_scf = scf[p];
								}
							}
						}
						// --- SWAP DATA ---
						swap = scf_c[ch]; scf_c[ch] = scf_l_short[ch]; scf_l_short[ch] = swap;
					}				
				}
				
				
				// ---> COEFFICIENTS PROCESSING <---
				
				// --- DECODING AND FIXING OF SV BOUND ---
				if ( !sbl ) {
					mod_svb->shift_context( ctx_svb[ch] );
				} else mod_svb->shift_context( 144 + 1 );
				c = decode_ari( dec, mod_svb );
				if ( c <= 144 ) { // for proper files
					granule->sv_bound = c << 2;
					if ( granule->region_bound[ 2 ] % 4 == 2 )
						granule->sv_bound += 2;
					if ( !sbl ) ctx_svb[ch] = c;
				} else { // for broken files
					c = decode_ari( dec, mod_bvf );
					granule->sv_bound = granule->region_bound[ 2 ] - ( c << 1 );
					// fix bounds
					granule->region_bound[ 2 ] = granule->sv_bound;
					for ( i = 1; i >= 0; i-- ) if ( granule->sv_bound < granule->region_bound[i] )
						granule->region_bound[i] = granule->sv_bound;
					else break;
				}
				
				
				// --- COEFFICIENT DECODING: PREPARATIONS ---
				if ( granule->block_type != SHORT_BLOCK ) {
					ctx_h_abs = absl_ctx_h[ch]+1;
					ctx_h_sgn = sgnl_ctx_h[ch]+1;
					ctx_h_len = lenl_ctx_h[ch]+1;
				} else {
					ctx_h_abs = abss_ctx_h[ch]+1;
					ctx_h_sgn = sgns_ctx_h[ch]+1;
					ctx_h_len = lens_ctx_h[ch]+1;
				}
				ctx_abs = 0; ctx_pat = 0;
				rlb = region_bounds[2];
				mod_sgc = mod_sgn[ch][flags];
				
				// --- COEFFICIENT DECODING: SMALL VALUES ---
				mod_asc = mod_asv[ch][flags][(int) granule->select_htabB];
				for ( p = granule->sv_bound-1; p >= rlb; p-- ) {
					shift_model( mod_asc, ctx_pat, ctx_h_abs[p] );
					abs[p] = decode_ari( dec, mod_asc ); // absolutes
					ctx_pat = ( (ctx_pat<<1) | abs[p] ) & 0xF;
					ctx_abs = ( 2 * abs[p] + 5 * ctx_abs + 3 ) / 7;
					if ( abs[p] == 1 ) {
						shift_model( mod_sgc, ctx_h_abs[p], ctx_h_sgn[p] );
						sgn[p] = decode_ari( dec, mod_sgc ); // signs
					}
				}
				
				// --- COEFFICIENT DECODING: BIG VALUES ---
				for ( r = 2; r >= 0; r-- ) {
					rlb = (r==0) ? 0 : region_bounds[ r-1 ];
					if ( p < rlb ) continue;
					if ( region_tables[ r ] == 0 ) { // tbl0 skipping
						memset( abs + rlb, 0, p - rlb );
						p = rlb; ctx_abs = 0;
						continue;
					}
					// set table and linbits
					bv_table = bv_enc_table + region_tables[ r ];
					linbits = bv_table->linbits;
					mod_abc = mod_abv[ch][flags][(int) region_tables[ r ]];
					mod_lnc = mod_len[ch][linbits];
					
					// decoding with/without linbits
					for ( ; p >= rlb; p-- ) {
						shift_model( mod_abc, ctx_abs, ctx_h_abs[p] );
						abs[p] = decode_ari( dec, mod_abc ); // absolutes
						ctx_abs = ( 2 * abs[p] + 5 * ctx_abs + 3 ) / 7;
						if ( abs[p] > 0 ) {
							shift_model( mod_sgc, ctx_h_abs[p], ctx_h_sgn[p] );
							sgn[p] = decode_ari( dec, mod_sgc ); // signs
							if ( linbits > 0 ) if ( abs[p] == 15 ) {
								// loop invariant condition (-funswitch-loops)
								mod_lnc->shift_context( ctx_h_len[p] );
								len[p] = decode_ari( dec, mod_lnc ); // bitlengths									
								if ( len[p] > 0 ) {
									for ( lbt[p] = 1, i = len[p] - 2; i >= 0; i-- ) {
										shift_model( mod_res, len[p], i ); // bit residuals
										lbt[p] = ( lbt[p] << 1 ) | decode_ari( dec, mod_res );
									}
								} else lbt[p] = 0;
							}
						}
					}
				}
				
				
				// --- COEFFICIENT ENCODING: BIG VALUES ---
				for ( p = 0, r = 0; r < 3; r++ ) {
					if ( region_tables[ r ] == 0 ) { // tbl0 skipping
						p = region_bounds[ r ];			
						continue;
					}
					// set table and linbits
					bv_table = bv_enc_table + region_tables[ r ];
					hcodes = bv_table->h;
					linbits = bv_table->linbits;
					
					// encoding with/without linbits
					while ( p < region_bounds[ r ] ) {
						huffman->encode_pair( hcodes, abs + p );
						for ( i = 0; i < 2; i++, p++ ) if ( abs[p] > 0 ) {
							if ( linbits > 0 ) // loop invariant condition (-unswitch-loops)
								if ( abs[p] == 15 ) huffman->write_bits( lbt[p], linbits );
							huffman->write_bit( sgn[p] );
						}
					}
				}
				
				// --- COEFFICIENT ENCODING: SMALL VALUES ---
				hcode = ( granule->select_htabB ) ? htabB_enc : htabA_enc;
				while ( p < granule->sv_bound ) {
					huffman->encode_quadruple( hcode, abs + p );
					for ( i = 0; i < 4; i++, p++ ) if ( abs[p] )
						huffman->write_bit( sgn[p] );
				}
				
				// --- COEFFICIENTS FINISHED: UPDATE CONTEXT ---
				if ( p < 578 ) memset( abs + p, 0, 578 - p );
				// loop invariant condition
				if ( !j_coding || ( ch == 0 ) ) {
					// channel 0 context
					for ( i = 578-1; i >= 0; i-- ) {
						ctx_h_abs[i] = ( 2 * abs[i] + abs[i-1] + abs[i+1] + 3 * ctx_h_abs[i] + 3 ) / 7;
						if ( abs[i] > 0 ) {
							ctx_h_sgn[i] = ( ( ctx_h_sgn[i] << 1 ) | ( sgn[i] & 0x1 ) ) & 0xF;
							if ( abs[i] == 15 ) ctx_h_len[i] = ( 2 * len[i] + ctx_h_len[i] + 2 ) / 3;
							else ctx_h_len[i] >>= 1;
						} else {
							ctx_h_sgn[i] = ( ctx_h_sgn[i] << 1 ) & 0xF;
							ctx_h_len[i] >>= 1;
						}
					}
					// loop invariant condition
					if ( j_coding ) {
						// channel 1 context
						if ( !sbl ) {
							ctx_h_abs = absl_ctx_h[1]+1;
							ctx_h_sgn = sgnl_ctx_h[1]+1;
							ctx_h_len = lenl_ctx_h[1]+1;
						} else {
							ctx_h_abs = abss_ctx_h[1]+1;
							ctx_h_sgn = sgns_ctx_h[1]+1;
							ctx_h_len = lens_ctx_h[1]+1;
						}
						if ( p < 578 ) {
							memset( ctx_h_abs + p, 0, 578 - p );
							memset( ctx_h_sgn + p, 2, 578 - p );
							memset( ctx_h_len + p, 0, 578 - p );
						}
						for ( i = p-1; i >= 0; i-- ) {
							ctx_h_abs[i] = abs[i];
							if ( abs[i] > 0 ) {
								ctx_h_sgn[i] = sgn[i];
								if ( abs[i] == 15 ) ctx_h_len[i] = len[i];
								else ctx_h_len[i] = 0;
							} else {
								ctx_h_sgn[i] = 2;
								ctx_h_len[i] = 0;
							}
						}
					}
				}
			
			
				// ---> RECONSTRUCTION INFORMATION: STUFFING BITS <---
				for ( n = 0, c = STUFFING_STEP; c == STUFFING_STEP; n += c ) {
					// find out # of stuffing bits
					mod_nst->shift_context( ctx_nst );
					c = decode_ari( dec, mod_nst );
					ctx_nst = c;
				}
				for ( ; n > 0; n-- ) {
					mod_bst->shift_context( ctx_bst );
					c = decode_ari( dec, mod_bst );
					huffman->write_bit( c );
					ctx_bst = ( (ctx_bst<<1) | c ) & 0xF;
				}
				
				
				// ---> GRANULE FINISHED! <---
				granule->main_data_bit = huffman->get_count();
				bitp += granule->main_data_bit;
				/*if ( ( granule->n >= 0 ) && ( granule->n <= 0 ) ) {
					fprintf( stderr, "\ngranule %i channel %i block_type: %i flags: %i\n", granule->n, ch, granule->block_type, flags );
					fprintf( stderr, "\ngranule %i channel %i nst: %i bst: %i aus: %i aux: %i pad: %i abs: %i\n", granule->n, ch, ctx_nst, ctx_bst, ctx_aux, ctx_aux, ctx_pad, ctx_abs );
					fprintf( stderr, "\ngranule %i channel %i bounds: %i/%i/%i/%i/%i\n", granule->n, ch, granule->region_bound[0], granule->region_bound[1], granule->region_bound[2], granule->sv_bound, granule->main_data_bit );
					fprintf( stderr, "\ngranule %i channel %i tables: %i/%i/%i/%i\n", granule->n, ch, granule->region_table[0], granule->region_table[1], granule->region_table[2], granule->select_htabB );
					fprintf( stderr, "\ngranule %i channel %i pos: %i\n", granule->n, ch, huffman->getpos() );
					fprintf( stderr, "\ngranule %i channel %i scfs: (%i/%i)\n", granule->n, ch, granule->share, granule->slength );
					for ( i = 0; i < 21; i++ ) fprintf( stderr, "%i, ", scf[i] );
					fprintf( stderr, "\n" );
					fprintf( stderr, "\ngranule %i channel %i coefs:\n", granule->n, ch );
					for ( i = 0; i < 578; i++ ) fprintf( stderr, "%i, ", abs[i] );
					fprintf( stderr, "\n" );
					for ( i = 0; i < 576; i++ ) if ( abs[i] == 15 ) fprintf( stderr, "%i, ", lbt[i] );
					fprintf( stderr, "\n" );
					for ( i = 0; i < 578; i++ ) if ( abs[i] > 0 ) fprintf( stderr, "%s", (sgn[i]) ? "+" : "-" );
					fprintf( stderr, "\n" );
					// fprintf( stderr, "\ngranule %i channel %i abs_ctx:\n", granule->n, ch );
					// for ( i = 0; i < 578; i++ ) fprintf( stderr, "%i, ", ctx_h_abs[i] );
					// fprintf( stderr, "\n" );
					// fprintf( stderr, "\ngranule %i channel %i sgn_ctx:\n", granule->n, ch );
					// for ( i = 0; i < 578; i++ ) fprintf( stderr, "%i, ", ctx_h_sgn[i] );
					// fprintf( stderr, "\n" );
					// fprintf( stderr, "\ngranule %i channel %i len_ctx:\n", granule->n, ch );
					// for ( i = 0; i < 578; i++ ) fprintf( stderr, "%i, ", ctx_h_len[i] );
					// fprintf( stderr, "\n" );
				}*/
			}
		}
		// --- STORE FRAME MAIN DATA SIZES ---
		frame->main_bits = bitp;
		frame->main_size = ( bitp + 7 ) / 8;
		
		
		// ---> DECODE BITRATE <---
		if ( i_bitrate == -1 ) {
			mod_btr->shift_context( bitrate_pred[ frame->main_size ] );
			frame->bits = decode_ari( dec, mod_btr );
		}
		// calculate size of frame (MPEG-1 via table, MPEG-2/2.5 via formula)
		frame->frame_size = mp3_frame_bytes( i_mpeg, i_samplerate, frame->bits, frame->padding );
		
		
		// ---> RECONSTRUCTION INFORMATION: AUX DATA AND PADDING <---
		if ( i_bit_res != 0 ) {
			// preliminary next frame bit reservoir size
			bitres =
				frame->frame_size -
				frame->fixed_size +
				frame->bit_reservoir -
				frame->main_size;
			if ( frame != lastframe ) {
				n = ( bitres < 511 ) ? 0 : bitres - 511;
				for ( c = AUX_DATA_STEP; c == AUX_DATA_STEP; n += c ) {
					// byte size of aux data
					mod_aux->shift_context( ctx_aux );
					c = decode_ari( dec, mod_aux );
					ctx_aux = c;
				}
				frame->aux_size = n;
			} else frame->aux_size = bitres;
			// finalize size of bit reservoir
			bitres -= frame->aux_size;
		} else {
			bitres = 0;
			frame->aux_size = 
				frame->frame_size -
				frame->fixed_size +
				frame->bit_reservoir -
				frame->main_size;
		}
		// # of padding and aux data bits
		n = ( ( frame->main_size + frame->aux_size ) * 8 ) - bitp;
		// check if data can be predicted 100%
		if ( decode_ari( dec, mod_pap ) == 1 ) {
			// prediction matches !
			pna_c = pmp_predict_lame_anc( n, NULL );
			for ( ; n >= 8; n -= 8 ) huffman->write_bits( *(pna_c++), 8 );
			if ( n > 0 ) huffman->write_bits( *(pna_c) >> (8-n), n );
		} else {
			// prediction doesn't match (d'oh!)
			pna_c = pad_and_aux; *pna_c = 0;
			for ( ctx_pad = 0xFF, i = 1; i <= n; i++ ) {
				mod_pad->shift_context( ctx_pad );
				c = decode_ari( dec, mod_pad );
				huffman->write_bit( c );
				ctx_pad = ( (ctx_pad<<1) | c ) & 0xFF;
				*pna_c = (*pna_c<<1)|c;
				if ( i % 8 == 0 ) *(++pna_c) = 0;
			}
			if ( pmp_predict_lame_anc( n, pad_and_aux ) == NULL ) {
				snprintf( errormessage, MSG_SIZE, "ancilary prediction error, please report" );
				errorlevel = 2;
				return false;
			}
		}
		// decode padding bits
		/*for ( c = 0xFF; n >= 8; n -= 8 ) {
			mod_pad->shift_context( c );
			c = decode_ari( dec, mod_pad );
			huffman->write_bits( c, 8 );
		}
		if ( n > 0 ) {
			mod_pad->shift_context( 0xFF + n );
			c = decode_ari( dec, mod_pad );
			huffman->write_bits( c, n );
		}*/		
		
		// ---> FRAME FINISHED! <----		
	}
	
	
	// ---> AFTER ENCODING: GRAB POINTER AND CLEAN UP <---
	
	// --- GRAB POINTER AND SIZE ---
	main_data = huffman->getptr();
	main_data_size = huffman->getpos();
	
	// --- CALCULATE MP3 FILE SIZE ---
	mp3filesize =
		data_before_size +
		data_after_size +
		main_data_size +
		( g_nframes * lastframe->fixed_size );
	
	// --- CLEAN UP: MODELS AND HUFFMAN CODER ---
	delete( huffman );
	delete( mod_svb );
	delete( mod_bvf );
	delete( mod_nst );
	delete( mod_bst );
	delete( mod_pad );
	delete( mod_pap );
	delete( mod_aux );
	delete( mod_btr );
	delete( mod_res );
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( sl = 0; sl < 5; sl++ )
			for ( g = 0; g < 10; g++ )
				delete( mod_scf[ch][sl][g] );
		for ( flags = 0x0; flags < ( (j_coding&&ch) ? 0x8 : 0x2 ); flags++ ) {
			delete( mod_abv[ch][flags][ 1] );
			delete( mod_abv[ch][flags][ 2] );
			delete( mod_abv[ch][flags][ 3] );
			delete( mod_abv[ch][flags][ 5] );
			delete( mod_abv[ch][flags][ 6] );
			delete( mod_abv[ch][flags][ 7] );
			delete( mod_abv[ch][flags][ 8] );
			delete( mod_abv[ch][flags][ 9] );
			delete( mod_abv[ch][flags][10] );
			delete( mod_abv[ch][flags][11] );
			delete( mod_abv[ch][flags][12] );
			delete( mod_abv[ch][flags][13] );
			delete( mod_abv[ch][flags][15] );
			delete( mod_abv[ch][flags][16] );
			delete( mod_abv[ch][flags][24] );
			delete( mod_asv[ch][flags][0] );
			delete( mod_asv[ch][flags][1] );
			delete( mod_sgn[ch][flags] );
		}
		for ( i = 0; i <= 13; i++ )
			delete( mod_len[ch][i] );
	}
	
	// --- CLEAN UP: MEMORY DEALLOCATION ---
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		free( scf_c[ch] ); free( scf_l_long[ch] ); free( scf_l_short[ch] );
		free( abs_c[ch] ); free( absl_ctx_h[ch] ); free( abss_ctx_h[ch] );
		free( sgn_c[ch] ); free( sgnl_ctx_h[ch] ); free( sgns_ctx_h[ch] );
		free( len_c[ch] ); free( lenl_ctx_h[ch] ); free( lens_ctx_h[ch] );
		free( lbt_c[ch] );
	}
	
	
	return true;
}


/* -----------------------------------------------
	store unmute data
	----------------------------------------------- */
INTERN inline bool pmp_store_unmute_data( iostream* str )
{
	// as simple as it gets...
	str->write( unmute_data, sizeof( char ), unmute_data_size );
	
	return true;
}


/* -----------------------------------------------
	unstore unmute data
	----------------------------------------------- */
INTERN inline bool pmp_unstore_unmute_data( iostream* str )
{
	unsigned char n;
	
	// read # of of muted frames, set data size
	str->read( &n, sizeof( char ), 1 );
	if ( str->chkeof() ) n = 0;
	unmute_data_size = 1 + n * ( 2 + ( g_nchannels * mp3_ngr( i_mpeg ) * ( ( i_mpeg != MP3_V1_0 ) ? 5 : 4 ) ) );
	
	// alloc memory for unmute data
	unmute_data = (unsigned char*) calloc( unmute_data_size, sizeof( char ) );
	if ( unmute_data == NULL ) {
		snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
		errorlevel = 2;
		return false;
	}
	
	// read unmute data
	*unmute_data = n;
	str->read( unmute_data + 1, sizeof( char ), unmute_data_size - 1 );
	
	// check for eof
	if ( str->chkeof() ) {
		snprintf( errormessage, MSG_SIZE, "unexpected end of data" );
		errorlevel = 2;
		return false;
	}
	
	
	return true;
}


/* -----------------------------------------------
	build universal context from global gain
	----------------------------------------------- */
INTERN inline bool pmp_build_context( void )
{
	granuleInfo* granule;
	int count_gg[ 2 ][ 256 ];
	unsigned char* gg0;
	unsigned char* gg1;
	int ngr;
	
	int lbound;
	int inc0, inc1;
	int i, ch;
	
	
	// check and free gg context arrays where needed
	if ( gg_context[0] != NULL ) free ( gg_context[0] );
	if ( gg_context[1] != NULL ) free ( gg_context[1] );
	gg_context[0] = NULL;
	gg_context[1] = NULL;
	
	// number of granules (per channel)
	ngr = g_nframes * 2;
	
	// alloc memory for three global gain contexts
	// yup, this might be a waste for mono files
	gg_context[0] = (unsigned char*) calloc ( ngr, sizeof( char ) );
	gg_context[1] = (unsigned char*) calloc ( ngr, sizeof( char ) );
	if ( ( gg_context[0] == NULL ) || ( gg_context[1] == NULL ) ) {
		snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
		errorlevel = 2;
		return false;
	}
	
	// set everything zero
	memset( count_gg[0], 0, 256 * 4 );
	memset( count_gg[1], 0, 256 * 4 );
	
	// count channel 0 symbol occurences
	gg0 = gg_context[0];
	for ( granule = firstframe->granules[0][0]; granule != NULL; granule = granule->next ) {
		*gg0 = granule->global_gain;
		count_gg[0][*(gg0++)]++;
	}
	for ( i = 1; i < 256; i++ ) // accumulate counts 
		count_gg[0][i] += count_gg[0][i-1];
	
	// count channel 1 and diff symbol occurences
	if ( g_nchannels == 2 ) {
		gg0 = gg_context[0];
		gg1 = gg_context[1];
		for ( granule = firstframe->granules[1][0]; granule != NULL; granule = granule->next ) {
			*gg1 = granule->global_gain;
			count_gg[1][*(gg1++)]++;
			gg0++;
		}
		for ( i = 1; i < 256; i++ ) // accumulate counts
			count_gg[1][i] += count_gg[1][i-1];
	}
	
	// analyse and process gg_context
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// find the interval with the highest increase
		for ( lbound = 0, inc0 = 0, inc1 = 0, i = 0; i < 256 - GG_CONTEXT_SIZE; i++ ) {
			inc1 = count_gg[ch][i + GG_CONTEXT_SIZE] - count_gg[ch][i];
			if ( inc1 >= inc0 ) {
				inc0 = inc1;
				lbound = i;
			}
		}
		// process gg_context
		for ( i = 0, gg0 = gg_context[ch]; i < ngr; i++, gg0++ ) {
			if ( *gg0 >= lbound ) {
				*gg0 -= lbound;
				if ( *gg0 >= GG_CONTEXT_SIZE ) *gg0 = GG_CONTEXT_SIZE - 1;
			} else *gg0 = 0;
		}
	}
	
	
	return true;
}





/* -----------------------------------------------
	predict LAME ancillary data
	----------------------------------------------- */
INTERN inline unsigned char* pmp_predict_lame_anc( int nbits, unsigned char* ref )
{
	static thread_local unsigned char* pred = (unsigned char*) calloc( 2048, sizeof( char ) );
	static thread_local unsigned char* lame_str = (unsigned char*) calloc( 4 + 16, sizeof( char ) ); // !!!
	const unsigned char b01 = 0x55;
	const unsigned char b10 = 0xAA;
	const unsigned char b00 = 0x00;
	const unsigned char b11 = 0xFF;
	static thread_local int lame_str_len = 4;
	static thread_local int lame_bit = 0;
	static thread_local bool alt_pred = 0;
	int offset;
	int nbytes;
	int i;
	
	// fprintf( stderr, "state:%i/%i/%02X/%02X ", (alt_pred)?0:1, nbits, pred[0], (ref!=NULL)?ref[0]:0xFF );
	if ( nbits < 0 ) {
		// (re-)init prediction
		lame_bit = 0;
		lame_str_len = 4;
		memcpy( lame_str, "LAME", 4 );
		alt_pred = false;
	} else {
		offset = nbits % 8;
		nbytes = nbits / 8;
		// build prediction
		if ( !alt_pred ) { // option 0 - lame predictor
			for ( i = 0; ( nbits >=8 ) && ( i < 4 ); nbits -= 8, i++ )
				pred[i] = lame_str[i]; // 'LAME'
			if ( nbits >= 32 ) {
				for ( ; ( nbits >= 8 ) && ( i < lame_str_len ); nbits -= 8, i++ )
					pred[i] = lame_str[i]; // version number
			}
			for ( ; nbits > 0; nbits -= 8, i++ )
				pred[i] = ( lame_bit == 0 ) ? b01 : b10; // switching bits
			lame_bit ^= ( offset % 2 );
		} // else: keep the prediction as is
		if ( ref != NULL ) { // compare prediction with reference (if any)
			if ( ( memcmp( pred, ref, nbytes ) == 0 ) && // match
				( ref[nbytes] == pred[nbytes] >> (8-offset) ) ) return NULL;
			else { // no match - check alternatives / relearn version number
				if ( (nbytes >= 8 ) && ( memcmp( ref, lame_str, 4 ) == 0 ) ) { // 'LAME' match
					// careful !!! (ref has to be >=8 in size)
					// relearn version number (if any)
					for ( i = 4; ( i < 4 + 16 ) && ( i < nbytes ); i++ ) {
						if ( ref[i] == b01 ) {
							lame_bit = 0;
							break;
						} else if ( ref[i] == b10 ) {
							lame_bit = 1;
							break;
						}
						lame_str[i] = ref[i];
					}
					lame_str_len = i;
				} else if ( ( nbytes == 0 ) && ( offset > 1 ) ) {
					if ( (b00^ref[0]) >> (8-offset) == 0 ) {
						memset( pred, b00, 2048 );
						alt_pred = true;
					} else if ( (b11^ref[0]) >> (8-offset) == 0 ) {
						memset( pred, b11, 2048 );
						alt_pred = true;
					} else alt_pred = false;
				} else if ( nbytes >= 1 ) {
					if ( b00 == ref[0] ) {
						memset( pred, b00, 2048 );
						alt_pred = true;
					} else if ( b11 == ref[0] ) {
						memset( pred, b11, 2048 );
						alt_pred = true;
					} else alt_pred = false;
				}
			}
		}
		// else: no reference, so prediction is assumed correct
	}
	
	
	return pred;
}

/* ----------------------- End of PMP specific functions -------------------------- */

/* ----------------------- Begin of miscellaneous helper functions -------------------------- */


/* -----------------------------------------------
	displays progress bar on screen
	----------------------------------------------- */
#if !defined(BUILD_LIB)
INTERN inline void progress_bar( int current, int last )
{
	int barpos = ( ( current * BARLEN ) + ( last / 2 ) ) / last;
	int i;
	
	
	// generate progress bar
	fprintf( msgout, "[" );
	#if defined(_WIN32)
	for ( i = 0; i < barpos; i++ )
		fprintf( msgout, "\xFE" );
	#else
	for ( i = 0; i < barpos; i++ )
		fprintf( msgout, "X" );
	#endif
	for (  ; i < BARLEN; i++ )
		fprintf( msgout, " " );
	fprintf( msgout, "]" );
}
#endif

/* -----------------------------------------------
	v1.2 'list' / 'stats' subcommand helpers
	----------------------------------------------- */
#if !defined(BUILD_LIB)
INTERN std::string pmp_human_size( long long bytes )
{
	char buf[ 32 ];
	if ( bytes >= 1024 * 1024 )
		snprintf( buf, sizeof(buf), "%.2f MB", bytes / (1024.0*1024.0) );
	else if ( bytes >= 1024 )
		snprintf( buf, sizeof(buf), "%.1f KB", bytes / 1024.0 );
	else
		snprintf( buf, sizeof(buf), "%lld B", bytes );
	return buf;
}

INTERN const char* pmp_channel_label( void )
{
	switch ( i_channels ) {
		case MP3_STEREO:		return "stereo";
		case MP3_JOINT_STEREO:	return "joint stereo";
		case MP3_DUAL_CHANNEL:	return "dual channel";
		case MP3_MONO:			return "mono";
		default:				return "unknown";
	}
}

INTERN const char* pmp_format_label( void )
{
	switch ( i_mpeg ) {
		case MP3_V1_0:	return "MPEG-1 Layer III";
		case MP3_V2_0:	return "MPEG-2 Layer III";
		case MP3_V2_5:	return "MPEG-2.5 Layer III";
		default:		return "MPEG Layer III";
	}
}

/* ===========================================================================
	Layer I/II (MUSICAM: subband, no Huffman, no bit reservoir)

	Completely separate from the Layer III path. Backed by the packMP2
	library (sibling project: unpack/pack byte-exact MP2<->um2 transform +
	TCAM2/zpaq compression) rather than an in-place arithmetic codec -- see
	l2_compress/l2_decompress below. Archive magic is "M2" so it never
	collides with the Layer III "MS"/"MK".
   =========================================================================== */
#if !defined(BUILD_LIB)
// Layer I/II is now backed by the packMP2 library (unpack/pack + TCAM2/zpaq),
// developed as a sibling project specifically for this integration. The old
// in-place arithmetic codec (bit-alloc/scalefactor/sample modelling) is
// retired in its favor -- packMP2's own container (TCAM/zpaq/RAW2 magic) is
// already self-describing, so this wrapper only needs to add the outer "M2"
// framing packMP3 uses to route files during decompression, plus an extra
// stored-fallback safety net (never fail to compress: worst case, store the
// original bytes verbatim under the same "M2" magic).
INTERN bool l2_compress( void )
{
	int fsize = str_in->getsize();
	if ( fsize <= 0 ) { snprintf( errormessage, MSG_SIZE, "empty input" ); errorlevel = 2; return false; }
	unsigned char* d = (unsigned char*) calloc( fsize, 1 );
	if ( d == NULL ) { snprintf( errormessage, MSG_SIZE, MEM_ERRMSG ); errorlevel = 2; return false; }
	str_in->rewind();
	str_in->read( d, 1, fsize );

	packmp2_opts opts = packmp2_opts_default();
	char pmsg[ 256 ] = {0};
	unsigned char* out = NULL; size_t outlen = 0;
	int rc;
	{
		std::lock_guard<std::mutex> lk( l2_pmp2_mutex );	// packMP2 engine isn't thread-safe
		rc = packmp2_compress( d, (size_t) fsize, &out, &outlen, &opts, pmsg );
	}

	bool stored = ( rc != 0 );	// packmp2 failure -> fall back to verbatim store
	if ( !stored ) {
		unsigned char hd[ 4 ] = { (unsigned char) l2_magic[0], (unsigned char) l2_magic[1], (unsigned char) appversion, 0 };
		str_out->write( hd, 1, 4 );
		str_out->write( out, 1, (int) outlen );
		free( out );
	} else {
		unsigned char hd[ 4 ] = { (unsigned char) l2_magic[0], (unsigned char) l2_magic[1], (unsigned char) appversion, 1 };
		str_out->write( hd, 1, 4 );
		str_out->write( d, 1, fsize );
	}
	mp3filesize = fsize;
	pmpfilesize = str_out->getsize();
	free( d );
	return true;
}

INTERN bool l2_decompress( void )
{
	unsigned char hd[ 4 ];
	str_in->rewind();
	if ( str_in->read( hd, 1, 4 ) != 4 ) { snprintf( errormessage, MSG_SIZE, "truncated archive" ); errorlevel = 2; return false; }
	if ( hd[0] != (unsigned char) l2_magic[0] || hd[1] != (unsigned char) l2_magic[1] ) { snprintf( errormessage, MSG_SIZE, "not a Layer I/II archive" ); errorlevel = 2; return false; }

	int rest = str_in->getsize() - 4;
	unsigned char* payload = (unsigned char*) calloc( ( rest > 0 ? rest : 1 ), 1 );
	str_in->read( payload, 1, rest );

	if ( hd[3] == 1 ) {	// stored fallback: payload is the verbatim original file
		str_out->write( payload, 1, rest );
		free( payload );
		pmpfilesize = str_in->getsize();
		mp3filesize = str_out->getsize();
		return true;
	}

	unsigned char* out = NULL; size_t outlen = 0;
	char pmsg[ 256 ] = {0};
	int rc;
	{
		std::lock_guard<std::mutex> lk( l2_pmp2_mutex );	// packMP2 engine isn't thread-safe
		rc = packmp2_decompress( payload, (size_t) rest, &out, &outlen, pmsg );
	}
	free( payload );
	if ( rc != 0 ) { snprintf( errormessage, MSG_SIZE, "packmp2 decode failed: %s", pmsg ); errorlevel = 2; return false; }

	str_out->write( out, 1, (int) outlen );
	free( out );
	pmpfilesize = str_in->getsize();
	mp3filesize = str_out->getsize();
	return true;
}

// Layer I/II frame size in bytes (read-only, cosmetic use in list_l2/stats_l2
// below -- packMP2 itself owns frame-format knowledge for the codec path,
// this just walks sync words to report frame count / CBR-vs-VBR).
INTERN inline int l2_frame_bytes( int mpeg, int layer, int samples, int bits, int padding )
{
	int br = bitrate_table[ mpeg ][ layer ][ bits ];   // kbps
	int sr = samplerate_table[ mpeg ][ samples ];      // Hz
	if ( br <= 0 || sr <= 0 ) return 0;
	if ( layer == LAYER_I )
		return ( ( 12 * br * 1000 ) / sr + ( padding ? 1 : 0 ) ) * 4;
	return ( 144 * br * 1000 ) / sr + ( padding ? 1 : 0 );	// LAYER_II
}

INTERN const char* l2_format_label( int mpeg, int layer )
{
	switch ( mpeg ) {
		case MP3_V1_0: return ( layer == LAYER_I ) ? "MPEG-1 Layer I"   : "MPEG-1 Layer II";
		case MP3_V2_0: return ( layer == LAYER_I ) ? "MPEG-2 Layer I"   : "MPEG-2 Layer II";
		case MP3_V2_5: return ( layer == LAYER_I ) ? "MPEG-2.5 Layer I" : "MPEG-2.5 Layer II";
		default:       return "MPEG Layer I/II";
	}
}

INTERN const char* l2_channel_label( int mode )
{
	switch ( mode ) {
		case MP3_STEREO:        return "stereo";
		case MP3_JOINT_STEREO:  return "joint stereo";
		case MP3_DUAL_CHANNEL:  return "dual channel";
		case MP3_MONO:          return "mono";
		default:                return "unknown";
	}
}

// Finds the first valid Layer I/II sync word in buf, then walks frame sync
// words from there to count frames and detect VBR. Read-only, no full decode.
INTERN bool l2_scan( const unsigned char* buf, int len, int* out_mpeg, int* out_layer,
                     int* out_channels, int* out_samplerate, int* out_bitrate,
                     bool* out_vbr, long long* out_frames )
{
	int p = 0;
	int mpeg = 0, layer = 0, mode = 0, samplerate = 0, bitrate = 0;
	for ( ; p + 4 <= len; p++ ) {
		if ( buf[p] != 0xFF || ( buf[p+1] & 0xE0 ) != 0xE0 ) continue;
		int lb = ( buf[p+1] >> 1 ) & 0x3, ver = ( buf[p+1] >> 3 ) & 0x3;
		int br = ( buf[p+2] >> 4 ) & 0xF, sr = ( buf[p+2] >> 2 ) & 0x3;
		if ( lb == 0 || ver == 1 || br == 0 || br == 15 || sr == 3 ) continue;
		int lyr = 4 - lb;
		if ( lyr != LAYER_I && lyr != LAYER_II ) continue;
		int hz = samplerate_table[ ver ][ sr ];
		int kbps = bitrate_table[ ver ][ lyr ][ br ];
		if ( hz <= 0 || kbps <= 0 ) continue;
		mpeg = ver; layer = lyr; samplerate = hz; bitrate = kbps;
		mode = ( buf[p+3] >> 6 ) & 0x3;
		break;
	}
	if ( p + 4 > len ) return false;	// no valid Layer I/II sync found

	long long frames = 0; int ref_br = bitrate; bool vbr = false;
	int q = p;
	while ( q + 4 <= len ) {
		if ( buf[q] != 0xFF || ( buf[q+1] & 0xE0 ) != 0xE0 ) break;
		int lb = ( buf[q+1] >> 1 ) & 0x3, ver = ( buf[q+1] >> 3 ) & 0x3;
		int br = ( buf[q+2] >> 4 ) & 0xF, sr = ( buf[q+2] >> 2 ) & 0x3;
		int pad = ( buf[q+2] >> 1 ) & 0x1;
		if ( lb == 0 || ver == 1 || br == 0 || br == 15 || sr == 3 ) break;
		int lyr = 4 - lb;
		if ( lyr != layer || ver != mpeg ) break;	// format change: stop counting
		int kbps = bitrate_table[ ver ][ lyr ][ br ];
		int fsize = l2_frame_bytes( ver, lyr, sr, br, pad );
		if ( kbps <= 0 || fsize <= 0 ) break;
		if ( kbps != ref_br ) vbr = true;
		frames++;
		q += fsize;
	}

	*out_mpeg = mpeg; *out_layer = layer; *out_channels = mode;
	*out_samplerate = samplerate; *out_bitrate = bitrate;
	*out_vbr = vbr; *out_frames = frames;
	return true;
}

// 'stats' on a raw Layer I/II input file: sync-scan only, no packmp2 call.
INTERN bool stats_l2( void )
{
	long long sz = (long long) str_in->getsize();
	unsigned char* buf = (unsigned char*) malloc( sz > 0 ? (size_t) sz : 1 );
	if ( buf == NULL ) { snprintf( errormessage, MSG_SIZE, MEM_ERRMSG ); errorlevel = 2; return false; }
	str_in->rewind();
	str_in->read( buf, 1, (int) sz );

	int mpeg, layer, channels, samplerate, bitrate; bool vbr; long long frames;
	bool ok = l2_scan( buf, (int) sz, &mpeg, &layer, &channels, &samplerate, &bitrate, &vbr, &frames );
	free( buf );
	if ( !ok ) { snprintf( errormessage, MSG_SIZE, "no valid Layer I/II frame found" ); errorlevel = 2; return false; }

	fprintf( msgout, "  size     : %s\n", pmp_human_size( sz ).c_str() );
	fprintf( msgout, "  format   : %s\n", l2_format_label( mpeg, layer ) );
	fprintf( msgout, "  frames   : %lli\n", frames );
	fprintf( msgout, "  channels : %i (%s)\n", ( channels == MP3_MONO ) ? 1 : 2, l2_channel_label( channels ) );
	fprintf( msgout, "  rate     : %i Hz\n", samplerate );
	if ( !vbr ) fprintf( msgout, "  bitrate  : %i kbps (CBR)\n", bitrate );
	else        fprintf( msgout, "  bitrate  : VBR / not global\n" );

	pmpfilesize = 0;
	mp3filesize = (int) sz;
	return true;
}

// 'list' on an F_PL2 ("M2") archive: peeks the outer header, then either
// reads the stored-fallback payload directly or runs packmp2_decompress
// in-memory to recover the raw Layer I/II bytes for a sync-scan. packMP2
// files are small enough that a full decompress here is cheap (unlike
// Layer III's arithmetic main data, which list_pmp deliberately avoids).
INTERN bool list_l2( void )
{
	long long sz = (long long) str_in->getsize();
	unsigned char hd[ 4 ];
	str_in->rewind();
	if ( str_in->read( hd, 1, 4 ) != 4 || hd[0] != (unsigned char) l2_magic[0] || hd[1] != (unsigned char) l2_magic[1] ) {
		snprintf( errormessage, MSG_SIZE, "not a Layer I/II archive" );
		errorlevel = 2;
		return false;
	}
	int vmaj = hd[2] / 10, vmin = hd[2] % 10;
	int rest = (int) sz - 4;
	unsigned char* payload = (unsigned char*) malloc( rest > 0 ? (size_t) rest : 1 );
	if ( payload == NULL ) { snprintf( errormessage, MSG_SIZE, MEM_ERRMSG ); errorlevel = 2; return false; }
	str_in->read( payload, 1, rest );

	unsigned char* raw = payload; size_t rawlen = (size_t) rest; bool free_raw = false;
	const char* method = "stored (verbatim)";
	if ( hd[3] != 1 ) {
		unsigned char* out = NULL; size_t outlen = 0;
		char pmsg[ 256 ] = {0};
		int rc;
		{
			std::lock_guard<std::mutex> lk( l2_pmp2_mutex );	// packMP2 engine isn't thread-safe
			rc = packmp2_decompress( payload, (size_t) rest, &out, &outlen, pmsg );
		}
		free( payload );
		if ( rc != 0 ) { snprintf( errormessage, MSG_SIZE, "packmp2 decode failed: %s", pmsg ); errorlevel = 2; return false; }
		raw = out; rawlen = outlen; free_raw = true;
		method = "packMP2 (zstd/zpaq)";
	}

	int mpeg, layer, channels, samplerate, bitrate; bool vbr; long long frames;
	bool ok = l2_scan( raw, (int) rawlen, &mpeg, &layer, &channels, &samplerate, &bitrate, &vbr, &frames );
	if ( free_raw ) free( raw ); else free( payload );
	if ( !ok ) { snprintf( errormessage, MSG_SIZE, "no valid Layer I/II frame found" ); errorlevel = 2; return false; }

	fprintf( msgout, "  version  : v%i.%i\n", vmaj, vmin );
	fprintf( msgout, "  packed   : %s\n", pmp_human_size( sz ).c_str() );
	fprintf( msgout, "  original : %s\n", pmp_human_size( (long long) rawlen ).c_str() );
	fprintf( msgout, "  method   : %s (packMP2 v%s)\n", method, packmp2_version() );
	fprintf( msgout, "  format   : %s\n", l2_format_label( mpeg, layer ) );
	fprintf( msgout, "  frames   : %lli\n", frames );
	fprintf( msgout, "  channels : %i (%s)\n", ( channels == MP3_MONO ) ? 1 : 2, l2_channel_label( channels ) );
	fprintf( msgout, "  rate     : %i Hz\n", samplerate );
	if ( !vbr ) fprintf( msgout, "  bitrate  : %i kbps (CBR)\n", bitrate );
	else        fprintf( msgout, "  bitrate  : VBR / not global\n" );

	pmpfilesize = (int) sz;
	mp3filesize = 0;
	return true;
}
#endif


// 'list': show PMP archive info without decompressing the audio.
// Only the header + per-frame side info is read (pmp_read_header), never the
// arithmetic-coded main data, so this is cheap.
INTERN bool list_pmp( void )
{
	long long sz = (long long) str_in->getsize();

	if ( arch_chunked ) {
		// "MK" container: header + N independent "MS" sub-streams. Walk the
		// chunk table and read each sub-stream's own header (cheap, no main
		// data decode), summing frames and checking format/bitrate consistency.
		unsigned char* a = (unsigned char*) malloc( (size_t) sz );
		if ( a == NULL ) { snprintf( errormessage, MSG_SIZE, MEM_ERRMSG ); errorlevel = 2; return false; }
		str_in->rewind();
		str_in->read( a, 1, (int) sz );
		if ( sz < 5 ) { snprintf( errormessage, MSG_SIZE, "corrupt chunked archive" ); errorlevel = 2; free( a ); return false; }
		int vmaj = a[2] / 10, vmin = a[2] % 10;
		int nch = a[3];
		if ( nch < 1 || nch > MAX_CHUNKS ) { snprintf( errormessage, MSG_SIZE, "corrupt chunked archive" ); errorlevel = 2; free( a ); return false; }
		int o = 4; int sizes[ MAX_CHUNKS ];
		for ( int i = 0; i < nch; i++ ) { sizes[i] = a[o]|(a[o+1]<<8)|(a[o+2]<<16)|(a[o+3]<<24); o += 4; }
		int offs[ MAX_CHUNKS ]; { int p = o; for ( int i = 0; i < nch; i++ ) { offs[i] = p; p += sizes[i]; } }

		iostream* real_in = str_in;
		long long tot_frames = 0; int ref_ch = -1, ref_sr = -1, ref_br = -1; bool br_varies = false;
		const char* fmt = NULL; const char* ch_label = NULL;
		bool ok = true;
		for ( int i = 0; i < nch && ok; i++ ) {
			str_in = new iostream( a + offs[i], 1, sizes[i], 0 );
			unsigned char hb = 0;
			str_in->read( &hb, 1, 1 ); str_in->read( &hb, 1, 1 ); // sub-archive magic (MS)
			str_in->read( &hb, 1, 1 );	// sub-archive version byte
			pmp_archive_version = hb;
			ok = pmp_read_header( str_in );
			if ( ok ) {
				tot_frames += g_nframes;
				// snapshot labels/refs BEFORE reset_buffers() clears i_channels etc.
				if ( fmt == NULL ) fmt = pmp_format_label();
				if ( ref_ch == -1 ) { ref_ch = g_nchannels; ref_sr = g_samplerate; ref_br = g_bitrate; ch_label = pmp_channel_label(); }
				else if ( g_bitrate != ref_br ) br_varies = true;
			}
			delete( str_in );
			reset_buffers();
		}
		str_in = real_in;
		free( a );
		if ( !ok ) return false;

		fprintf( msgout, "  version  : v%i.%i\n", vmaj, vmin );
		fprintf( msgout, "  packed   : %s\n", pmp_human_size( sz ).c_str() );
		fprintf( msgout, "  chunks   : %i (intra-file parallel)\n", nch );
		fprintf( msgout, "  format   : %s\n", fmt );
		fprintf( msgout, "  frames   : %lli\n", tot_frames );
		fprintf( msgout, "  channels : %i (%s)\n", ref_ch, ch_label );
		fprintf( msgout, "  rate     : %i Hz\n", ref_sr );
		if ( !br_varies && ref_br > 0 ) fprintf( msgout, "  bitrate  : %i kbps (CBR)\n", ref_br );
		else                            fprintf( msgout, "  bitrate  : VBR / not global\n" );

		pmpfilesize = (int) sz;
		mp3filesize = 0;
		return true;
	}

	unsigned char b = 0;
	str_in->rewind();
	str_in->read( &b, 1, 1 );	// magic byte 0
	str_in->read( &b, 1, 1 );	// magic byte 1
	str_in->read( &b, 1, 1 );	// version byte
	int vmaj = b / 10, vmin = b % 10;
	pmp_archive_version = b;	// so pmp_read_header reads the MPEG-version bits (v1.3+)

	if ( !pmp_read_header( str_in ) ) return false;

	fprintf( msgout, "  version  : v%i.%i\n", vmaj, vmin );
	fprintf( msgout, "  packed   : %s\n", pmp_human_size( sz ).c_str() );
	fprintf( msgout, "  format   : %s\n", pmp_format_label() );
	fprintf( msgout, "  frames   : %i\n", g_nframes );
	fprintf( msgout, "  channels : %i (%s)\n", g_nchannels, pmp_channel_label() );
	fprintf( msgout, "  rate     : %i Hz\n", g_samplerate );
	if ( g_bitrate > 0 ) fprintf( msgout, "  bitrate  : %i kbps (CBR)\n", g_bitrate );
	else                 fprintf( msgout, "  bitrate  : VBR / not global\n" );

	pmpfilesize = (int) sz;
	mp3filesize = 0;
	return true;
}

// 'stats': show MP3 info. Runs after read_mp3 + analyze_frames have populated
// the i_*/g_* globals, so it only prints.
INTERN bool stats_mp3( void )
{
	fprintf( msgout, "  size     : %s\n", pmp_human_size( (long long) mp3filesize ).c_str() );
	fprintf( msgout, "  format   : %s\n", pmp_format_label() );
	fprintf( msgout, "  frames   : %i\n", g_nframes );
	fprintf( msgout, "  channels : %i (%s)\n", g_nchannels, pmp_channel_label() );
	fprintf( msgout, "  rate     : %i Hz\n", g_samplerate );
	if ( g_bitrate > 0 ) fprintf( msgout, "  bitrate  : %i kbps (CBR)\n", g_bitrate );
	else                 fprintf( msgout, "  bitrate  : VBR / not global\n" );

	pmpfilesize = 0;
	return true;
}
#endif


/* -----------------------------------------------
	creates filename, callocs memory for it
	----------------------------------------------- */
#if !defined(BUILD_LIB)
INTERN inline char* create_filename( const char* base, const char* extension )
{
	// v1.2: when outdir is set, drop base's directory prefix and place
	// the file under outdir. -fs: when -r expanded a dir AND outdir is
	// set, mirror the path's relative subdir from src_root under outdir
	// (caesium-clt -RS semantics).
	const char* basename_only = base;
	std::string fs_subdir; // set only when -fs applies
	if ( outdir != NULL ) {
		const char* sep = strrchr( base, '/' );
	#if defined(_WIN32) || defined(WIN32)
		const char* sep2 = strrchr( base, '\\' );
		if ( sep2 > sep ) sep = sep2;
	#endif
		if ( sep != NULL ) basename_only = sep + 1;

		if ( fs_mode && filelist_srcroot != NULL
		     && file_no >= 0 && filelist_srcroot[ file_no ] != NULL ) {
			std::error_code ec;
			std::filesystem::path full_p( base );
			std::filesystem::path root_p( filelist_srcroot[ file_no ] );
			std::filesystem::path rel = std::filesystem::relative(
				full_p.parent_path(), root_p, ec );
			if ( !ec && !rel.empty() && rel.string() != "." ) {
				fs_subdir = rel.string();
			}
		}
	}

	int dirlen = ( outdir != NULL ) ? (int)strlen( outdir ) + 1 : 0;
	int sublen = fs_subdir.empty() ? 0 : (int)fs_subdir.size() + 1;
	int len = dirlen + sublen + (int)strlen( basename_only )
	          + ( ( extension == NULL ) ? 0 : (int)strlen( extension ) + 1 ) + 1;
	char* filename = (char*) calloc( len, sizeof( char ) );

	if ( outdir != NULL ) {
		strcpy( filename, outdir );
		int dl = (int)strlen( outdir );
		if ( dl > 0 && outdir[ dl-1 ] != '/'
	#if defined(_WIN32) || defined(WIN32)
			&& outdir[ dl-1 ] != '\\'
	#endif
		) strcat( filename, "/" );
		if ( !fs_subdir.empty() ) {
			strcat( filename, fs_subdir.c_str() );
			strcat( filename, "/" );
		}
		// ensure the output directory exists (-od on its own, and -od + -fs).
		// filename currently holds the directory path (ends with '/').
		// -dry never writes, so don't create directories either.
		if ( !dry_run ) {
			std::error_code ec;
			std::filesystem::create_directories( std::filesystem::path( filename ), ec );
		}
		strcat( filename, basename_only );
	} else {
		strcpy( filename, base );
	}
	set_extension( filename, extension );

	return filename;
}
#endif

/* -----------------------------------------------
	creates filename, callocs memory for it
	----------------------------------------------- */
#if !defined(BUILD_LIB)
INTERN inline char* unique_filename( const char* base, const char* extension )
{
	// v1.2: build the base candidate via create_filename so -od (output dir)
	// and -fs (folder structure) are honoured in non-overwrite mode too.
	// Previously this strcpy'd base directly and silently ignored -od.
	char* filename = create_filename( base, extension );
	if ( filename == NULL ) return NULL;

	// guarantee +2 bytes of headroom for the first add_underscore
	int len = (int) strlen( filename ) + 2;
	char* tmp = (char*) realloc( filename, len );
	if ( tmp == NULL ) { free( filename ); return NULL; }
	filename = tmp;

	// create a unique filename using underscores
	while ( file_exists( filename ) ) {
		len += 1;
		tmp = (char*) realloc( filename, len );
		if ( tmp == NULL ) { free( filename ); return NULL; }
		filename = tmp;
		add_underscore( filename );
	}

	return filename;
}
#endif

/* -----------------------------------------------
	changes extension of filename
	----------------------------------------------- */
#if !defined(BUILD_LIB)
INTERN inline void set_extension( char* filename, const char* extension )
{
	char* extstr;
	
	// find position of extension in filename	
	extstr = ( strrchr( filename, '.' ) == NULL ) ?
		strrchr( filename, '\0' ) : strrchr( filename, '.' );
	
	// set new extension
	if ( extension != NULL ) {
		(*extstr++) = '.';
		strcpy( extstr, extension );
	}
	else
		(*extstr) = '\0';
}
#endif

/* -----------------------------------------------
	adds underscore after filename
	----------------------------------------------- */
#if !defined(BUILD_LIB)
INTERN inline void add_underscore( char* filename )
{
	char* tmpname = (char*) calloc( strlen( filename ) + 1, sizeof( char ) );
	char* extstr;
	
	// copy filename to tmpname
	strcpy( tmpname, filename );
	// search extension in filename
	extstr = strrchr( filename, '.' );
	
	// add underscore before extension
	if ( extstr != NULL ) {
		(*extstr++) = '_';
		strcpy( extstr, strrchr( tmpname, '.' ) );
	}
	else {
		size_t _len = strlen( tmpname );
		// safe: unique_filename guarantees +2 bytes of headroom for this case
		memcpy( filename, tmpname, _len );
		filename[ _len ] = '_';
		filename[ _len + 1 ] = '\0';
	}
	
	// free memory
	free( tmpname );
}
#endif

/* -----------------------------------------------
	checks if a file exists
	----------------------------------------------- */
INTERN inline bool file_exists( const char* filename )
{
	// needed for both, executable and library
	FILE* fp = fopen( filename, "rb" );
	
	if ( fp == NULL ) return false;
	else {
		fclose( fp );
		return true;
	}
}

/* ----------------------- End of miscellaneous helper functions -------------------------- */

/* ----------------------- Begin of developers functions -------------------------- */


#if !defined(BUILD_LIB) && defined(DEV_BUILD)
/* -----------------------------------------------
	Writes to file
	----------------------------------------------- */
INTERN bool write_file( const char* base, const char* ext, void* data, int bpv, int size )
{	
	FILE* fp;
	char* fn;
	
	// create filename
	fn = create_filename( base, ext );
	
	// open file for output
	fp = fopen( fn, "wb" );	
	if ( fp == NULL ) {
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		free( fn );
		errorlevel = 2;
		return false;
	}
	free( fn );
	
	// write & close
	fwrite( data, bpv, size, fp );
	fclose( fp );
	
	return true;
}


/* -----------------------------------------------
	Writes error info file
	----------------------------------------------- */
INTERN bool write_errfile( void )
{
	FILE* fp;
	char* fn;
	
	
	// return immediately if theres no error
	if ( errorlevel == 0 ) return true;
	
	// create filename based on errorlevel
	if ( errorlevel == 1 ) {
		fn = create_filename( filelist[ file_no ], "wrn.nfo" );
	}
	else {
		fn = create_filename( filelist[ file_no ], "err.nfo" );
	}
	
	// open file for output
	fp = fopen( fn, "w" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		free( fn );
		errorlevel = 2;
		return false;
	}
	free( fn );
	
	// write status and errormessage to file
	fprintf( fp, "--> error (level %i) in file \"%s\" <--\n", errorlevel, filelist[ file_no ] );
	fprintf( fp, "\n" );
	// write error specification to file
	fprintf( fp, " %s -> %s:\n", get_status( errorfunction ),
			( errorlevel == 1 ) ? "warning" : "error" );
	fprintf( fp, " %s\n", errormessage );
	
	// done, close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	Writes info to a specific csv file
	----------------------------------------------- */
INTERN bool write_file_analysis( void )
{
	static const char* fn = FILE_ANALYSIS_CSV;
	FILE* fp;	
	bool labels;
	
	mp3Frame* frame;
	granuleInfo* granule;
	int total_fixed = 0;
	int total_scf   = 0;
	int total_coef  = 0;
	int total_aux   = 0;
	int main_bits;
	int cur_scf;
	int share = 0;
	const int* slen;
	int ch, gr;
	
	
	// check if file exists
	labels = !file_exists( fn );
	
	// open file for output
	fp = fopen( fn, "a" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	
	
	// calculate sizes of scf/coef/aux/fixed
	for ( frame = firstframe; frame != NULL; frame = frame->next ) {
		main_bits = 0; cur_scf = 0;
		for ( ch = 0; ch < g_nchannels; ch++ ) for ( gr = 0; gr < 2; gr++ ) {
			granule = frame->granules[ch][gr];
			slen = slength_table[ (int) granule->slength ];
			main_bits += granule->main_data_bit;
			if ( granule->block_type != SHORT_BLOCK ) {
				if ( ( ( share >> 3 ) & 1 ) == 0 ) cur_scf += slen[ 0 ] * 6;
				if ( ( ( share >> 2 ) & 1 ) == 0 ) cur_scf += slen[ 0 ] * 5;
				if ( ( ( share >> 1 ) & 1 ) == 0 ) cur_scf += slen[ 1 ] * 5;
				if ( ( ( share >> 0 ) & 1 ) == 0 ) cur_scf += slen[ 1 ] * 5;
			} else {
				cur_scf += slen[ 0 ] * 6 * 3;
				cur_scf += slen[ 1 ] * 6 * 3;
			}			
			share = granule->share;
		}
		total_fixed += frame->fixed_size * 8;
		total_scf += cur_scf;
		total_coef += main_bits - cur_scf;
		total_aux += frame->aux_size * 8;
		if ( main_bits % 8 > 0 )
			total_aux += 8 - ( main_bits % 8 );
	}
	
	// write labels
	if ( labels ) {
		fprintf( fp, "name;" );
		fprintf( fp, "total_bits;" );
		fprintf( fp, "tag_bits;" );
		fprintf( fp, "fixed_bits;" );
		fprintf( fp, "scf_bits;" );
		fprintf( fp, "coef_bits;" );
		fprintf( fp, "aux_bits;" );
		fprintf( fp, "frames;" );
		fprintf( fp, "mpeg;" );
		fprintf( fp, "layer;" );
		fprintf( fp, "samples;" );
		fprintf( fp, "bitrate;" );
		fprintf( fp, "channels;" );
		fprintf( fp, "protection;" );
		fprintf( fp, "padding;" );
		fprintf( fp, "ms_stereo;" );
		fprintf( fp, "int_stereo;" );
		fprintf( fp, "private;" );
		fprintf( fp, "copyright;" );
		fprintf( fp, "original;" );
		fprintf( fp, "emphasis;" );
		fprintf( fp, "padbits;" );
		fprintf( fp, "bitres;" );
		fprintf( fp, "sharing;" );
		fprintf( fp, "switching;" );
		fprintf( fp, "mixed;" );
		fprintf( fp, "preemphasis;" );
		fprintf( fp, "coarse;" );
		fprintf( fp, "sbgain;" );
		fprintf( fp, "auxiliary_h;" );
		fprintf( fp, "sblock_diff;" );
		fprintf( fp, "bad_first" );
		fprintf( fp, "\n" );
	}
	
	// write data
	fprintf( fp, "%s;", mp3filename );
	fprintf( fp, "%i;", mp3filesize * 8 );
	fprintf( fp, "%i;", ( data_before_size + data_after_size ) * 8 );
	fprintf( fp, "%i;", total_fixed );
	fprintf( fp, "%i;", total_scf );
	fprintf( fp, "%i;", total_coef );
	fprintf( fp, "%i;", total_aux );
	fprintf( fp, "%i;", g_nframes );
	fprintf( fp, "%s;", mpeg_description[(int)i_mpeg] );
	fprintf( fp, "%s;", layer_description[(int)i_layer] );
	fprintf( fp, "%i;", g_samplerate );
	fprintf( fp, "%i;", g_bitrate * 1000 );
	fprintf( fp, "%s;", channels_description[(int)i_channels ] );
	fprintf( fp, "%i;", i_protection );
	fprintf( fp, "%i;", i_padding );
	fprintf( fp, "%i;", i_stereo_ms );
	fprintf( fp, "%i;", i_stereo_int );
	fprintf( fp, "%i;", i_privbit );
	fprintf( fp, "%i;", i_copyright );
	fprintf( fp, "%i;", i_original );
	fprintf( fp, "%i;", i_emphasis );
	fprintf( fp, "%i;", i_padbits );
	fprintf( fp, "%i;", i_bit_res);
	fprintf( fp, "%i;", i_share );
	fprintf( fp, "%i;", i_sblocks );
	fprintf( fp, "%i;", i_mixed );
	fprintf( fp, "%i;", i_preemphasis );
	fprintf( fp, "%i;", i_coarse );
	fprintf( fp, "%i;", i_sbgain );
	fprintf( fp, "%i;", i_aux_h );
	fprintf( fp, "%i;", i_sb_diff );
	fprintf( fp, "%i", n_bad_first );
	fprintf( fp, "\n" );
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	Writes block analysis to csv
	----------------------------------------------- */
INTERN bool write_block_analysis( void )
{
	const char* frm_ext = "frm.csv";
	const char* ch_ext[2] = { "ch0.csv", "ch1.csv" };	
	FILE* fp;
	char* fn;
	
	mp3Frame* frame;
	int ch, gr;
	
	
	/* --- frames analysis --- */
	
	// create filename
	fn = create_filename( filelist[ file_no ], frm_ext );
	
	// open file for output
	fp = fopen( fn, "w" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		free( fn );
		errorlevel = 2;
		return false;
	}
	free( fn );
	
	// write labels (header)
	fprintf( fp, "frame;" );
	fprintf( fp, "main_index;" );
	fprintf( fp, "frame_size;" );
	fprintf( fp, "fixed_size;" );
	fprintf( fp, "bit_reservoir;");
	fprintf( fp, "main_bits;" );
	fprintf( fp, "main_size;" );
	fprintf( fp, "aux_size;" );
	fprintf( fp, "mpeg;");
	fprintf( fp, "layer;");
	fprintf( fp, "samples;");
	fprintf( fp, "bitrate;");
	fprintf( fp, "channels;");
	fprintf( fp, "protection;");
	fprintf( fp, "padding;");
	fprintf( fp, "ms_stereo;");
	fprintf( fp, "int_stereo;");
	fprintf( fp, "private;");
	fprintf( fp, "copyright;");
	fprintf( fp, "original;");
	fprintf( fp, "emphasis;");
	fprintf( fp, "padbits");
	fprintf( fp, "\n" );
	
	// write frame data
	for ( frame = firstframe; frame != NULL; frame = frame->next ) {
		// header/frame-global info
		fprintf( fp, "%i;", frame->n );
		fprintf( fp, "%i;", frame->main_index );
		fprintf( fp, "%i;", frame->frame_size );
		fprintf( fp, "%i;", frame->fixed_size );
		fprintf( fp, "%i;", frame->bit_reservoir );
		fprintf( fp, "%i;", frame->main_bits );
		fprintf( fp, "%i;", frame->main_size );
		fprintf( fp, "%i;", frame->aux_size );
		fprintf( fp, "%s;", mpeg_description[(int)frame->mpeg ] );
		fprintf( fp, "%s;", layer_description[(int)frame->layer ] );
		fprintf( fp, "%i;", mp3_samplerate_table[(int)frame->samples] );
		fprintf( fp, "%i;", mp3_bitrate_table[(int)frame->bits] * 1000 );
		fprintf( fp, "%s;", channels_description[(int)frame->channels ] );
		fprintf( fp, "%i;", frame->protection );
		fprintf( fp, "%i;", frame->padding );
		fprintf( fp, "%i;", frame->stereo_ms );
		fprintf( fp, "%i;", frame->stereo_int );
		fprintf( fp, "%i;", frame->privbit );
		fprintf( fp, "%i;", frame->copyright );
		fprintf( fp, "%i;", frame->original );
		fprintf( fp, "%i;", frame->emphasis );
		fprintf( fp, "%i;", frame->padbits );
		fprintf( fp, "\n" );
	}
	
	// close file
	fclose( fp );
	
	
	/* --- granules analysis --- */
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// create filename
		fn = create_filename( filelist[ file_no ], ch_ext[ch] );
		
		// open file for output
		fp = fopen( fn, "w" );
		if ( fp == NULL ){
			snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
			errorlevel = 2;
			return false;
		}
		free( fn );
		
		// labels (granules)
		fprintf( fp, "block;" );
		fprintf( fp, "ms_stereo;" );
		fprintf( fp, "int_stereo;" );
		fprintf( fp, "share;" );
		fprintf( fp, "main_bit;" );
		fprintf( fp, "big_vals;" );
		fprintf( fp, "gl_gain;" );
		fprintf( fp, "slength;" );
		fprintf( fp, "switching;" );
		fprintf( fp, "block_type;" );
		fprintf( fp, "mixed;" );
		fprintf( fp, "preemphasis;" );
		fprintf( fp, "coarse;" );
		fprintf( fp, "r0_tbl;" );
		fprintf( fp, "r1_tbl;" );
		fprintf( fp, "r2_tbl;" );
		fprintf( fp, "sm_tbl;" );
		fprintf( fp, "r0_size;" );
		fprintf( fp, "r1_size;" );
		fprintf( fp, "r0_bound;" );
		fprintf( fp, "r1_bound;" );
		fprintf( fp, "r2_bound;" );
		fprintf( fp, "sv_bound;" );
		fprintf( fp, "sb0_gain;" );
		fprintf( fp, "sb1_gain;" );
		fprintf( fp, "sb2_gain" );
		fprintf( fp, "\n" );
		
		// write granules data
		for ( frame = firstframe; frame != NULL; frame = frame->next ) if ( ch < frame->nchannels ) {
			for ( gr = 0; gr < 2; gr++ ) {
				fprintf( fp, "%i;", frame->granules[ch][gr]->n );
				fprintf( fp, "%i;", frame->stereo_ms );
				fprintf( fp, "%i;", frame->stereo_int );
				fprintf( fp, "0b%i%i%i%i;",
					BITN( frame->granules[ch][gr]->share, 3 ),
					BITN( frame->granules[ch][gr]->share, 2 ),
					BITN( frame->granules[ch][gr]->share, 1 ),
					BITN( frame->granules[ch][gr]->share, 0 ) );
				fprintf( fp, "%i;", frame->granules[ch][gr]->main_data_bit );
				fprintf( fp, "%i;", frame->granules[ch][gr]->big_val_pairs );
				fprintf( fp, "%i;", frame->granules[ch][gr]->global_gain );
				fprintf( fp, "%i/%i;", slength_table[ (int) frame->granules[ch][gr]->slength ][ 0 ], slength_table[ (int) frame->granules[ch][gr]->slength ][ 1 ] );
				fprintf( fp, "%i;", frame->granules[ch][gr]->window_switching );
				fprintf( fp, "%s;", blocktype_description[(int)frame->granules[ch][gr]->block_type] );
				fprintf( fp, "%i;", frame->granules[ch][gr]->mixed_flag );
				fprintf( fp, "%i;", frame->granules[ch][gr]->preemphasis );
				fprintf( fp, "%i;", frame->granules[ch][gr]->coarse_scalefactors );
				fprintf( fp, "%i;", frame->granules[ch][gr]->region_table[0] );
				fprintf( fp, "%i;", frame->granules[ch][gr]->region_table[1] );
				fprintf( fp, "%i;", frame->granules[ch][gr]->region_table[2] );
				fprintf( fp, "%i;", frame->granules[ch][gr]->select_htabB );
				fprintf( fp, "%i;", frame->granules[ch][gr]->region0_size );
				fprintf( fp, "%i;", frame->granules[ch][gr]->region1_size );
				fprintf( fp, "%i;", frame->granules[ch][gr]->region_bound[0] );
				fprintf( fp, "%i;", frame->granules[ch][gr]->region_bound[1] );
				fprintf( fp, "%i;", frame->granules[ch][gr]->region_bound[2] );
				fprintf( fp, "%i;", frame->granules[ch][gr]->sv_bound );
				fprintf( fp, "%i;", frame->granules[ch][gr]->sb_gain[0] );
				fprintf( fp, "%i;", frame->granules[ch][gr]->sb_gain[1] );
				fprintf( fp, "%i", frame->granules[ch][gr]->sb_gain[2] );
				fprintf( fp, "\n" );
			}
		}
	
		// close file
		fclose( fp );
	}
	
	
	return true;
}


/* -----------------------------------------------
	Writes statistic info to a specific csv file
	----------------------------------------------- */
INTERN bool write_stat_analysis( void )
{
	static const char* fn = STAT_ANALYSIS_CSV;
	FILE* fp;
	bool labels;
	
	huffman_reader* decoder;
	mp3Frame* frame;
	granuleInfo* granule;
	granuleData*** frame_data;
	int ch, gr;
	int i;
	
	unsigned int coef_stats[ 16 ] = { 0 };
	unsigned int coefb_stats[ 15 ] = { 0 };
	unsigned int scf_stats[ 16 ] = { 0 };
	unsigned int bv_tbl_stats[ 32 ] = { 0 };
	unsigned int sv_tbl_stats[ 2 ] = { 0 };
	unsigned int blt_stats[ 4 ] = { 0 };
	unsigned int n_bad_bv_bound = 0;
	unsigned int n_bad_sv_bound = 0;
	int bv_bound_max_dist = 0;
	int sv_bound_max_dist = 0;
	int bv_bound_avrg = 0;
	int sv_bound_avrg = 0;
	int bad_code = 0;
	int r2bvs_avrg = 0;
	
	
	
	// check if file exists
	labels = !file_exists( fn );
	
	// open file for output
	fp = fopen( fn, "a" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	
	// write labels
	if ( labels ) {
		fprintf( fp, "name;" );
		fprintf( fp, "n_frames;" );
		fprintf( fp, "muted;" );
		fprintf( fp, "bad_code;" );
		for ( i = 0; i < 15; i++ )
			fprintf( fp, "cf=%i;", i );
		fprintf( fp, "cf>=15;" );
		for ( i = 0; i < 15; i++ )
			fprintf( fp, "cb=%i;", i );
		for ( i = 0; i < 16; i++ )
			fprintf( fp, "sc=%i;", i );
		for ( i = 0; i < 32; i++ )
			fprintf( fp, "bv_tbl%i;", i );
		fprintf( fp, "sv_tblA;" );
		fprintf( fp, "sv_tblB;" );
		fprintf( fp, "long;" );
		fprintf( fp, "start;" );
		fprintf( fp, "short;" );
		fprintf( fp, "stop;" );
		fprintf( fp, "n_bad_bv;" );
		fprintf( fp, "bad_bv_lim;" );
		fprintf( fp, "bad_bv_avr;" );
		fprintf( fp, "n_bad_sv;" );
		fprintf( fp, "bad_sv_lim;" );
		fprintf( fp, "bad_sv_avr;" );
		fprintf( fp, "reg2_bv_avr" );
		fprintf( fp, "\n" );
	}
	
	// init decoder	
	decoder = new huffman_reader( main_data, main_data_size );
	
	// run statistical analysis
	for ( frame = firstframe; frame != NULL; frame = frame->next ) {
		// skip bad first frames
		if ( frame->n < n_bad_first ) continue;
		frame_data = mp3_decode_frame( decoder, frame );
		if ( frame_data == NULL ) bad_code++;
		for ( ch = 0; ch < frame->nchannels; ch++ ) {
			for ( gr = 0; gr < 2; gr++ ) {
				granule = frame->granules[ch][gr];
				// simple stuff first
				bv_tbl_stats[ (int) granule->region_table[0] ]++;
				bv_tbl_stats[ (int) granule->region_table[1] ]++;
				sv_tbl_stats[ (int) granule->select_htabB ]++;
				blt_stats[ (int) granule->block_type ]++;
				if ( !granule->window_switching )
					bv_tbl_stats[ (int) granule->region_table[2] ]++;
				if ( frame_data == NULL ) continue;
				// count coefficients and scalefactors
				if ( granule->block_type != SHORT_BLOCK ) {
					for ( i = 0; i < 21; i++ )
						scf_stats[ (int) frame_data[ch][gr]->scalefactors[i] ]++;
				} else {
					for ( i = 0; i < 36; i++ )
						scf_stats[ (int) frame_data[ch][gr]->scalefactors[i] ]++;
				}
				for ( i = 0; i < 576; i++ ) {
					if ( ABS(frame_data[ch][gr]->coefficients[i]) < 16 )
						coef_stats[ ABS(frame_data[ch][gr]->coefficients[i]) ]++;
					else coef_stats[ 15 ]++;
					coefb_stats[ BITLEN8224N(frame_data[ch][gr]->coefficients[i]) ]++;
				}
				// check for bad sv bounds
				for ( i = 0; i < granule->sv_bound; i++ )
					if ( frame_data[ch][gr]->coefficients[granule->sv_bound-i-1] != 0 ) break;
				i /= 4;
				if ( i > 0 ) {
					n_bad_sv_bound++;
					sv_bound_avrg += i;
					if ( sv_bound_max_dist < i ) sv_bound_max_dist = i;
				}
				// done if zero table
				// if 	( granule->window_switching ) if ( granule->region_table[2] == 0 ) continue;
				// else if ( granule->region_table[1] == 0 ) continue;
				// check for bad bv bounds
				for ( i = 0; i < granule->region_bound[2]; i++ )
					if ( ABS(frame_data[ch][gr]->coefficients[granule->region_bound[2]-i-1]) > 1 ) break;
				i /= 2;
				if ( i > 1 ) {
					n_bad_bv_bound++;
					bv_bound_avrg += i;
					if ( bv_bound_max_dist < i ) bv_bound_max_dist = i;
				}
				// count region 2 coefficients ABS > 2
				for ( i = granule->region_bound[(granule->window_switching)?0:1]; i < granule->region_bound[2]; i++ )
					if ( ABS(frame_data[ch][gr]->coefficients[i]) > 1 ) r2bvs_avrg++;
			}
		}
	}
	sv_bound_avrg = ( n_bad_sv_bound > 0 ) ? sv_bound_avrg / n_bad_sv_bound : 0;
	bv_bound_avrg = ( n_bad_bv_bound > 0 ) ? bv_bound_avrg / n_bad_bv_bound : 0;
	r2bvs_avrg = r2bvs_avrg / ( g_nframes * 2 * g_nchannels );
	
	// done analyzing, delete decoder
	delete( decoder );
	
	// write data
	fprintf( fp, "%s;", mp3filename );
	fprintf( fp, "%i;", g_nframes );
	fprintf( fp, "%i;", n_bad_first );
	fprintf( fp, "%i;", bad_code );
	for ( i = 0; i < 16; i++ )
		fprintf( fp, "%i;", (int) coef_stats[i] );
	for ( i = 0; i < 15; i++ )
		fprintf( fp, "%i;", (int) coefb_stats[i] );
	for ( i = 0; i < 16; i++ )
		fprintf( fp, "%i;", (int) scf_stats[i] );
	for ( i = 0; i < 32; i++ )
		fprintf( fp, "%i;", (int) bv_tbl_stats[i] );
	for ( i = 0; i < 2; i++ )
		fprintf( fp, "%i;", (int) sv_tbl_stats[i] );
	for ( i = 0; i < 4; i++ )
		fprintf( fp, "%i;", (int) blt_stats[i] );
	fprintf( fp, "%i;", (int) n_bad_bv_bound );
	fprintf( fp, "%i;", bv_bound_max_dist );
	fprintf( fp, "%i;", bv_bound_avrg );
	fprintf( fp, "%i;", (int) n_bad_sv_bound );
	fprintf( fp, "%i;", sv_bound_max_dist );
	fprintf( fp, "%i;", sv_bound_avrg );
	fprintf( fp, "%i", r2bvs_avrg );
	fprintf( fp, "\n" );
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	Make a PGM of header/sideinfo data
	----------------------------------------------- */
INTERN bool visualize_headers( void )
{
	static const int img_width = 1280; // must be divisible by 2
	static const bool inc_all = false;
	static thread_local unsigned char* line = (unsigned char*) calloc ( img_width, sizeof( char ) );
	mp3Frame* frame;
	mp3Frame* frame0 = firstframe;
	mp3Frame* frame1 = firstframe;
	int i;
	
	FILE* fp;
	char* fn;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "headers.pgm" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );
	
	// write PGM header
	fprintf( fp, "P5\n" );
	fprintf( fp, "# created by %s v%i.%i%s (%s) by %s\n",
		appname, appversion / 10, appversion % 10, subversion, versiondate, author );
	fprintf( fp, "%i %i\n", img_width, ( ((g_nframes*2) + img_width - 1)  / img_width ) * ( inc_all ? 60 : 40 ) );
	fprintf( fp, "255\n" );
	
	do { // write data, line per line, 2x2 pixels for header stuff
		memset ( line, 0x0, img_width );
		frame0 = frame1;
		for ( i = 0; i < (img_width/2); i++ ) {
			frame1 = frame1->next;
			if ( frame1 == NULL ) break;
		}
		i = frame0->n;
		
		if ( inc_all ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 ] = frame->mpeg << 6;
				line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
			} // 2 (MPEG)
			fwrite( line, 1, img_width, fp );
			fwrite( line, 1, img_width, fp );
		} // -2
		
		if ( inc_all ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 ] = frame->layer << 6;
				line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
			} // 4 (LAYER)
			fwrite( line, 1, img_width, fp );
			fwrite( line, 1, img_width, fp );
		} // -4
		
		if ( inc_all ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 ] = frame->channels << 6;
				line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
			} // 6 (CHANNELS)
			fwrite( line, 1, img_width, fp );
			fwrite( line, 1, img_width, fp );
		} // -6
		
		if ( inc_all ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 ] = frame->samples << 6;
				line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
			} // 8 (SAMPLES)
			fwrite( line, 1, img_width, fp );
			fwrite( line, 1, img_width, fp );
		} // -8
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 ] = frame->bits << 4;
			line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
		} // 10 (BITRATE)
		fwrite( line, 1, img_width, fp );
		fwrite( line, 1, img_width, fp );
		
		if ( inc_all ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 ] = ( frame->protection == 0 ) ? 0 : 255;
				line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
			} // 12 (PROTECTION)
			fwrite( line, 1, img_width, fp );
			fwrite( line, 1, img_width, fp );
		} // -10
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 ] = ( frame->padding == 0 ) ? 0 : 255;
			line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
		} // 14 (PADDING)
		fwrite( line, 1, img_width, fp );
		fwrite( line, 1, img_width, fp );
		
		if ( inc_all ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 ] = ( frame->privbit == 0 ) ? 0 : 255;
				line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
			} // 16 (PRIVATE)
			fwrite( line, 1, img_width, fp );
			fwrite( line, 1, img_width, fp );
		} // -12
		
		if ( inc_all ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 ] = ( frame->copyright == 0 ) ? 0 : 255;
				line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
			} // 18 (COPYRIGHT)
			fwrite( line, 1, img_width, fp );
			fwrite( line, 1, img_width, fp );
		} // -14
		
		if ( inc_all ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 ] = ( frame->original == 0 ) ? 0 : 255;
				line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
			} // 20 (ORIGINAL)
			fwrite( line, 1, img_width, fp );
			fwrite( line, 1, img_width, fp );
		} // -16
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 ] = ( frame->stereo_ms == 0 ) ? 0 : 255;
			line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
		} // 22 (MS STEREO)
		fwrite( line, 1, img_width, fp );
		fwrite( line, 1, img_width, fp );
		
		if ( inc_all ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 ] = ( frame->stereo_int == 0 ) ? 0 : 255;
				line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
			} // 24 (INT STEREO)
			fwrite( line, 1, img_width, fp );
			fwrite( line, 1, img_width, fp );
		} // -18
		
		if ( inc_all ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 ] = frame->emphasis << 6;
				line[ (frame->n-i) * 2 + 1 ] = line[ (frame->n-i) * 2 ];
			} // 26 (EMPHASIS)
			fwrite( line, 1, img_width, fp );
			fwrite( line, 1, img_width, fp );
		} // -20
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->main_data_bit >> 4;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->main_data_bit >> 4;
		}
		fwrite( line, 1, img_width, fp ); // 27 (MAIN DATA BIT)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->main_data_bit >> 4;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->main_data_bit >> 4;
			}
		}
		fwrite( line, 1, img_width, fp ); // 28 (MAIN DATA BIT)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->big_val_pairs >> 1;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->big_val_pairs >> 1;
		}
		fwrite( line, 1, img_width, fp ); // 29 (BIG VALUES)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->big_val_pairs >> 1;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->big_val_pairs >> 1;
			}
		}
		fwrite( line, 1, img_width, fp ); // 30 (BIG VALUES)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->share << 4;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->share << 4;
		}
		fwrite( line, 1, img_width, fp ); // 31 (SHARING)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->share << 4;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->share << 4;
			}
		}
		fwrite( line, 1, img_width, fp ); // 32 (SHARING)
			
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->global_gain;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->global_gain;
		}
		fwrite( line, 1, img_width, fp ); // 33 (GLOBAL GAIN)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->global_gain;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->global_gain;
			}
		}
		fwrite( line, 1, img_width, fp ); // 34 (GLOBAL GAIN)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->slength << 4;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->slength << 4;
		}
		fwrite( line, 1, img_width, fp ); // 35 (SLENGTH)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->slength << 4;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->slength << 4;
			}
		}
		fwrite( line, 1, img_width, fp ); // 36 (SLENGTH)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->block_type << 6;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->block_type << 6;
		}
		fwrite( line, 1, img_width, fp ); // 37 (BLOCK TYPE)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->block_type << 6;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->block_type << 6;
			}
		}
		fwrite( line, 1, img_width, fp ); // 38 (BLOCK TYPE)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->preemphasis * 255;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->preemphasis * 255;
		}
		fwrite( line, 1, img_width, fp ); // 39 (PREEMPHASIS)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->preemphasis * 255;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->preemphasis * 255;
			}
		}
		fwrite( line, 1, img_width, fp ); // 40 (PREEMPHASIS)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->coarse_scalefactors * 255;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->coarse_scalefactors * 255;
		}
		fwrite( line, 1, img_width, fp ); // 41 (COARSE)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->coarse_scalefactors * 255;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->coarse_scalefactors * 255;
			}
		}
		fwrite( line, 1, img_width, fp ); // 42 (COARSE)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->region_table[0] << 3;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->region_table[0] << 3;
		}
		fwrite( line, 1, img_width, fp ); // 43 (R0 TABLE)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->region_table[0] << 3;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->region_table[0] << 3;
			}
		}
		fwrite( line, 1, img_width, fp ); // 44 (R0 TABLE)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->region_table[1] << 3;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->region_table[1] << 3;
		}
		fwrite( line, 1, img_width, fp ); // 45 (R1 TABLE)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->region_table[1] << 3;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->region_table[1] << 3;
			}
		}
		fwrite( line, 1, img_width, fp ); // 46 (R1 TABLE)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->region_table[2] << 3;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->region_table[2] << 3;
		}
		fwrite( line, 1, img_width, fp ); // 47 (R2 TABLE)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->region_table[2] << 3;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->region_table[2] << 3;
			}
		}
		fwrite( line, 1, img_width, fp ); // 48 (R2 TABLE)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->select_htabB * 255;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->select_htabB * 255;
		}
		fwrite( line, 1, img_width, fp ); // 49 (R4 TABLE)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->select_htabB * 255;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->select_htabB * 255;
			}
		}
		fwrite( line, 1, img_width, fp ); // 50 (R4 TABLE)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->region0_size << 4;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->region0_size << 4;
		}
		fwrite( line, 1, img_width, fp ); // 51 (R0 SIZE)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->region0_size << 4;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->region0_size << 4;
			}
		}
		fwrite( line, 1, img_width, fp ); // 52 (R0 SIZE)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->region1_size << 4;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->region1_size << 4;
		}
		fwrite( line, 1, img_width, fp ); // 53 (R1 SIZE)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->region1_size << 4;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->region1_size << 4;
			}
		}
		fwrite( line, 1, img_width, fp ); // 54 (R1 SIZE)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->sb_gain[0] << 5;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->sb_gain[0] << 5;
		}
		fwrite( line, 1, img_width, fp ); // 55 (SB0 GAIN)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->sb_gain[0] << 5;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->sb_gain[0] << 5;
			}
		}
		fwrite( line, 1, img_width, fp ); // 56 (SB0 GAIN)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->sb_gain[1] << 5;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->sb_gain[1] << 5;
		}
		fwrite( line, 1, img_width, fp ); // 57 (SB1 GAIN)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->sb_gain[1] << 5;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->sb_gain[1] << 5;
			}
		}
		fwrite( line, 1, img_width, fp ); // 58 (SB1 GAIN)
		
		for ( frame = frame0; frame != frame1; frame = frame->next ) {
			line[ (frame->n-i) * 2 + 0 ] = frame->granules[0][0]->sb_gain[2] << 5;
			line[ (frame->n-i) * 2 + 1 ] = frame->granules[0][1]->sb_gain[2] << 5;
		}
		fwrite( line, 1, img_width, fp ); // 59 (SB2 GAIN)
		if ( g_nchannels == 2 ) {
			for ( frame = frame0; frame != frame1; frame = frame->next ) {
				line[ (frame->n-i) * 2 + 0 ] = frame->granules[1][0]->sb_gain[2] << 5;
				line[ (frame->n-i) * 2 + 1 ] = frame->granules[1][1]->sb_gain[2] << 5;
			}
		}
		fwrite( line, 1, img_width, fp ); // 60 (SB2 GAIN)
	} while ( frame1 != NULL );
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	Make a PGM of coefs/scalefactors
	----------------------------------------------- */
INTERN bool visualize_decoded_data( void )
{
	static const int nfs = 2;
	static const int nfc = 9;
	static const unsigned char v_types[4][2] = {
		{ 0xFF, 0xFF }, { 0xFF, 0x00 },
		{ 0x00, 0x00 }, { 0x00, 0xFF }
	};
	static const unsigned char v_unk  = 0x7F;
	static const unsigned char v_zero = 0x00;
	
	FILE* fp[nfs+nfc];
	const char* ext[nfs+nfc] = {
		"scale.unt.pgm",
		"scale.nrm.pgm",
		"coefs.f15.pgm",
		"coefs.sgn.pgm",
		"coefs.l15.pgm",
		"coefs.len.pgm",
		"coefs.nzz.pgm",
		"coefs.reg.pgm",
		"coefs.tbl.pgm",
		"coefs.bsb.pgm",
		"coefs.nnz.pgm"
	};
	char* fn;
	
	int total_cf, total_sc;
	int width_cf, width_sc;
	int height_cf, height_sc;
	mp3Frame* frame;
	granuleInfo* granule;
	granuleData*** frame_data;
	huffman_reader* decoder;
	unsigned char* data_cf[nfc];
	unsigned char* data_sc[nfs];
	unsigned char* temp_cf;
	unsigned char* temp_sc;	
	int temp_reg[16];
	int tmp;
	int gr, ch;
	int blt;
	int n_sc, n_cf;
	int i, r;
	
	
	// open files
	for ( i = 0; i < (nfs+nfc); i++ ) {
		fn = create_filename( filelist[ file_no ], ext[i] );
		// open file for output
		fp[i] = fopen( fn, "wb" );
		if ( fp[i] == NULL ) {
			for ( i--; i >= 0; i-- ) fclose( fp[i] );
			snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
			free( fn );
			errorlevel = 2;
			return false;
		}
		free( fn );
	}
	
	// decide widths for both (try reaching 4:3 ratio)
	total_cf = g_nframes * g_nchannels * 2 * ( 576 + 2 + 2 + 2 );
	total_sc = g_nframes * g_nchannels * 2 * (  32 + 2 + 2 + 2 );
	height_cf = ( 576 + 2 + 2 + 2 ) * 2;
	for ( i = 2; ( 582 * 582 * i * i ) <= total_cf; i += 2 )
		height_cf = ( 576 + 2 + 2 + 2 ) * i;
	height_sc = (  32 + 2 + 2 + 2 ) * 2;
	for ( i = 2; (  38 *  38 * i * i ) <= total_sc; i += 2 )
		height_sc = (  32 + 2 + 2 + 2 ) * i;
	width_cf = ( total_cf + height_cf - 1 ) / height_cf;
	width_sc = ( total_sc + height_sc - 1 ) / height_sc;
	if ( width_cf % 2 == 1 ) width_cf++;
	if ( width_sc % 2 == 1 ) width_sc++;
	
	// write pgm headers
	for ( i = 0; i < (nfs+nfc); i++ ) {
		fprintf( fp[i], "P5\n" );
		fprintf( fp[i], "# created by %s v%i.%i%s (%s) by %s\n",
			appname, appversion / 10, appversion % 10, subversion, versiondate, author );
		fprintf( fp[i], "%i %i\n", (i>=nfs)?width_cf:width_sc, (i>=nfs)?height_cf:height_sc );
		fprintf( fp[i], "255\n" );
	}
	
	// alloc memory
	temp_sc = ( unsigned char* ) calloc( 32, sizeof( char ) );
	temp_cf = ( unsigned char* ) calloc( 576, sizeof( char ) );
	for ( i = 0; i < nfs; i++ ) {
		data_sc[i] = ( unsigned char* ) calloc( width_sc * g_nchannels * 38, sizeof( char ) );
		if ( data_sc[i] == NULL ) {
			free( temp_sc ); free( temp_cf );
			for ( i = (nfs+nfc) - 1; i >= 0; i-- ) fclose( fp[i] );
			snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
			errorlevel = 2;
			return false;
		}
	}
	for ( i = 0; i < nfc; i++ ) {
		data_cf[i] = ( unsigned char* ) calloc( width_cf * g_nchannels * 582, sizeof( char ) );
		if ( data_cf[i] == NULL ) {
			free( temp_sc ); free( temp_cf );
			for ( i = (nfs+nfc) - 1; i >= 0; i-- ) fclose( fp[i] );
			snprintf( errormessage, MSG_SIZE, MEM_ERRMSG );
			errorlevel = 2;
			return false;
		}
	}
	
	
	// init decoder	
	decoder = new huffman_reader( main_data, main_data_size );
	
	// main processing loop
	for ( n_sc = 0, n_cf = 0, frame = firstframe; frame != NULL; frame = frame->next ) {
		frame_data = ( frame->n >= n_bad_first ) ? mp3_decode_frame( decoder, frame ) : NULL;
		for ( gr = 0; gr < 2; gr++, n_sc++, n_cf++ ) {
			if ( frame_data != NULL ) for ( ch = 0; ch < g_nchannels; ch++ ) {
				granule = frame->granules[ch][gr];
				// block type
				blt = frame->granules[ch][gr]->block_type;
				
				// scalefactors visualization
				for ( i = 0; i < nfs; i++ ) {
					switch ( i ) {
						case 0: // untouched scalefactors
							for ( r = ( blt == SHORT_BLOCK ) ? 31 : 20; r >= 0; r-- )
								temp_sc[r] = frame_data[ch][gr]->scalefactors[ r ] * 16;
							break;
						case 1: // normalized scalefactors
							for ( r = ( blt == SHORT_BLOCK ) ? 31 : 20; r >= 0; r-- ) {
								temp_sc[r] = frame_data[ch][gr]->scalefactors[ r ];
								if ( granule->preemphasis && ( granule->block_type != SHORT_BLOCK ) )
									temp_sc[r] += preemphasis_table[ i ];
								if ( granule->coarse_scalefactors )
									temp_sc[r] <<= 1;
								temp_sc[r] *= 6;
							}
							break;
					}
					data_sc[i][ ( (  0 + (ch*40) ) * width_sc ) + n_sc ] = v_types[blt][0];
					data_sc[i][ ( (  1 + (ch*40) ) * width_sc ) + n_sc ] = v_types[blt][1];
					data_sc[i][ ( ( 34 + (ch*40) ) * width_sc ) + n_sc ] = v_types[blt][0];
					data_sc[i][ ( ( 35 + (ch*40) ) * width_sc ) + n_sc ] = v_types[blt][1];
					if ( ch == 0 ) for ( r = ( blt == SHORT_BLOCK ) ? 31 : 20; r >= 0; r-- )
						data_sc[i][ ( ( 2 + 31 - r ) * width_sc ) + n_sc ] = temp_sc[r];
					else for ( r = ( blt == SHORT_BLOCK ) ? 31 : 20; r >= 0; r-- )
						data_sc[i][ ( ( 40 + 2 + r ) * width_sc ) + n_sc ] = temp_sc[r];
					if ( g_nchannels == 1 ) {
						data_sc[i][ ( 36 * width_sc ) + n_sc ] = granule->global_gain;
						data_sc[i][ ( 37 * width_sc ) + n_sc ] = granule->global_gain;
					} else if ( ch == 0 ) {
						data_sc[i][ ( 36 * width_sc ) + n_sc ] = granule->global_gain;
						data_sc[i][ ( 37 * width_sc ) + n_sc ] = frame->stereo_ms * 255;
					} else {
						data_sc[i][ ( 38 * width_sc ) + n_sc ] = frame->stereo_ms * 255;
						data_sc[i][ ( 39 * width_sc ) + n_sc ] = granule->global_gain;
					}
				}				
				
				
				// coefficients visualization
				for ( i = 0; i < nfc; i++ ) {
					switch ( i ) {
						case 0: // fixed 4 bit part
							for ( r = 0; r < 576; r++ )	temp_cf[r] =
								( frame_data[ch][gr]->coefficients[ r ] >  15 ) ? 15 * 16:
								( frame_data[ch][gr]->coefficients[ r ] < -15 ) ? 15 * 16:
								( frame_data[ch][gr]->coefficients[ r ] >=  0 ) ? 
									 frame_data[ch][gr]->coefficients[ r ] * 16:
									-frame_data[ch][gr]->coefficients[ r ] * 16;
							break;
						case 1: // sign
							for ( r = 0; r < 576; r++ ) temp_cf[r] = 
								( frame_data[ch][gr]->coefficients[ r ] == 0 ) ? 128 :
									( frame_data[ch][gr]->coefficients[ r ] > 0 ) ? 255 : 0;
							break;
						case 2: // 4 bit truncated bitlength
							for ( r = 0; r < 576; r++ ) temp_cf[r] =
								( frame_data[ch][gr]->coefficients[ r ] > 15 ) ?
									BITLEN8192P( ( frame_data[ch][gr]->coefficients[ r ] - 15 ) ) * 18 :
								( frame_data[ch][gr]->coefficients[ r ] < -15 ) ?
									BITLEN8192P( ( -frame_data[ch][gr]->coefficients[ r ] - 15 ) ) * 18 : 0;
							break;
						case 3: // full bitlength
							for ( r = 0; r < 576; r++ ) temp_cf[r] = 
								BITLEN8224N( frame_data[ch][gr]->coefficients[ r ] ) * 19;
							break;
						case 4: // zeroes - non-zeroes
							for ( r = 0; r < 576; r++ ) temp_cf[r] = 
								( frame_data[ch][gr]->coefficients[ r ] == 0 ) ? 0 : 128;
							break;
						case 5: // regions
							for ( r = 0; r < 576; r++ ) temp_cf[r] = 
								( r < granule->region_bound[0] ) ? 255 :
								( r < granule->region_bound[1] ) ? 191 :
								( r < granule->region_bound[2] ) ? 131 :
								( r < granule->sv_bound ) ? 63 : 0;
							break;
						case 6: // tables
							for ( r = 0; r < 576; r++ ) temp_cf[r] =
								( r < granule->region_bound[0] ) ? granule->region_table[0] * 7 + 21 :
								( r < granule->region_bound[1] ) ? granule->region_table[1] * 7 + 21 :
								( r < granule->region_bound[2] ) ? granule->region_table[2] * 7 + 21 :
								( r < granule->sv_bound ) ? granule->select_htabB * 7 + 7 : 0;
							break;
						case 7: // only bv/sv region diffs
							for ( r = 0; r < 576; r++ ) temp_cf[r] = 
								( r < granule->sv_bound - granule->region_bound[2] ) ? 255 : 0;
							break;
						case 8: // numbers of non-zeroes
							memset( temp_reg, 0x00, 16 * sizeof(int) );
							for ( tmp = 0; tmp < 16; tmp++ ) for ( r = 0; r < 576; r++ )
								if ( ABS( frame_data[ch][gr]->coefficients[ r ] ) >= tmp ) temp_reg[tmp]++;
							for ( tmp = 0, r = 0; tmp < 16; tmp++ ) for ( ; r < temp_reg[15-tmp]; r++ )
								temp_cf[r] = (15-tmp) * 16;
							break;
					}					
					data_cf[i][ ( (   0 + (ch*584) ) * width_cf ) + n_cf ] = v_types[blt][0];
					data_cf[i][ ( (   1 + (ch*584) ) * width_cf ) + n_cf ] = v_types[blt][1];
					data_cf[i][ ( ( 578 + (ch*584) ) * width_cf ) + n_cf ] = v_types[blt][0];
					data_cf[i][ ( ( 579 + (ch*584) ) * width_cf ) + n_cf ] = v_types[blt][1];
					if ( ch == 0 ) for ( r = 576 - 1; r >= 0; r-- )
						data_cf[i][ ( ( 2 + 575 - r ) * width_cf ) + n_cf ] = temp_cf[ r ];
					else for ( r = 0; r < 576; r++ )
						data_cf[i][ ( ( 584 + 2 + r ) * width_cf ) + n_cf ] = temp_cf[ r ];
					if ( g_nchannels == 1 ) {
						data_cf[i][ ( 580 * width_cf ) + n_cf ] = granule->global_gain;
						data_cf[i][ ( 581 * width_cf ) + n_cf ] = granule->global_gain;
					} else if ( ch == 0 ) {
						data_cf[i][ ( 580 * width_cf ) + n_cf ] = granule->global_gain;
						data_cf[i][ ( 581 * width_cf ) + n_cf ] = frame->stereo_ms * 255;
					} else {
						data_cf[i][ ( 582 * width_cf ) + n_cf ] = frame->stereo_ms * 255;
						data_cf[i][ ( 583 * width_cf ) + n_cf ] = granule->global_gain;
					}
				}
			} else {
				// no scalefactors
				for ( i = 0; i < nfs; i++ ) {
					data_sc[i][ (  0 * width_sc ) + n_sc ] = v_unk;
					data_sc[i][ (  1 * width_sc ) + n_sc ] = v_unk;
					data_sc[i][ ( 34 * width_sc ) + n_sc ] = v_unk;
					data_sc[i][ ( 35 * width_sc ) + n_sc ] = v_unk;
					if ( g_nchannels == 2 ) {
						data_sc[i][ ( 36 * width_sc ) + n_sc ] = v_unk;
						data_sc[i][ ( 37 * width_sc ) + n_sc ] = v_unk;
						data_sc[i][ ( 70 * width_sc ) + n_sc ] = v_unk;
						data_sc[i][ ( 71 * width_sc ) + n_sc ] = v_unk;
					}
				}
				// no coefficients
				for ( i = 0; i < nfc; i++ ) {
					data_cf[i][ (    0 * width_cf ) + n_cf ] = v_unk;
					data_cf[i][ (    1 * width_cf ) + n_cf ] = v_unk;
					data_cf[i][ (  578 * width_cf ) + n_cf ] = v_unk;
					data_cf[i][ (  579 * width_cf ) + n_cf ] = v_unk;
					if ( g_nchannels == 2 ) {
						data_cf[i][ (  580 * width_cf ) + n_cf ] = v_unk;
						data_cf[i][ (  581 * width_cf ) + n_cf ] = v_unk;
						data_cf[i][ ( 1158 * width_cf ) + n_cf ] = v_unk;
						data_cf[i][ ( 1159 * width_cf ) + n_cf ] = v_unk;
					}
				}
			}
		}
		
		if ( n_sc >= width_sc ) { // flush scfs
			for ( i = 0; i < nfs; i++ ) {
				fwrite( data_sc[i], 1, width_sc * g_nchannels * 38, fp[i] );
				memset( data_sc[i], v_zero, width_sc * g_nchannels * 38 );
			}
			n_sc = 0;
		}
		
		if ( n_cf >= width_cf ) { // flush coefs
			for ( i = 0; i < nfc; i++ ) {
				fwrite( data_cf[i], 1, width_cf * g_nchannels * 582, fp[nfs+i] );
				memset( data_cf[i], v_zero, width_cf * g_nchannels * 582 );
			}
			n_cf = 0;
		}
	}
	if ( n_sc > 0 ) { // flush scfs
		for ( i = 0; i < nfs; i++ )
			fwrite( data_sc[i], 1, width_sc * g_nchannels * 38, fp[i] );
		n_sc = 0;
	}
	if ( n_cf > 0 ) { // flush coefs
		for ( i = 0; i < nfc; i++ )
			fwrite( data_cf[i], 1, width_cf * g_nchannels * 582, fp[nfs+i] );
		n_cf = 0;
	}
	
	
	// close file pointers / dealloc memory
	free( temp_cf );
	free( temp_sc );
	for ( i = 0; i < (nfs+nfc); i++ ) fclose( fp[i] );
	for ( i = 0; i < nfs; i++ ) free( data_sc[i] );
	for ( i = 0; i < nfc; i++ ) free( data_cf[i] );
	
	
	// delete decoder
	delete( decoder );
	
	
	return true;
}


/* -----------------------------------------------
	dump main data size
	----------------------------------------------- */
INTERN bool dump_main_sizes( void )
{	
	FILE* fp;
	char* fn;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "main_size.u2b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );
	
	// dump data & close file
	for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
		fwrite( &(frame->main_size), 2, 1, fp );
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump aux data size
	----------------------------------------------- */
INTERN bool dump_aux_sizes( void )
{
	FILE* fp;
	char* fn;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "aux_size.u2b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );
	
	// dump data & close file
	for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
		fwrite( &(frame->aux_size), 2, 1, fp );
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump bitrates
	----------------------------------------------- */
INTERN bool dump_bitrates( void )
{
	FILE* fp;
	char* fn;
	
	
	// only output for vbr files
	if ( i_bitrate != -1 ) return true;
	
	// create filename
	fn = create_filename( filelist[ file_no ], "bitrate.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );
	
	// dump data & close file
	for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
		fwrite( &(frame->bits), 1, 1, fp );
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump stereo ms bit
	----------------------------------------------- */
INTERN bool dump_stereo_ms( void )
{
	FILE* fp;
	char* fn;
	
	
	// check if there is any ms stereo
	if ( i_stereo_ms == 0 ) return true;
	
	// create filename
	fn = create_filename( filelist[ file_no ], "stereo_ms.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );
	
	// dump data & close file
	for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
		fwrite( &(frame->stereo_ms), 1, 1, fp );
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump padding bit
	----------------------------------------------- */
INTERN bool dump_padding( void )
{
	FILE* fp;
	char* fn;
	
	
	// check if there is any padding
	if ( i_padding == 0 ) return true;
	
	// create filename
	fn = create_filename( filelist[ file_no ], "padding.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );
	
	// dump data & close file
	for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
		fwrite( &(frame->padding), 1, 1, fp );
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump main data bit counts
	----------------------------------------------- */
INTERN bool dump_main_data_bits( void )
{
	FILE* fp;
	char* fn;
	int ch, gr;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "main_bits.u2b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );	
	
	// dump data
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
			for ( gr = 0; gr < 2; gr++ )
				fwrite( &(frame->granules[ch][gr]->main_data_bit), 2, 1, fp );
		
	}
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump big values
	----------------------------------------------- */
INTERN bool dump_big_value_ns( void )
{
	FILE* fp;
	char* fn;
	int ch, gr;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "big_value_pairs.u2b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );	
	
	// dump data
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
			for ( gr = 0; gr < 2; gr++ )
				fwrite( &(frame->granules[ch][gr]->big_val_pairs), 2, 1, fp );
		
	}
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump global gains
	----------------------------------------------- */
INTERN bool dump_global_gain( void )
{
	FILE* fp;
	char* fn;
	int ch, gr;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "global_gain.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );	
	
	// dump data
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
			for ( gr = 0; gr < 2; gr++ )
				fwrite( &(frame->granules[ch][gr]->global_gain), 1, 1, fp );
		
	}
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump slength
	----------------------------------------------- */
INTERN bool dump_slength( void )
{
	FILE* fp;
	char* fn;
	int ch, gr;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "slength.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );	
	
	// dump data
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
			for ( gr = 0; gr < 2; gr++ )
				fwrite( &(frame->granules[ch][gr]->slength), 1, 1, fp );
		
	}
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump block types
	----------------------------------------------- */
INTERN bool dump_block_types( void )
{
	FILE* fp;
	char* fn;
	int ch, gr;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "block_types.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );	
	
	// dump data
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
			for ( gr = 0; gr < 2; gr++ )
				fwrite( &(frame->granules[ch][gr]->block_type), 1, 1, fp );
		
	}
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump sharing bits
	----------------------------------------------- */
INTERN bool dump_sharing( void )
{
	FILE* fp;
	char* fn;
	int ch;
	int c;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "sharing.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );	
	
	// dump data
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next ) {
			c = BITN( frame->granules[ch][0]->share, 3 ); fwrite( &c, 1, 1, fp );
			c = BITN( frame->granules[ch][0]->share, 2 ); fwrite( &c, 1, 1, fp );
			c = BITN( frame->granules[ch][0]->share, 1 ); fwrite( &c, 1, 1, fp );
			c = BITN( frame->granules[ch][0]->share, 0 ); fwrite( &c, 1, 1, fp );
		}		
	}
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump preemphasis setting
	----------------------------------------------- */
INTERN bool dump_preemphasis( void )
{
	FILE* fp;
	char* fn;
	int ch, gr;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "preemphasis.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );	
	
	// dump data
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
			for ( gr = 0; gr < 2; gr++ )
				fwrite( &(frame->granules[ch][gr]->preemphasis), 1, 1, fp );
		
	}
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump coarse setting
	----------------------------------------------- */
INTERN bool dump_coarse( void )
{
	FILE* fp;
	char* fn;
	int ch, gr;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "coarse_scalefactors.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );	
	
	// dump data
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
			for ( gr = 0; gr < 2; gr++ )
				fwrite( &(frame->granules[ch][gr]->coarse_scalefactors), 1, 1, fp );
		
	}
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump huffman table selections
	----------------------------------------------- */
INTERN bool dump_htable_sel( void )
{
	FILE* fp;
	char* fn;
	int ch, gr;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "htable_selection.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );	
	
	// dump data
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( int i = 0; i < 3; i++ ) {
			for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
				for ( gr = 0; gr < 2; gr++ )
					fwrite( &(frame->granules[ch][gr]->region_table[i]), 1, 1, fp );
		}
		for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
			for ( gr = 0; gr < 2; gr++ )
				fwrite( &(frame->granules[ch][gr]->select_htabB), 1, 1, fp );
	}
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump region sizes
	----------------------------------------------- */
INTERN bool dump_region_sizes( void )
{
	FILE* fp;
	char* fn;
	int ch, gr;
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "region_sizes.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );	
	
	// dump data
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
			for ( gr = 0; gr < 2; gr++ )
				fwrite( &(frame->granules[ch][gr]->region0_size), 1, 1, fp );
		for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next )
			for ( gr = 0; gr < 2; gr++ )
				fwrite( &(frame->granules[ch][gr]->region1_size), 1, 1, fp );		
	}
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump subblock gains
	----------------------------------------------- */
INTERN bool dump_subblock_gains( void )
{
	FILE* fp;
	char* fn;
	int ch, gr;
	
	
	// check if there is any subblock gain
	if ( i_sbgain == 0 ) return true;
	
	// create filename
	fn = create_filename( filelist[ file_no ], "subblock_gain.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );	
	
	// dump data
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next ) {
			for ( gr = 0; gr < 2; gr++ ) if ( frame->granules[ch][gr]->block_type == SHORT_BLOCK ) {
				fwrite( &(frame->granules[ch][gr]->sb_gain[0]), 1, 1, fp );
				fwrite( &(frame->granules[ch][gr]->sb_gain[1]), 1, 1, fp );
				fwrite( &(frame->granules[ch][gr]->sb_gain[2]), 1, 1, fp );
			}
		}
	}
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump data files
	----------------------------------------------- */
INTERN bool dump_data_files( void )
{
	FILE* fp_aux;
	FILE* fp_huf;
	char* fn;
	
	
	// --- simple stuff ---
	if ( main_data_size > 0 )
		if ( !write_file( filelist[ file_no ], "main_data.s1b", main_data, 1, main_data_size ) )
			return false;
	if ( data_before_size > 0 )
		if ( !write_file( filelist[ file_no ], "data_before.s1b", data_before, 1, data_before_size ) )
			return false;
	if ( data_after_size > 0 )
		if ( !write_file( filelist[ file_no ], "data_after.s1b", data_after, 1, data_after_size ) )
			return false;
	
	// --- huffman and aux data files ---
	
	// open huffman file
	fn = create_filename( filelist[ file_no ], "huff_data.s1b" );
	fp_huf = fopen( fn, "wb" );
	free( fn );	
	// open aux file
	fn = create_filename( filelist[ file_no ], "aux_data.s1b" );
	fp_aux = fopen( fn, "wb" );
	// check for problems
	if ( ( fp_huf == NULL ) || ( fp_aux == NULL ) ) {
		if ( fp_huf != NULL ) fclose( fp_huf );
		if ( fp_aux != NULL ) fclose( fp_aux );
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		free( fn );
		errorlevel = 2;
		return false;
	}
	free( fn );
	
	// dump data
	for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next ) {
		if ( frame->n < n_bad_first ) continue;
		fwrite( main_data + frame->main_index, 1, frame->main_size, fp_huf );
		fwrite( main_data + frame->main_index + frame->main_size, 1, frame->aux_size, fp_aux );
	}
	
	// close files
	fclose( fp_huf );
	fclose( fp_aux );
	
	
	return true;
}


/* -----------------------------------------------
	dump global gain context
	----------------------------------------------- */
INTERN bool dump_gg_ctx( void )
{	
	FILE* fp;
	char* fn;
	
	
	// build the context (even if it is already built)
	pmp_build_context();
	
	
	// create filename
	fn = create_filename( filelist[ file_no ], "gg_ctx.u1b" );
	
	// open file for output
	fp = fopen( fn, "wb" );
	if ( fp == NULL ){
		snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
		errorlevel = 2;
		return false;
	}
	free( fn );	
	
	// dump data
	fwrite( gg_context[0], 1, 2 * g_nframes, fp );
	if ( g_nchannels == 2 )
		fwrite( gg_context[1], 1, 2 * g_nframes, fp );
	
	// close file
	fclose( fp );
	
	
	return true;
}


/* -----------------------------------------------
	dump coefficients
	----------------------------------------------- */
INTERN bool dump_decoded_data( void )
{
	static const unsigned char zeroes[32] = { 0 };
	FILE* fp_cf[2];
	FILE* fp_sc[2];
	const char* ext_cf[2] = { "coefs.ch0.s2b", "coefs.ch1.s2b" };
	const char* ext_sc[2] = { "scale.ch0.u1b", "scale.ch1.u1b" };
	char* fn;
	int ch, gr;
	
	granuleData*** frame_data;
	huffman_reader* decoder;
	
	
	// open 4 files (max)
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		// coeficients file
		// create filename
		fn = create_filename( filelist[ file_no ], ext_cf[ch] );
		// open file for output
		fp_cf[ch] = fopen( fn, "wb" );
		free( fn );
		// scalefactors file
		// create filename
		fn = create_filename( filelist[ file_no ], ext_sc[ch] );
		// open file for output
		fp_sc[ch] = fopen( fn, "wb" );
		// error checking and file pointer rollback
		if ( ( fp_cf[ch] == NULL ) || ( fp_sc[ch] == NULL ) ) {
			for ( ; ch >= 0; ch-- ) { 
				if ( fp_cf[ch] != NULL ) fclose( fp_cf[ch] );
				if ( fp_sc[ch] != NULL ) fclose( fp_sc[ch] );
			}
			snprintf( errormessage, MSG_SIZE, FWR_ERRMSG, fn );
			free( fn );
			errorlevel = 2;
			return false;
		}
		free( fn );
	}
	
	// init decoder	
	decoder = new huffman_reader( main_data, main_data_size );
	
	// dump data
	for ( mp3Frame* frame = firstframe; frame != NULL; frame = frame->next ) {
		// skip bad first frames
		if ( frame->n < n_bad_first ) continue;
		frame_data = mp3_decode_frame( decoder, frame );
		if ( frame_data == NULL ) continue;
		for ( gr = 0; gr < 2; gr++ ) {
			for ( ch = 0; ch < g_nchannels; ch++ ) {
				fwrite( frame_data[ch][gr]->coefficients, sizeof( short ), 576, fp_cf[ch] );
				if ( frame->granules[ch][gr]->block_type != SHORT_BLOCK ) {
					fwrite( frame_data[ch][gr]->scalefactors, sizeof( char ), 21, fp_sc[ch] );
					fwrite( zeroes, sizeof( char ), 11, fp_sc[ch] );
				} else fwrite( frame_data[ch][gr]->scalefactors, sizeof( char ), 32, fp_sc[ch] );
			}
		}
	}
	
	// close files
	for ( ch = 0; ch < g_nchannels; ch++ ) {
		fclose( fp_cf[ch] );
		fclose( fp_sc[ch] );
	}
	
	// delete decoder
	delete( decoder );
	
	
	return true;
}
#endif

/* ----------------------- End of developers functions -------------------------- */

/* ----------------------- End of file -------------------------- */
