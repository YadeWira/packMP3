// defines for coder
#define CODER_USE_BITS		31 // must never be above 31
#define CODER_LIMIT100		( (unsigned int) ( 1 << CODER_USE_BITS ) )
#define CODER_LIMIT025		( ( CODER_LIMIT100 / 4 ) * 1 )
#define CODER_LIMIT050		( ( CODER_LIMIT100 / 4 ) * 2 )
#define CODER_LIMIT075		( ( CODER_LIMIT100 / 4 ) * 3 )
#define CODER_MAXSCALE		CODER_LIMIT025 - 1
#define ESCAPE_SYMBOL		CODER_LIMIT025


// symbol struct, used in arithmetic coding
struct symbol {
    unsigned int low_count;
	unsigned int high_count;
	unsigned int scale;
};

// table struct, used in in statistical models,
// holding all info needed for one context
struct table {
	// counts for each symbol contained in the table
	unsigned short* counts;
	// links to higher order contexts
	struct table** links;
	// link to lower order context
	struct table* lesser;	
	// accumulated counts
	unsigned int scale;
};

// special table struct, used in in model_s,
// holding additional info for a speedier 'totalize_table'
struct table_s {
	// counts for each symbol contained in the table
	unsigned short* counts;
	// links to higher order contexts
	struct table_s** links;
	// link to lower order context
	struct table_s* lesser;
	// speedup info
	unsigned short max_count;
	unsigned short max_symbol;
	// unsigned short esc_prob;
	// Fenwick tree (BIT) mirror of counts[], 1-indexed, size model_max_symbol+1.
	// Lets the no-exclusion fast path (see model_s::totalize_table) get the
	// grand total and any symbol's cumulative count in O(log n) instead of
	// rebuilding the whole totals[] array in O(n) on every call. Kept in
	// sync incrementally in update_model() and rebuilt in rescale_table().
	// Only used/valid on the fast path (no PPM exclusion active yet for the
	// symbol being coded) -- the exclusion ("slow path") stays untouched.
	unsigned int* bit;
	unsigned int bit_total;
	// symbols with counts[i] > 0, for lazily populating the scoreboard only
	// if this context's attempt actually escapes (see convert_int_to_symbol/
	// convert_symbol_to_int) -- avoids the scoreboard scan entirely on the
	// (~99%) common case where the symbol resolves without escaping.
	int* active_symbols;
	int distinct_used;
};


/* -----------------------------------------------
	class for arithmetic coding of data to/from iostream
	----------------------------------------------- */
	
class aricoder
{
	public:
	aricoder( iostream* stream, int iomode );
	~aricoder( void );
	void encode( symbol* s );
	unsigned int decode_count( symbol* s );
	void decode( symbol* s );

