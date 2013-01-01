/* ----------
 * pg_lzcompress.c -
 *
 *		This is an implementation of LZ compression for PostgreSQL.
 *		It uses a simple history table and generates 2-3 byte tags
 *		capable of backward copy information for 3-273 bytes with
 *		a max offset of 4095.
 *
 *		Entry routines:
 *
 *			bool
 *			pglz_compress(const char *source, int32 slen, PGLZ_Header *dest,
 *						  const PGLZ_Strategy *strategy);
 *
 *				source is the input data to be compressed.
 *
 *				slen is the length of the input data.
 *
 *				dest is the output area for the compressed result.
 *					It must be at least as big as PGLZ_MAX_OUTPUT(slen).
 *
 *				strategy is a pointer to some information controlling
 *					the compression algorithm. If NULL, the compiled
 *					in default strategy is used.
 *
 *				The return value is TRUE if compression succeeded,
 *				FALSE if not; in the latter case the contents of dest
 *				are undefined.
 *
 *			void
 *			pglz_decompress(const PGLZ_Header *source, char *dest)
 *
 *				source is the compressed input.
 *
 *				dest is the area where the uncompressed data will be
 *					written to. It is the callers responsibility to
 *					provide enough space. The required amount can be
 *					obtained with the macro PGLZ_RAW_SIZE(source).
 *
 *					The data is written to buff exactly as it was handed
 *					to pglz_compress(). No terminating zero byte is added.
 *
 *		The decompression algorithm and internal data format:
 *
 *			PGLZ_Header is defined as
 *
 *				typedef struct PGLZ_Header {
 *					int32		vl_len_;
 *					int32		rawsize;
 *				}
 *
 *			The header is followed by the compressed data itself.
 *
 *			The data representation is easiest explained by describing
 *			the process of decompression.
 *
 *			If VARSIZE(x) == rawsize + sizeof(PGLZ_Header), then the data
 *			is stored uncompressed as plain bytes. Thus, the decompressor
 *			simply copies rawsize bytes from the location after the
 *			header to the destination.
 *
 *			Otherwise the first byte after the header tells what to do
 *			the next 8 times. We call this the control byte.
 *
 *			An unset bit in the control byte means, that one uncompressed
 *			byte follows, which is copied from input to output.
 *
 *			A set bit in the control byte means, that a tag of 2-3 bytes
 *			follows. A tag contains information to copy some bytes, that
 *			are already in the output buffer, to the current location in
 *			the output. Let's call the three tag bytes T1, T2 and T3. The
 *			position of the data to copy is coded as an offset from the
 *			actual output position.
 *
 *			The offset is in the upper nibble of T1 and in T2.
 *			The length is in the lower nibble of T1.
 *
 *			So the 16 bits of a 2 byte tag are coded as
 *
 *				7---T1--0  7---T2--0
 *				OOOO LLLL  OOOO OOOO
 *
 *			This limits the offset to 1-4095 (12 bits) and the length
 *			to 3-18 (4 bits) because 3 is always added to it. To emit
 *			a tag of 2 bytes with a length of 2 only saves one control
 *			bit. But we lose one byte in the possible length of a tag.
 *
 *			In the actual implementation, the 2 byte tag's length is
 *			limited to 3-17, because the value 0xF in the length nibble
 *			has special meaning. It means, that the next following
 *			byte (T3) has to be added to the length value of 18. That
 *			makes total limits of 1-4095 for offset and 3-273 for length.
 *
 *			Now that we have successfully decoded a tag. We simply copy
 *			the output that occurred <offset> bytes back to the current
 *			output location in the specified <length>. Thus, a
 *			sequence of 200 spaces (think about bpchar fields) could be
 *			coded in 4 bytes. One literal space and a three byte tag to
 *			copy 199 bytes with a -1 offset. Whow - that's a compression
 *			rate of 98%! Well, the implementation needs to save the
 *			original data size too, so we need another 4 bytes for it
 *			and end up with a total compression rate of 96%, what's still
 *			worth a Whow.
 *
 *		The compression algorithm
 *
 *			The following uses numbers used in the default strategy.
 *
 *			The compressor works best for attributes of a size between
 *			1K and 1M. For smaller items there's not that much chance of
 *			redundancy in the character sequence (except for large areas
 *			of identical bytes like trailing spaces) and for bigger ones
 *			our 4K maximum look-back distance is too small.
 *
 *			The compressor creates a table for 8192 lists of positions.
 *			For each input position (except the last 3), a hash key is
 *			built from the 4 next input bytes and the position remembered
 *			in the appropriate list. Thus, the table points to linked
 *			lists of likely to be at least in the first 4 characters
 *			matching strings. This is done on the fly while the input
 *			is compressed into the output area.  Table entries are only
 *			kept for the last 4096 input positions, since we cannot use
 *			back-pointers larger than that anyway.
 *
 *			For each byte in the input, it's hash key (built from this
 *			byte and the next 3) is used to find the appropriate list
 *			in the table. The lists remember the positions of all bytes
 *			that had the same hash key in the past in increasing backward
 *			offset order. Now for all entries in the used lists, the
 *			match length is computed by comparing the characters from the
 *			entries position with the characters from the actual input
 *			position.
 *
 *			The compressor starts with a so called "good_match" of 128.
 *			It is a "prefer speed against compression ratio" optimizer.
 *			So if the first entry looked at already has 128 or more
 *			matching characters, the lookup stops and that position is
 *			used for the next tag in the output.
 *
 *			For each subsequent entry in the history list, the "good_match"
 *			is lowered by 10%. So the compressor will be more happy with
 *			short matches the farer it has to go back in the history.
 *			Another "speed against ratio" preference characteristic of
 *			the algorithm.
 *
 *			Thus there are 3 stop conditions for the lookup of matches:
 *
 *				- a match >= good_match is found
 *				- there are no more history entries to look at
 *				- the next history entry is already too far back
 *				  to be coded into a tag.
 *
 *			Finally the match algorithm checks that at least a match
 *			of 3 or more bytes has been found, because thats the smallest
 *			amount of copy information to code into a tag. If so, a tag
 *			is omitted and all the input bytes covered by that are just
 *			scanned for the history add's, otherwise a literal character
 *			is omitted and only his history entry added.
 *
 *		Acknowledgements:
 *
 *			Many thanks to Adisak Pochanayon, who's article about SLZ
 *			inspired me to write the PostgreSQL compression this way.
 *
 *			Jan Wieck
 *
 * Copyright (c) 1999-2013, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/pg_lzcompress.c
 * ----------
 */
