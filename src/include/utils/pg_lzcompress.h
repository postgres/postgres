/* ----------
 * pg_lzcompress.h -
 *
 * $Header: /cvsroot/pgsql/src/include/utils/pg_lzcompress.h,v 1.9 2002/11/23 03:59:09 momjian Exp $
 *
 *	Definitions for the builtin LZ compressor
 * ----------
 */

#ifndef _PG_LZCOMPRESS_H_
#define _PG_LZCOMPRESS_H_


/* ----------
 * PGLZ_Header -
 *
 *		The information at the top of the compressed data.
 *		The varsize must be kept the same data type as the value
 *		in front of all variable size data types in PostgreSQL.
 * ----------
 */
typedef struct PGLZ_Header
{
	int32		varsize;
	int32		rawsize;
} PGLZ_Header;


/* ----------
 * PGLZ_MAX_OUTPUT -
 *
 *		Macro to compute the maximum buffer required for the
 *		compression output. It is larger than the input, because
 *		in the worst case, we cannot write out one single tag but
 *		need one control byte per 8 literal data bytes plus the
 *		EOF mark at the end.
 * ----------
 */
#define PGLZ_MAX_OUTPUT(_dlen)			((_dlen) + (((_dlen) | 0x07) >> 3)	\
													 + sizeof(PGLZ_Header))

/* ----------
 * PGLZ_RAW_SIZE -
 *
 *		Macro to determine the uncompressed data size contained
 *		in the entry.
 * ----------
 */
#define PGLZ_RAW_SIZE(_lzdata)			(_lzdata->rawsize)

/* ----------
 * PGLZ_IS_COMPRESSED -
 *
 *		Macro to determine if the data itself is stored as raw
 *		uncompressed data.
 * ----------
 */
#define PGLZ_IS_COMPRESSED(_lzdata)		(_lzdata->varsize !=				\
										 _lzdata->rawsize +					\
														sizeof(PGLZ_Header))

/* ----------
 * PGLZ_RAW_DATA -
 *
 *		Macro to get access to the plain compressed or uncompressed
 *		data. Useful if PGLZ_IS_COMPRESSED returns false.
 * ----------
 */
#define PGLZ_RAW_DATA(_lzdata)			(((char *)(_lzdata)) +				\
														sizeof(PGLZ_Header))

/* ----------
 * PGLZ_Strategy -
 *
 *		Some values that control the compression algorithm.
 *
 *		min_input_size		Minimum input data size to start compression.
 *
 *		force_input_size	Input data size at which compressed storage is
 *							forced even if the compression rate drops below
 *							min_comp_rate (but not below 0).
 *
 *		min_comp_rate		Minimum compression rate (0-99%), the output
 *							must be smaller than the input. If that isn't
 *							the case, the compressor will throw away its
 *							output and copy the original, uncompressed data
 *							to the output buffer.
 *
 *		match_size_good		The initial GOOD match size when starting history
 *							lookup. When looking up the history to find a
 *							match that could be expressed as a tag, the
 *							algorithm does not always walk back entirely.
 *							A good match fast is usually better than the
 *							best possible one very late. For each iteration
 *							in the lookup, this value is lowered so the
 *							longer the lookup takes, the smaller matches
 *							are considered good.
 *
 *		match_size_drop		The percentage, match_size_good is lowered
 *							at each history check. Allowed values are
 *							0 (no change until end) to 100 (only check
 *							latest history entry at all).
 * ----------
 */
typedef struct PGLZ_Strategy
{
	int32		min_input_size;
	int32		force_input_size;
	int32		min_comp_rate;
	int32		match_size_good;
	int32		match_size_drop;
} PGLZ_Strategy;


/* ----------
 * PGLZ_DecompState -
 *
 *		Decompression state variable for byte-per-byte decompression
 *		using pglz_decomp_getchar() macro.
 * ----------
 */
typedef struct PGLZ_DecompState
{
	unsigned char *temp_buf;
	unsigned char *cp_in;
	unsigned char *cp_end;
	unsigned char *cp_out;
	unsigned char *cp_copy;
	int			(*next_char) (struct PGLZ_DecompState *dstate);
	int			tocopy;
	int			ctrl_count;
	unsigned char ctrl;
} PGLZ_DecompState;


/* ----------
 * The standard strategies
 *
 *		PGLZ_strategy_default		Starts compression only if input is
 *									at least 256 bytes large. Stores output
 *									uncompressed if compression does not
 *									gain at least 20% size reducture but
 *									input does not exceed 6K. Stops history
 *									lookup if at least a 128 byte long
 *									match has been found.
 *
 *									This is the default strategy if none
 *									is given to pglz_compress().
 *
 *		PGLZ_strategy_always		Starts compression on any infinitely
 *									small input and does fallback to
 *									uncompressed storage only if output
 *									would be larger than input.
 *
 *		PGLZ_strategy_never			Force pglz_compress to act as a custom
 *									interface for memcpy(). Only useful
 *									for generic interfacing.
 * ----------
 */
extern PGLZ_Strategy *PGLZ_strategy_default;
extern PGLZ_Strategy *PGLZ_strategy_always;
extern PGLZ_Strategy *PGLZ_strategy_never;


/* ----------
 * pglz_decomp_getchar -
 *
 *		Get next character (or EOF) from decompressor.
 *		The status variable must be initialized before and deinitialized
 *		after compression with the next two macros below.
 * ----------
 */
#define pglz_decomp_getchar(_ds)											\
	((*((_ds)->next_char))((_ds)))


/* ----------
 * pglz_decomp_init -
 *
 *		Initialize a decomp state from a compressed input.
 * ----------
 */
#define pglz_decomp_init(_ds,_lz) do {											\
		(_ds)->cp_in		= ((unsigned char *)(_lz))						\
											+ sizeof(PGLZ_Header);			\
		(_ds)->cp_end		= (_ds)->cp_in + (_lz)->varsize					\
											- sizeof(PGLZ_Header);			\
		if (PGLZ_IS_COMPRESSED((_lz))) {									\
			(_ds)->temp_buf		= (unsigned char *)							\
										palloc(PGLZ_RAW_SIZE((_lz)));		\
			(_ds)->cp_out		= (_ds)->temp_buf;							\
			(_ds)->next_char	= pglz_get_next_decomp_char_from_lzdata;	\
			(_ds)->tocopy		= 0;										\
			(_ds)->ctrl_count	= 0;										\
		} else {															\
			(_ds)->temp_buf		= NULL;										\
			(_ds)->next_char	= pglz_get_next_decomp_char_from_plain;		\
		}																	\
	} while (0)


/* ----------
 * pglz_decomp_end -
 *
 *		Deallocate resources after decompression.
 * ----------
 */
#define pglz_decomp_end(_ds) do {											\
		if ((_ds)->temp_buf != NULL)										\
			pfree((void *)((_ds)->temp_buf));								\
	} while (0)


/* ----------
 * Global function declarations
 * ----------
 */
int pglz_compress(char *source, int32 slen, PGLZ_Header *dest,
			  PGLZ_Strategy *strategy);
int			pglz_decompress(PGLZ_Header *source, char *dest);


/* ----------
 * Functions used by pglz_decomp_getchar().
 * Internal use only.
 * ----------
 */
extern int	pglz_get_next_decomp_char_from_lzdata(PGLZ_DecompState *dstate);
extern int	pglz_get_next_decomp_char_from_plain(PGLZ_DecompState *dstate);

#endif   /* _PG_LZCOMPRESS_H_ */
