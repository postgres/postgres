/* ----------
 * pg_lzcompress.c -
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/pg_lzcompress.c,v 1.2 1999/11/17 22:18:45 wieck Exp $
 *
 *		This is an implementation of LZ compression for PostgreSQL.
 *		It uses a simple history table and generates 2-3 byte tags
 *		capable of backward copy information for 3-273 bytes with
 *		an offset of max. 4095.
 *
 *		Entry routines:
 *
 *			int
 *			pglz_compress(char *source, int slen, PGLZ_Header *dest,
 *										PGLZ_Strategy *strategy);
 *
 *				source is the input data to be compressed.
 *
 *				slen is the length of the input data.
 *
 *				dest is the output area for the compressed result.
 *					It must be big enough to hold the worst case of
 *					compression failure and can be computed by the
 *					macro PGLZ_MAX_OUTPUT(slen). Don't be surprised,
 *					it is larger than the input data size.
 *
 *				strategy is a pointer to some information controlling
 *					the compression algorithm. If NULL, the compiled
 *					in default strategy is used.
 *
 *				The return value is the size of bytes written to buff.
 *
 *			int
 *			pglz_decompress(PGLZ_Header *source, char *dest)
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
 *				The return value is the size of bytes written to buff.
 *					Obviously the same as PGLZ_RAW_SIZE() returns.
 *
 *		The compression algorithm and internal data format:
 *
 *			PGLZ_Header is defined as
 *
 *				typedef struct PGLZ_Header {
 *					int32		varsize;
 *					int32		rawsize;
 *				}
 *
 *			The header is followed by the compressed data itself.
 *
 *			The algorithm is easiest explained by describing the process
 *			of decompression.
 *
 *			If varsize == rawsize + sizeof(PGLZ_Header), then the data
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
 *              7---T1--0  7---T2--0
 *				OOOO LLLL  OOOO OOOO
 *
 *			This limits the offset to 1-4095 (12 bits) and the length
 *			to 3-18 (4 bits) because 3 is allways added to it. To emit
 *			a tag of 2 bytes with a length of 2 only saves one control
 *			bit. But we loose one byte in the possible length of a tag.
 *
 *			In the actual implementation, the 2 byte tag's length is
 *			limited to 3-17, because the value 0xF in the length nibble
 *			has special meaning. It means, that the next following
 *			byte (T3) has to be added to the length value of 18. That
 *			makes total limits of 1-4095 for offset and 3-273 for length.
 *
 *			Now that we have successfully decoded a tag. We simply copy
 *			the output that occured <offset> bytes back to the current
 *			output location in the specified <length>. Thus, a
 *			sequence of 200 spaces (think about bpchar fields) could be
 *			coded in 4 bytes. One literal space and a three byte tag to
 *			copy 199 bytes with a -1 offset. Whow - that's a compression
 *			rate of 98%! Well, the implementation needs to save the
 *			original data size too, so we need another 4 bytes for it
 *			and end up with a total compression rate of 96%, what's still
 *			worth a Whow.
 *
 *		Acknowledgements:
 *
 *			Many thanks to Adisak Pochanayon, who's article about SLZ
 *			inspired me to write the PostgreSQL compression this way.
 *
 *			Jan Wieck
 * ----------
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include "postgres.h"
#include "utils/palloc.h"
#include "utils/pg_lzcompress.h"


/* ----------
 * Local definitions
 * ----------
 */
#define PGLZ_HISTORY_SIZE		8192
#define PGLZ_HISTORY_MASK		0x1fff
#define PGLZ_HISTORY_PREALLOC	8192
#define PGLZ_MAX_MATCH			273


/* ----------
 * PGLZ_HistEntry -
 *
 *		Linked list for the backward history lookup
 * ----------
 */
typedef struct PGLZ_HistEntry {
	struct PGLZ_HistEntry	   *next;
	char					   *pos;
} PGLZ_HistEntry;


/* ----------
 * The provided standard strategies
 * ----------
 */
static PGLZ_Strategy strategy_default_data = {
	256,	/* Data chunks smaller 256 bytes are nott compressed			*/
	6144,	/* Data chunks greater equal 6K force compression				*/
			/* except compressed result is greater uncompressed data		*/
	20,		/* Compression rates below 20% mean fallback to uncompressed	*/
			/* storage except compression is forced by previous parameter	*/
	128,	/* Stop history lookup if a match of 128 bytes is found			*/
	10		/* Lower good match size by 10% at every lookup loop iteration.	*/
};
PGLZ_Strategy	*PGLZ_strategy_default = &strategy_default_data;