#include "postgres.h"

#include <limits.h>

#include "utils/pg_lzcompress.h"


/* ----------
 * Local definitions
 * ----------
 */
#define PGLZ_HISTORY_LISTS		8192	/* must be power of 2 */
#define PGLZ_HISTORY_MASK		(PGLZ_HISTORY_LISTS - 1)
#define PGLZ_HISTORY_SIZE		4096
#define PGLZ_MAX_MATCH			273


/* ----------
 * PGLZ_HistEntry -
 *
 *		Linked list for the backward history lookup
 *
 * All the entries sharing a hash key are linked in a doubly linked list.
 * This makes it easy to remove an entry when it's time to recycle it
 * (because it's more than 4K positions old).
 * ----------
 */
typedef struct PGLZ_HistEntry
{
	struct PGLZ_HistEntry *next;	/* links for my hash key's list */
	struct PGLZ_HistEntry *prev;
	int			hindex;			/* my current hash key */
	const char *pos;			/* my input position */
} PGLZ_HistEntry;


/* ----------
 * The provided standard strategies
 * ----------
 */
static const PGLZ_Strategy strategy_default_data = {
	32,							/* Data chunks less than 32 bytes are not
								 * compressed */
	INT_MAX,					/* No upper limit on what we'll try to
								 * compress */
	25,							/* Require 25% compression rate, or not worth
								 * it */
	1024,						/* Give up if no compression in the first 1KB */
	128,						/* Stop history lookup if a match of 128 bytes
								 * is found */
	10							/* Lower good match size by 10% at every loop
								 * iteration */
};
const PGLZ_Strategy *const PGLZ_strategy_default = &strategy_default_data;


static const PGLZ_Strategy strategy_always_data = {
	0,							/* Chunks of any size are compressed */
	INT_MAX,
	0,							/* It's enough to save one single byte */
	INT_MAX,					/* Never give up early */
	128,						/* Stop history lookup if a match of 128 bytes
								 * is found */
	6							/* Look harder for a good match */
};
const PGLZ_Strategy *const PGLZ_strategy_always = &strategy_always_data;


/* ----------
 * Statically allocated work arrays for history
 * ----------
 */
static PGLZ_HistEntry *hist_start[PGLZ_HISTORY_LISTS];
static PGLZ_HistEntry hist_entries[PGLZ_HISTORY_SIZE];


/* ----------
 * pglz_hist_idx -
 *
 *		Computes the history table slot for the lookup by the next 4
 *		characters in the input.
 *
 * NB: because we use the next 4 characters, we are not guaranteed to
 * find 3-character matches; they very possibly will be in the wrong
 * hash list.  This seems an acceptable tradeoff for spreading out the
 * hash keys more.
 * ----------
 */