	/* ---- corrupt-input detection (decode side) ---------------------------
	   The coded stream carries no length and no end marker in the byte
	   domain, so the decoder cannot tell "the data ended" from "the next
	   symbol is a zero". read_bit() substitutes zero bytes forever once the
	   stream runs out, which a correct decode NEEDS in small amounts (the
	   31-bit prefill in the constructor means every valid decode reads a
	   little past its last encoded byte) but which on a truncated archive
	   becomes an endless supply of fabricated bits. The decoder then keeps
	   producing plausible garbage: it has crashed, hung, and -- worst --
	   completed with exit 0 while writing a file that is NOT the original.

	   The threshold needs SEPARATION, which is two measurements, not one --
	   a point packJPG made after measuring its own coder, where the two
	   populations turned out to be 2 bits apart and a threshold like this
	   one would have missed every dangerous case. Measured here over 10
	   archives (synthetic and real, mono/stereo, CBR/VBR, MPEG-1 and
	   MPEG-2, with and without cover art):

	     valid decodes                       24 to 40 bits fabricated
	     truncations that would otherwise
	     write a wrong file, minimum        120 bits

	   That margin does NOT hold as a separation. Denser sampling -- 60
	   truncation depths per file instead of ten -- found a case that
	   fabricates 32 bits, inside the valid range. The populations do not
	   merely touch, they OVERLAP: no threshold catches that case without
	   also rejecting valid archives.

	   Which matters far less than it sounds, and the reason is worth
	   stating because both this project and packJPG got it backwards first.
	   A corruption guard is not symmetric. A false positive -- refusing a
	   good archive -- is unacceptable. A false negative just leaves the
	   previous behaviour. So the number to protect is the maximum on the
	   VALID side, and 64 clears it while still catching nearly everything
	   above. Overlap kills the guarantee, not the guard.

	   And that valid-side maximum is STRUCTURAL, not a sample high-water
	   mark, which is what makes 64 safe rather than lucky. Measured over 84
	   valid decodes the fabricated-bit count takes exactly three values --
	   24, 32 and 40, i.e. 3, 4 and 5 bytes -- and nothing else, ever. The
	   mechanism is the constructor's prefill: it pulls CODER_USE_BITS = 31
	   bits before decoding starts, and 31 bits spans 4 byte-fetches, plus
	   or minus one depending on where the last real byte falls relative to
	   the byte boundary. 3 to 5 bytes is the whole range the coder can
	   produce, so 40 is a ceiling and not a maximum-so-far. It moves only
	   if CODER_USE_BITS moves.

	   Corroboration from a sibling, which came out stronger than expected:
	   packJPG measured its own distribution and got the SAME three values,
	   24/32/40, from the same CODER_USE_BITS = 31 and the same prefill.
	   Two codecs, two formats, one mechanism.

	   The threshold is 48 -- six bytes, one full byte above the five-byte
	   ceiling -- and it was measured, not picked. An earlier version of this
	   comment said not to tighten below 64 because doing so "buys nothing
	   measured". That was wrong, and wrong the same way as several other
	   claims in this file before they were corrected: it generalised from a
	   single observed case. Over 383 truncations that would otherwise write
	   a wrong file:

	     threshold   caught   still slips
	        44        380         3
	        48        379         4
	        64        376         7

	   Lowering does buy something. Choosing 48 over 44 is the asymmetry
	   again -- a false positive refuses a good archive and is unacceptable,
	   a false negative only leaves the previous behaviour -- so a whole
	   spare byte over the ceiling is worth one extra escaping case. packJPG
	   measured the same shape in its own coder and reached the same
	   conclusion from the opposite starting point.

	   Measured over the same 216 dense truncation points, v3.0e as the
	   baseline and this check disabled in the middle row:

	                                  crashes   wrong file   clean error
	     v3.0e                           32        170           14
	     this check disabled              0        166           50
	     this check enabled               0          1          215

	   So it converts 166 wrong files into refusals. It is a mitigation and
	   not a guarantee -- one case still slips -- but it is the check doing
	   the detecting; the other three guards convert crashes into clean
	   errors without changing what is caught.

	   The reason is not a weakness in the threshold. A truncated archive can
	   be a perfectly valid encoding of a DIFFERENT file -- packJPG confirmed
	   the same property in its own coder by re-compressing the wrong output
	   and getting the truncated input back, byte for byte. When that
	   happens there is nothing in the data to detect. Closing it needs
	   information the format does not carry: a declared payload length
	   (kills truncation by construction, as packPNG demonstrated) or a
	   checksum (the only thing that catches full-length corruption). Both
	   are format changes.                                                  */
	/* One definition, used in exactly one place (read_bit). There used to be
	   an exhausted() accessor here as well, left over from an earlier shape
	   of this check; nothing called it any more and it still carried the old
	   value, so the file briefly held two thresholds that were supposed to
	   agree and did not. */
	static const int MAX_FABRICATED_BITS = 48;

	/* Latched once the decode is known to be running on data that is not a
	   valid stream: by exhaustion above, or by the model reporting an escape
	   below the order(-1) fallback. Decode loops must consult this -- see
	   the note in decode_ari. */
	void mark_corrupt( void ) { corrupt = true; }
	bool is_corrupt( void ) const { return corrupt; }
	