static PGLZ_Strategy strategy_allways_data = {
	0,		/* Chunks of any size are compressed							*/
	0,		/* 																*/
	0,		/* We want to save at least one single byte						*/
	128,	/* Stop history lookup if a match of 128 bytes is found			*/
	6		/* Look harder for a good match.								*/
};
PGLZ_Strategy	*PGLZ_strategy_allways = &strategy_allways_data;


static PGLZ_Strategy strategy_never_data = {
	0,		/* 																*/
	0,		/* 																*/
	0,		/* 																*/
	0,		/* Zero indicates "store uncompressed allways"					*/
	0		/* 																*/
};
PGLZ_Strategy	*PGLZ_strategy_never = &strategy_never_data;



/* ----------
 * pglz_hist_idx -
 *
 *		Computes the history table slot for the lookup by the next 4
 *		characters in the input.
 * ----------
 */
#if 1
#define pglz_hist_idx(_s,_e) (												\
			(((_e) - (_s)) < 4) ? 0 :										\
			((((_s)[0] << 9) ^ ((_s)[1] << 6) ^ 							\
			((_s)[2] << 3) ^ (_s)[3]) & (PGLZ_HISTORY_MASK))				\
		)
#else
#define pglz_hist_idx(_s,_e) (												\
			(((_e) - (_s)) < 2) ? 0 :										\
			((((_s)[0] << 8) ^ (_s)[1]) & (PGLZ_HISTORY_MASK))				\
		)
#endif


/* ----------
 * pglz_hist_add -
 *
 *		Adds a new entry to the history table.
 * ----------
 */
#define pglz_hist_add(_hs,_hn,_s,_e) {										\
			int __hindex = pglz_hist_idx((_s),(_e));						\
			(_hn)->next = (_hs)[__hindex];									\
			(_hn)->pos  = (_s);												\
			(_hs)[__hindex] = (_hn)++;										\
		}


/* ----------
 * pglz_out_ctrl -
 *
 *		Outputs the last and allocates a new control byte if needed.
 * ----------
 */
#define pglz_out_ctrl(__ctrlp,__ctrlb,__ctrl,__buf) {						\
	if ((__ctrl & 0xff) == 0)												\
	{																		\
		*__ctrlp = __ctrlb;													\
		__ctrlp = __buf++;													\
		__ctrlb = 0;														\
		__ctrl = 1;															\
	}																		\
}


/* ----------
 * pglz_out_literal -
 *
 *		Outputs a literal byte to the destination buffer including the
 *		appropriate control bit.
 * ----------
 */
#define pglz_out_literal(_ctrlp,_ctrlb,_ctrl,_buf,_byte) {					\
	pglz_out_ctrl(_ctrlp,_ctrlb,_ctrl,_buf);								\
	*_buf++ = (unsigned char)(_byte);										\
	_ctrl <<= 1;															\
}


/* ----------
 * pglz_out_tag -
 *
 *		Outputs a backward reference tag of 2-4 bytes (depending on
 *		offset and length) to the destination buffer including the
 *		appropriate control bit.
 * ----------
 */
#define pglz_out_tag(_ctrlp,_ctrlb,_ctrl,_buf,_len,_off) {					\
	pglz_out_ctrl(_ctrlp,_ctrlb,_ctrl,_buf);								\
	_ctrlb |= _ctrl;														\
	_ctrl <<= 1;															\
	if (_len > 17)															\
	{																		\
		_buf[0] = (unsigned char)((((_off) & 0xf00) >> 4) | 0x0f);			\
		_buf[1] = (unsigned char)((_off & 0xff));							\
		_buf[2] = (unsigned char)((_len) - 18);								\
		_buf += 3;															\
	} else {																\
		_buf[0] = (unsigned char)((((_off) & 0xf00) >> 4) | (_len - 3));	\
		_buf[1] = (unsigned char)((_off) & 0xff);							\
		_buf += 2;															\
	}																		\
}


/* ----------
 * pglz_find_match -
 *
 *		Lookup the history table if the actual input stream matches
 *		another sequence of characters, starting somewhere earlier
 *		in the input buffer.
 * ----------
 */