#define pglz_hist_idx(_s,_e) (												\
			((((_e) - (_s)) < 4) ? (int) (_s)[0] :							\
			 (((_s)[0] << 9) ^ ((_s)[1] << 6) ^								\
			  ((_s)[2] << 3) ^ (_s)[3])) & (PGLZ_HISTORY_MASK)				\
		)


/* ----------
 * pglz_hist_add -
 *
 *		Adds a new entry to the history table.
 *
 * If _recycle is true, then we are recycling a previously used entry,
 * and must first delink it from its old hashcode's linked list.
 *
 * NOTE: beware of multiple evaluations of macro's arguments, and note that
 * _hn and _recycle are modified in the macro.
 * ----------
 */
#define pglz_hist_add(_hs,_he,_hn,_recycle,_s,_e) \
do {									\
			int __hindex = pglz_hist_idx((_s),(_e));						\
			PGLZ_HistEntry **__myhsp = &(_hs)[__hindex];					\
			PGLZ_HistEntry *__myhe = &(_he)[_hn];							\
			if (_recycle) {													\
				if (__myhe->prev == NULL)									\
					(_hs)[__myhe->hindex] = __myhe->next;					\
				else														\
					__myhe->prev->next = __myhe->next;						\
				if (__myhe->next != NULL)									\
					__myhe->next->prev = __myhe->prev;						\
			}																\
			__myhe->next = *__myhsp;										\
			__myhe->prev = NULL;											\
			__myhe->hindex = __hindex;										\
			__myhe->pos  = (_s);											\
			if (*__myhsp != NULL)											\
				(*__myhsp)->prev = __myhe;									\
			*__myhsp = __myhe;												\
			if (++(_hn) >= PGLZ_HISTORY_SIZE) {								\
				(_hn) = 0;													\
				(_recycle) = true;											\
			}																\
} while (0)


/* ----------
 * pglz_out_ctrl -
 *
 *		Outputs the last and allocates a new control byte if needed.
 * ----------
 */
#define pglz_out_ctrl(__ctrlp,__ctrlb,__ctrl,__buf) \
do { \
	if ((__ctrl & 0xff) == 0)												\
	{																		\
		*(__ctrlp) = __ctrlb;												\
		__ctrlp = (__buf)++;												\
		__ctrlb = 0;														\
		__ctrl = 1;															\
	}																		\
} while (0)


/* ----------
 * pglz_out_literal -
 *
 *		Outputs a literal byte to the destination buffer including the
 *		appropriate control bit.
 * ----------
 */
#define pglz_out_literal(_ctrlp,_ctrlb,_ctrl,_buf,_byte) \
do { \
	pglz_out_ctrl(_ctrlp,_ctrlb,_ctrl,_buf);								\
	*(_buf)++ = (unsigned char)(_byte);										\
	_ctrl <<= 1;															\
} while (0)


/* ----------
 * pglz_out_tag -
 *
 *		Outputs a backward reference tag of 2-4 bytes (depending on
 *		offset and length) to the destination buffer including the
 *		appropriate control bit.
 * ----------
 */
#define pglz_out_tag(_ctrlp,_ctrlb,_ctrl,_buf,_len,_off) \
do { \
	pglz_out_ctrl(_ctrlp,_ctrlb,_ctrl,_buf);								\
	_ctrlb |= _ctrl;														\
	_ctrl <<= 1;															\
	if (_len > 17)															\
	{																		\
		(_buf)[0] = (unsigned char)((((_off) & 0xf00) >> 4) | 0x0f);		\
		(_buf)[1] = (unsigned char)(((_off) & 0xff));						\
		(_buf)[2] = (unsigned char)((_len) - 18);							\
		(_buf) += 3;														\
	} else {																\
		(_buf)[0] = (unsigned char)((((_off) & 0xf00) >> 4) | ((_len) - 3)); \
		(_buf)[1] = (unsigned char)((_off) & 0xff);							\
		(_buf) += 2;														\
	}																		\
} while (0)


/* ----------
 * pglz_find_match -
 *
 *		Lookup the history table if the actual input stream matches
 *		another sequence of characters, starting somewhere earlier
 *		in the input buffer.
 * ----------
 */