	private:
	// bitwise operations
	void write_bit( unsigned char bit );
	unsigned char read_bit( void );
	
	// i/o variables
	iostream* sptr;
	int mode;
	unsigned char bbyte;
	unsigned char cbit;
	int  past_eof_bits;  // bits fabricated after the stream ran out
	bool corrupt;        // latched: input is not a valid stream
	
	// arithmetic coding variables
	unsigned int ccode;
	unsigned int clow;
	unsigned int chigh;
	unsigned int cstep;
	unsigned int nrbits;
};


/* -----------------------------------------------
	universal statistical model for arithmetic coding
	----------------------------------------------- */
	
class model_s
{	
	public:
	
	model_s( int max_s, int max_c, int max_o, int c_lim );
	~model_s( void );
	
	void update_model( int symbol );
	void shift_context( int c );
	void flush_model( int scale_factor );
	void exclude_symbols( char rule, int c );
	
	int  convert_int_to_symbol( int c, symbol *s );
	void get_symbol_scale( symbol *s );
	int  convert_symbol_to_int( int count, symbol *s );

	/* True once an escape was decoded while already at the order(-1)
	   fallback. contexts[] is storage+1, so index -1 is a real slot holding
	   the uniform null table -- escaping from there is impossible for a
	   well-formed stream, because once any symbol remains un-excluded the
	   escape has zero probability width. Corrupt input can walk into it,
	   and the next context read would be contexts[-2], outside the
	   allocation. Reported rather than clamped: clamping leaves the decoder
	   escaping at the same order forever, which measured as a livelock --
	   worse for a user than the crash it replaced. */
	bool invalid_escape( void ) const { return escaped_past_null; }
	
	bool error;
	bool escaped_past_null;
	
	
	private:
	
	// unsigned short* totals;
	unsigned int* totals;
	char* scoreboard;
	int sb0_count;
	table_s **contexts;
	table_s **storage;
	
	int max_symbol;
	int max_context;
	int current_order;
	int max_order;
	int max_count;
	
	inline void totalize_table(table_s* context );
	inline void rescale_table(table_s* context, int scale_factor );
	inline void recursive_flush(table_s* context, int scale_factor );
	inline void recursive_cleanup(table_s* context );

	// Fenwick tree (BIT) fast-path helpers -- see table_s::bit above.
	inline void ensure_context_ready( table_s* context );
	inline void bit_add( table_s* context, int i, int delta );
	inline unsigned int bit_prefix( table_s* context, int i );
	inline int bit_find_kth( table_s* context, unsigned int target, unsigned int* out_prefix );
	inline void bit_rebuild( table_s* context );
	inline unsigned int fastpath_scale( table_s* context );
	inline void lazy_populate_scoreboard( table_s* context );
};


/* -----------------------------------------------
	binary statistical model for arithmetic coding
	----------------------------------------------- */
	
class model_b
{	
	public:
	
	model_b( int max_c, int max_o, int c_lim );
	~model_b( void );
	
	void update_model( int symbol );
	void shift_context( int c );
	void flush_model( int scale_factor );
	
	int  convert_int_to_symbol( int c, symbol *s );
	void get_symbol_scale( symbol *s );
	int  convert_symbol_to_int( int count, symbol *s );
	
	bool error;
	
	
	private:
	
	table **contexts;
	table **storage;
	
	int max_context;
	int max_order;
	int max_count;
	
	inline void check_counts( table *context );
	inline void rescale_table( table* context, int scale_factor );
	inline void recursive_flush( table* context, int scale_factor );
	inline void recursive_cleanup( table *context );
};


/* -----------------------------------------------
	shift context x2 model_s function
	----------------------------------------------- */
static inline void shift_model( model_s* model, int ctx1, int ctx2 )
{
	model->shift_context( ctx1 );
	model->shift_context( ctx2 );
}