static inline int
pglz_find_match (PGLZ_HistEntry **hstart, char *input, char *end, 
						int *lenp, int *offp, int good_match, int good_drop)
{
	PGLZ_HistEntry	   *hent;
	int32				len = 0;
	int32				off = 0;
	int32				thislen;
	int32				thisoff;
	char			   *ip;
	char			   *hp;

	/* ----------
	 * Traverse the linked history list until a good enough
	 * match is found.
	 * ----------
	 */
	hent = hstart[pglz_hist_idx(input, end)];
	while (hent && len < good_match)
	{
		/* ----------
		 * Be happy with lesser good matches the more entries we visited.
		 * ----------
		 */
		good_match -= (good_match * good_drop) /100;

		/* ----------
		 * Stop if the offset does not fit into our tag anymore.
		 * ----------
		 */
		thisoff = (ip = input) - (hp = hent->pos);
		if (thisoff >= 0x0fff)
			break;

		/* ----------
		 * Determine length of match. A better match must be larger than
		 * the best so far. And if we already have a match of 16 or more
		 * bytes, it's worth the call overhead to use memcmp() to check
		 * if this match is equal for the same size. After that we must
		 * fallback to character by character comparision to know the
		 * exact position where the diff occured.
		 * ----------
		 */
		if (len >= 16)
		{
			if (memcmp(ip, hp, len) != 0)
			{
				hent = hent->next;
				continue;
			}
			thislen = len;
			ip += len;
			hp += len;
		} else {
			thislen = 0;
		}
		while (ip < end && *ip == *hp && thislen < PGLZ_MAX_MATCH)
		{
			thislen++;
			ip++;
			hp++;
		}

		/* ----------
		 * Remember this match as the best (if it is)
		 * ----------
		 */
		if (thislen > len)
		{
			len = thislen;
			off = thisoff;
		}

		/* ----------
		 * Advance to the next history entry
		 * ----------
		 */
		hent = hent->next;
	}

	/* ----------
	 * Return match information only if it results at least in one
	 * byte reduction.
	 * ----------
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
int
pglz_compress (char *source, int slen, PGLZ_Header *dest, PGLZ_Strategy *strategy)
{
	PGLZ_HistEntry	   *hist_start[PGLZ_HISTORY_SIZE];
	PGLZ_HistEntry	   *hist_alloc;
	PGLZ_HistEntry	   hist_prealloc[PGLZ_HISTORY_PREALLOC];
	PGLZ_HistEntry	   *hist_next;

	unsigned char	   *bp = ((unsigned char *)dest) + sizeof(PGLZ_Header);
	unsigned char	   *bstart = bp;
	char			   *dp = source;
	char			   *dend = source + slen;
	unsigned char		ctrl_dummy = 0;
	unsigned char	   *ctrlp = &ctrl_dummy;
	unsigned char		ctrlb = 0;
	unsigned char		ctrl = 0;
	int32				match_len;
	int32				match_off;
	int32				good_match;
	int32				good_drop;
	int32				do_compress = 1;
	int32				result_size = -1;
	int32				result_max;
	int32				need_rate;

	/* ----------
	 * Our fallback strategy is the default.
	 * ----------
	 */
	if (strategy == NULL)
		strategy = PGLZ_strategy_default;

	/* ----------
	 * Save the original source size in the header.
	 * ----------
	 */
	dest->rawsize = slen;

	/* ----------
	 * If the strategy forbids compression (at all or if source chunk too
	 * small), copy input to output without compression.
	 * ----------
	 */
	if (strategy->match_size_good == 0)
	{
		memcpy(bstart, source, slen);
		return (dest->varsize = slen + sizeof(PGLZ_Header));
	} else {
		if (slen < strategy->min_input_size)
		{
			memcpy(bstart, source, slen);
			return (dest->varsize = slen + sizeof(PGLZ_Header));
		}
	}

	/* ----------
	 * Limit the match size to the maximum implementation allowed value
	 * ----------
	 */
	if ((good_match = strategy->match_size_good) > PGLZ_MAX_MATCH)
		good_match = PGLZ_MAX_MATCH;
	if (good_match < 17)
		good_match = 17;

	if ((good_drop = strategy->match_size_drop) < 0)
		good_drop = 0;
	if (good_drop > 100)
		good_drop = 100;

	/* ----------
	 * Initialize the history tables. For inputs smaller than
	 * PGLZ_HISTORY_PREALLOC, we already have a big enough history
	 * table on the stack frame.
	 * ----------
	 */
	memset((void *)hist_start, 0, sizeof(hist_start));
	if (slen + 1 <= PGLZ_HISTORY_PREALLOC)
		hist_alloc = hist_prealloc;
	else
		hist_alloc = (PGLZ_HistEntry *)
							palloc(sizeof(PGLZ_HistEntry) * (slen + 1));
	hist_next = hist_alloc;

	/* ----------
	 * Compute the maximum result size allowed by the strategy.
	 * If the input size exceeds force_input_size, the max result size
	 * is the input size itself.
	 * Otherwise, it is the input size minus the minimum wanted
	 * compression rate.
	 * ----------
	 */
	if (slen >= strategy->force_input_size)
	{
		result_max = slen;
	} else {
		need_rate = strategy->min_comp_rate;
		if (need_rate < 0)
			need_rate = 0;
		else if (need_rate > 99)
			need_rate = 99;
		result_max = slen - ((slen * need_rate) / 100);
	}

	/* ----------
	 * Compress the source directly into the output buffer.
	 * ----------
	 */
	while (dp < dend)
	{
		/* ----------
		 * If we already exceeded the maximum result size, set no compression
		 * flag and stop this. But don't check too often.
		 * ----------
		 */
		if (bp - bstart >= result_max)
		{
			do_compress = 0;
			break;
		}

		/* ----------
		 * Try to find a match in the history
		 * ----------
		 */
		if (pglz_find_match(hist_start, dp, dend, &match_len, 
										&match_off, good_match, good_drop))
		{
			/* ----------
			 * Create the tag and add history entries for
			 * all matched characters.
			 * ----------
			 */
			pglz_out_tag(ctrlp, ctrlb, ctrl, bp, match_len, match_off);
			while(match_len--)
			{
				pglz_hist_add(hist_start, hist_next, dp, dend);
				dp++;	/* Do not do this ++ in the line above!		*/
						/* The macro would do it four times - Jan.	*/
			}
		} else {
			/* ----------
			 * No match found. Copy one literal byte.
			 * ----------
			 */
			pglz_out_literal(ctrlp, ctrlb, ctrl, bp, *dp);
			pglz_hist_add(hist_start, hist_next, dp, dend);
			dp++;	/* Do not do this ++ in the line above!		*/
					/* The macro would do it four times - Jan.	*/
		}
	}

	/* ----------
	 * Get rid of the history (if allocated)
	 * ----------
	 */
	if (hist_alloc != hist_prealloc)
		pfree((void *)hist_alloc);

	/* ----------
	 * If we are still in compressing mode, write out the last
	 * control byte and determine if the compression gained the
	 * rate requested by the strategy.
	 * ----------
	 */
	if (do_compress)
	{
		*ctrlp = ctrlb;

		result_size = bp - bstart;
		if (result_size >= result_max) {
			do_compress = 0;
		}
	}

	/* ----------
	 * Done - if we successfully compressed and matched the
	 * strategy's constraints, return the compressed result.
	 * Otherwise copy the original source over it and return
	 * the original length.
	 * ----------
	 */
	if (do_compress)
	{
		return (dest->varsize = result_size + sizeof(PGLZ_Header));
	} else {
		memcpy(((char *)dest) + sizeof(PGLZ_Header), source, slen);
		return (dest->varsize = slen + sizeof(PGLZ_Header));
	}
}