static inline int
pglz_find_match(PGLZ_HistEntry **hstart, const char *input, const char *end,
				int *lenp, int *offp, int good_match, int good_drop)
{
	PGLZ_HistEntry *hent;
	int32		len = 0;
	int32		off = 0;

	/*
	 * Traverse the linked history list until a good enough match is found.
	 */
	hent = hstart[pglz_hist_idx(input, end)];
	while (hent)
	{
		const char *ip = input;
		const char *hp = hent->pos;
		int32		thisoff;
		int32		thislen;

		/*
		 * Stop if the offset does not fit into our tag anymore.
		 */
		thisoff = ip - hp;
		if (thisoff >= 0x0fff)
			break;

		/*
		 * Determine length of match. A better match must be larger than the
		 * best so far. And if we already have a match of 16 or more bytes,
		 * it's worth the call overhead to use memcmp() to check if this match
		 * is equal for the same size. After that we must fallback to
		 * character by character comparison to know the exact position where
		 * the diff occurred.
		 */
		thislen = 0;
		if (len >= 16)
		{
			if (memcmp(ip, hp, len) == 0)
			{
				thislen = len;
				ip += len;
				hp += len;
				while (ip < end && *ip == *hp && thislen < PGLZ_MAX_MATCH)
				{
					thislen++;
					ip++;
					hp++;
				}
			}
		}
		else
		{
			while (ip < end && *ip == *hp && thislen < PGLZ_MAX_MATCH)
			{
				thislen++;
				ip++;
				hp++;
			}
		}

		/*
		 * Remember this match as the best (if it is)
		 */
		if (thislen > len)
		{
			len = thislen;
			off = thisoff;
		}

		/*
		 * Advance to the next history entry
		 */
		hent = hent->next;

		/*
		 * Be happy with lesser good matches the more entries we visited. But
		 * no point in doing calculation if we're at end of list.
		 */
		if (hent)
		{
			if (len >= good_match)
				break;
			good_match -= (good_match * good_drop) / 100;
		}
	}

	/*
	 * Return match information only if it results at least in one byte
	 * reduction.
	 */
	if (len > 2)
	{
		*lenp = len;
		*offp = off;
		return 1;
	}

	return 0;
}


/* ----------
 * pglz_compress -
 *
 *		Compresses source into dest using strategy.
 * ----------
 */
bool
pglz_compress(const char *source, int32 slen, PGLZ_Header *dest,
			  const PGLZ_Strategy *strategy)
{
	unsigned char *bp = ((unsigned char *) dest) + sizeof(PGLZ_Header);
	unsigned char *bstart = bp;
	int			hist_next = 0;
	bool		hist_recycle = false;
	const char *dp = source;
	const char *dend = source + slen;
	unsigned char ctrl_dummy = 0;
	unsigned char *ctrlp = &ctrl_dummy;
	unsigned char ctrlb = 0;
	unsigned char ctrl = 0;
	bool		found_match = false;
	int32		match_len;
	int32		match_off;
	int32		good_match;
	int32		good_drop;
	int32		result_size;
	int32		result_max;
	int32		need_rate;

	/*
	 * Our fallback strategy is the default.
	 */
	if (strategy == NULL)
		strategy = PGLZ_strategy_default;

	/*
	 * If the strategy forbids compression (at all or if source chunk size out
	 * of range), fail.
	 */
	if (strategy->match_size_good <= 0 ||
		slen < strategy->min_input_size ||
		slen > strategy->max_input_size)
		return false;

	/*
	 * Save the original source size in the header.
	 */
	dest->rawsize = slen;

	/*
	 * Limit the match parameters to the supported range.
	 */
	good_match = strategy->match_size_good;
	if (good_match > PGLZ_MAX_MATCH)
		good_match = PGLZ_MAX_MATCH;
	else if (good_match < 17)
		good_match = 17;

	good_drop = strategy->match_size_drop;
	if (good_drop < 0)
		good_drop = 0;
	else if (good_drop > 100)
		good_drop = 100;

	need_rate = strategy->min_comp_rate;
	if (need_rate < 0)
		need_rate = 0;
	else if (need_rate > 99)
		need_rate = 99;

	/*
	 * Compute the maximum result size allowed by the strategy, namely the
	 * input size minus the minimum wanted compression rate.  This had better
	 * be <= slen, else we might overrun the provided output buffer.
	 */
	if (slen > (INT_MAX / 100))
	{
		/* Approximate to avoid overflow */
		result_max = (slen / 100) * (100 - need_rate);
	}
	else
		result_max = (slen * (100 - need_rate)) / 100;

	/*
	 * Initialize the history lists to empty.  We do not need to zero the
	 * hist_entries[] array; its entries are initialized as they are used.
	 */
	memset(hist_start, 0, sizeof(hist_start));

	/*
	 * Compress the source directly into the output buffer.
	 */
	while (dp < dend)
	{
		/*
		 * If we already exceeded the maximum result size, fail.
		 *
		 * We check once per loop; since the loop body could emit as many as 4
		 * bytes (a control byte and 3-byte tag), PGLZ_MAX_OUTPUT() had better
		 * allow 4 slop bytes.
		 */
		if (bp - bstart >= result_max)
			return false;

		/*
		 * If we've emitted more than first_success_by bytes without finding
		 * anything compressible at all, fail.	This lets us fall out
		 * reasonably quickly when looking at incompressible input (such as
		 * pre-compressed data).
		 */
		if (!found_match && bp - bstart >= strategy->first_success_by)
			return false;

		/*
		 * Try to find a match in the history
		 */
		if (pglz_find_match(hist_start, dp, dend, &match_len,
							&match_off, good_match, good_drop))
		{
			/*
			 * Create the tag and add history entries for all matched
			 * characters.
			 */
			pglz_out_tag(ctrlp, ctrlb, ctrl, bp, match_len, match_off);
			while (match_len--)
			{
				pglz_hist_add(hist_start, hist_entries,
							  hist_next, hist_recycle,
							  dp, dend);
				dp++;			/* Do not do this ++ in the line above! */
				/* The macro would do it four times - Jan.	*/
			}
			found_match = true;
		}
		else
		{
			/*
			 * No match found. Copy one literal byte.
			 */
			pglz_out_literal(ctrlp, ctrlb, ctrl, bp, *dp);
			pglz_hist_add(hist_start, hist_entries,
						  hist_next, hist_recycle,
						  dp, dend);
			dp++;				/* Do not do this ++ in the line above! */
			/* The macro would do it four times - Jan.	*/
		}
	}

	/*
	 * Write out the last control byte and check that we haven't overrun the
	 * output size allowed by the strategy.
	 */
	*ctrlp = ctrlb;
	result_size = bp - bstart;
	if (result_size >= result_max)
		return false;

	/*
	 * Success - need only fill in the actual length of the compressed datum.
	 */
	SET_VARSIZE_COMPRESSED(dest, result_size + sizeof(PGLZ_Header));

	return true;
}