/* -----------------------------------------------
	shift context x3 model_s function
	----------------------------------------------- */
static inline void shift_model( model_s* model, int ctx1, int ctx2, int ctx3 )
{
	model->shift_context( ctx1 );
	model->shift_context( ctx2 );
	model->shift_context( ctx3 );
}


/* -----------------------------------------------
	shift context x2 model_b function
	----------------------------------------------- */
static inline void shift_model( model_b* model, int ctx1, int ctx2 )
{
	model->shift_context( ctx1 );
	model->shift_context( ctx2 );
}


/* -----------------------------------------------
	shift context x3 model_b function
	----------------------------------------------- */
static inline void shift_model( model_b* model, int ctx1, int ctx2, int ctx3 )
{
	model->shift_context( ctx1 );
	model->shift_context( ctx2 );
	model->shift_context( ctx3 );
}


/* -----------------------------------------------
	generic model_s encoder function
	----------------------------------------------- */
static inline void encode_ari( aricoder* encoder, model_s* model, int c )
{
	symbol s;		// plain locals: scratch, fully overwritten each call.
	int esc;		// (were 'static' — a data race under -th multithreading)

	do {
		esc = model->convert_int_to_symbol( c, &s );
		encoder->encode( &s );
	} while ( esc );
	model->update_model( c );
}

/* -----------------------------------------------
	generic model_s decoder function
	----------------------------------------------- */	
static inline int decode_ari( aricoder* decoder, model_s* model )
{
	symbol s;
	unsigned int count;
	int c;

	/* Once the input is known to be invalid, stop feeding the model and hand
	   back symbol 0.

	   Returning 0 rather than a sentinel is deliberate. Callers use this
	   return value directly as an array index, a loop bound or a struct
	   field, at ~60 call sites, so a sentinel does not stop the damage -- it
	   relocates it. An earlier attempt at this bound returned ESCAPE_SYMBOL,
	   which is CODER_LIMIT025 (536870912); callers then used that as a real
	   symbol and the crash simply moved elsewhere. Symbol 0 exists in every
	   model this coder is used with, so it is inert.

	   The decode is not finished here, only made harmless: the outer decode
	   loops check is_corrupt() and abort with a diagnostic. */
	if ( decoder->is_corrupt() ) return 0;

	do{
		model->get_symbol_scale( &s );
		count = decoder->decode_count( &s );
		c = model->convert_symbol_to_int( count, &s );
		decoder->decode( &s );
		if ( c != ESCAPE_SYMBOL ) break;
		// Only on the escape path. Escapes are rare next to symbols, and the
		// invalid-escape flag can only change here, so testing it per symbol
		// was pure cost in the hot loop.
		if ( model->invalid_escape() ) { decoder->mark_corrupt(); return 0; }
	} while ( true );

	// One bool load per symbol: exhaustion is latched inside read_bit, so
	// there is no arithmetic left to do here.
	if ( decoder->is_corrupt() ) return 0;
	model->update_model( c );
	
	return c;
}

/* -----------------------------------------------
	generic model_b encoder function
	----------------------------------------------- */	
static inline void encode_ari( aricoder* encoder, model_b* model, int c )
{
	symbol s;

	model->convert_int_to_symbol( c, &s );
	encoder->encode( &s );
	model->update_model( c );
}

/* -----------------------------------------------
	generic model_b decoder function
	----------------------------------------------- */	
static inline int decode_ari( aricoder* decoder, model_b* model )
{
	symbol s;
	unsigned int count;
	int c;

	// Same reasoning as the model_s overload above: 0 is inert, a sentinel
	// is not. The binary model has no escape mechanism, so exhaustion is the
	// only signal that applies here.
	if ( decoder->is_corrupt() ) return 0;

	model->get_symbol_scale( &s );
	count = decoder->decode_count( &s );
	c = model->convert_symbol_to_int( count, &s );
	decoder->decode( &s );
	if ( decoder->is_corrupt() ) return 0;
	model->update_model( c );
	
	return c;
}