/* ----------
 * pglz_decompress -
 *
 *		Decompresses source into dest.
 * ----------
 */
int
pglz_decompress (PGLZ_Header *source, char *dest)
{
	unsigned char	   *dp;
	unsigned char	   *dend;
	unsigned char	   *bp;
	unsigned char		ctrl;
	int32				ctrlc;
	int32				len;
	int32				off;

	dp		= ((unsigned char *)source) + sizeof(PGLZ_Header);
	dend	= ((unsigned char *)source) + source->varsize;
	bp		= (unsigned char *)dest;

	if (source->varsize == source->rawsize + sizeof(PGLZ_Header))
	{
		memcpy(dest, dp, source->rawsize);
		return source->rawsize;
	}

	while (dp < dend)
	{
		/* ----------
		 * Read one control byte and process the next 8 items.
		 * ----------
		 */
		ctrl = *dp++;
		for (ctrlc = 0; ctrlc < 8 && dp < dend; ctrlc++)
		{
			if (ctrl & 1)
			{
				/* ----------
				 * Otherwise it contains the match length minus 3
				 * and the upper 4 bits of the offset. The next following
				 * byte contains the lower 8 bits of the offset. If
				 * the length is coded as 18, another extension tag byte
				 * tells how much longer the match really was (0-255).
				 * ----------
				 */
				len = (dp[0] & 0x0f) + 3;
				off = ((dp[0] & 0xf0) << 4) | dp[1];
				dp += 2;
				if (len == 18)
				{
					len += *dp++;
				}

				/* ----------
				 * Now we copy the bytes specified by the tag from
				 * OUTPUT to OUTPUT. It is dangerous and platform
				 * dependant to use memcpy() here, because the copied
				 * areas could overlap extremely!
				 * ----------
				 */
				while (len--)
				{
					*bp = bp[-off];
					bp++;
				}
			} else {
				/* ----------
				 * An unset control bit means LITERAL BYTE. So we
				 * just copy one from INPUT to OUTPUT.
				 * ----------
				 */
				*bp++ = *dp++;
			}

			/* ----------
			 * Advance the control bit
			 * ----------
			 */
			ctrl >>= 1;
		}
	}

	/* ----------
	 * That's it.
	 * ----------
	 */
	return (char *)bp - dest;
}