/* ----------
 * pglz_decompress -
 *
 *		Decompresses source into dest.
 * ----------
 */
void
pglz_decompress(const PGLZ_Header *source, char *dest)
{
	const unsigned char *sp;
	const unsigned char *srcend;
	unsigned char *dp;
	unsigned char *destend;

	sp = ((const unsigned char *) source) + sizeof(PGLZ_Header);
	srcend = ((const unsigned char *) source) + VARSIZE(source);
	dp = (unsigned char *) dest;
	destend = dp + source->rawsize;

	while (sp < srcend && dp < destend)
	{
		/*
		 * Read one control byte and process the next 8 items (or as many as
		 * remain in the compressed input).
		 */
		unsigned char ctrl = *sp++;
		int			ctrlc;

		for (ctrlc = 0; ctrlc < 8 && sp < srcend; ctrlc++)
		{
			if (ctrl & 1)
			{
				/*
				 * Otherwise it contains the match length minus 3 and the
				 * upper 4 bits of the offset. The next following byte
				 * contains the lower 8 bits of the offset. If the length is
				 * coded as 18, another extension tag byte tells how much
				 * longer the match really was (0-255).
				 */
				int32		len;
				int32		off;

				len = (sp[0] & 0x0f) + 3;
				off = ((sp[0] & 0xf0) << 4) | sp[1];
				sp += 2;
				if (len == 18)
					len += *sp++;

				/*
				 * Check for output buffer overrun, to ensure we don't clobber
				 * memory in case of corrupt input.  Note: we must advance dp
				 * here to ensure the error is detected below the loop.  We
				 * don't simply put the elog inside the loop since that will
				 * probably interfere with optimization.
				 */
				if (dp + len > destend)
				{
					dp += len;
					break;
				}

				/*
				 * Now we copy the bytes specified by the tag from OUTPUT to
				 * OUTPUT. It is dangerous and platform dependent to use
				 * memcpy() here, because the copied areas could overlap
				 * extremely!
				 */
				while (len--)
				{
					*dp = dp[-off];
					dp++;
				}
			}
			else
			{
				/*
				 * An unset control bit means LITERAL BYTE. So we just copy
				 * one from INPUT to OUTPUT.
				 */
				if (dp >= destend)		/* check for buffer overrun */
					break;		/* do not clobber memory */

				*dp++ = *sp++;
			}

			/*
			 * Advance the control bit
			 */
			ctrl >>= 1;
		}
	}

	/*
	 * Check we decompressed the right amount.
	 */
	if (dp != destend || sp != srcend)
		elog(ERROR, "compressed data is corrupt");

	/*
	 * That's it.
	 */
}
