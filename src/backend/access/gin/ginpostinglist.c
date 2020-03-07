/*-------------------------------------------------------------------------
 *
 * ginpostinglist.c
 *	  routines for dealing with posting lists.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginpostinglist.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin_private.h"

#ifdef USE_ASSERT_CHECKING
#define CHECK_ENCODING_ROUNDTRIP
#endif

/*
 * For encoding purposes, item pointers are represented as 64-bit unsigned
 * integers. The lowest 11 bits represent the offset number, and the next
 * lowest 32 bits are the block number. That leaves 21 bits unused, i.e.
 * only 43 low bits are used.
 *
 * 11 bits is enough for the offset number, because MaxHeapTuplesPerPage <
 * 2^11 on all supported block sizes. We are frugal with the bits, because
 * smaller integers use fewer bytes in the varbyte encoding, saving disk
 * space. (If we get a new table AM in the future that wants to use the full
 * range of possible offset numbers, we'll need to change this.)
 *
 * These 43-bit integers are encoded using varbyte encoding. In each byte,
 * the 7 low bits contain data, while the highest bit is a continuation bit.
 * When the continuation bit is set, the next byte is part of the same
 * integer, otherwise this is the last byte of this integer. 43 bits need
 * at most 7 bytes in this encoding:
 *
 * 0XXXXXXX
 * 1XXXXXXX 0XXXXYYY
 * 1XXXXXXX 1XXXXYYY 0YYYYYYY
 * 1XXXXXXX 1XXXXYYY 1YYYYYYY 0YYYYYYY
 * 1XXXXXXX 1XXXXYYY 1YYYYYYY 1YYYYYYY 0YYYYYYY
 * 1XXXXXXX 1XXXXYYY 1YYYYYYY 1YYYYYYY 1YYYYYYY 0YYYYYYY
 * 1XXXXXXX 1XXXXYYY 1YYYYYYY 1YYYYYYY 1YYYYYYY 1YYYYYYY 0uuuuuuY
 *
 * X = bits used for offset number
 * Y = bits used for block number
 * u = unused bit
 *
 * The bytes are in stored in little-endian order.
 *
 * An important property of this encoding is that removing an item from list
 * never increases the size of the resulting compressed posting list. Proof:
 *
 * Removing number is actually replacement of two numbers with their sum. We
 * have to prove that varbyte encoding of a sum can't be longer than varbyte
 * encoding of its summands. Sum of two numbers is at most one bit wider than
 * the larger of the summands. Widening a number by one bit enlarges its length
 * in varbyte encoding by at most one byte. Therefore, varbyte encoding of sum
 * is at most one byte longer than varbyte encoding of larger summand. Lesser
 * summand is at least one byte, so the sum cannot take more space than the
 * summands, Q.E.D.
 *
 * This property greatly simplifies VACUUM, which can assume that posting
 * lists always fit on the same page after vacuuming. Note that even though
 * that holds for removing items from a posting list, you must also be
 * careful to not cause expansion e.g. when merging uncompressed items on the
 * page into the compressed lists, when vacuuming.
 */

/*
 * How many bits do you need to encode offset number? OffsetNumber is a 16-bit
 * integer, but you can't fit that many items on a page. 11 ought to be more
 * than enough. It's tempting to derive this from MaxHeapTuplesPerPage, and
 * use the minimum number of bits, but that would require changing the on-disk
 * format if MaxHeapTuplesPerPage changes. Better to leave some slack.
 */
#define MaxHeapTuplesPerPageBits		11

/* Max. number of bytes needed to encode the largest supported integer. */
#define MaxBytesPerInteger				7

static inline uint64
itemptr_to_uint64(const ItemPointer iptr)
{
	uint64		val;

	Assert(ItemPointerIsValid(iptr));
	Assert(GinItemPointerGetOffsetNumber(iptr) < (1 << MaxHeapTuplesPerPageBits));

	val = GinItemPointerGetBlockNumber(iptr);
	val <<= MaxHeapTuplesPerPageBits;
	val |= GinItemPointerGetOffsetNumber(iptr);

	return val;
}

static inline void
uint64_to_itemptr(uint64 val, ItemPointer iptr)
{
	GinItemPointerSetOffsetNumber(iptr, val & ((1 << MaxHeapTuplesPerPageBits) - 1));
	val = val >> MaxHeapTuplesPerPageBits;
	GinItemPointerSetBlockNumber(iptr, val);

	Assert(ItemPointerIsValid(iptr));
}

/*
 * Varbyte-encode 'val' into *ptr. *ptr is incremented to next integer.
 */
static void
encode_varbyte(uint64 val, unsigned char **ptr)
{
	unsigned char *p = *ptr;

	while (val > 0x7F)
	{
		*(p++) = 0x80 | (val & 0x7F);
		val >>= 7;
	}
	*(p++) = (unsigned char) val;

	*ptr = p;
}

/*
 * Decode varbyte-encoded integer at *ptr. *ptr is incremented to next integer.
 */
static uint64
decode_varbyte(unsigned char **ptr)
{
	uint64		val;
	unsigned char *p = *ptr;
	uint64		c;

	/* 1st byte */
	c = *(p++);
	val = c & 0x7F;
	if (c & 0x80)
	{
		/* 2nd byte */
		c = *(p++);
		val |= (c & 0x7F) << 7;
		if (c & 0x80)
		{
			/* 3rd byte */
			c = *(p++);
			val |= (c & 0x7F) << 14;
			if (c & 0x80)
			{
				/* 4th byte */
				c = *(p++);
				val |= (c & 0x7F) << 21;
				if (c & 0x80)
				{
					/* 5th byte */
					c = *(p++);
					val |= (c & 0x7F) << 28;
					if (c & 0x80)
					{
						/* 6th byte */
						c = *(p++);
						val |= (c & 0x7F) << 35;
						if (c & 0x80)
						{
							/* 7th byte, should not have continuation bit */
							c = *(p++);
							val |= c << 42;
							Assert((c & 0x80) == 0);
						}
					}
				}
			}
		}
	}

	*ptr = p;

	return val;
}

/*
 * Encode a posting list.
 *
 * The encoded list is returned in a palloc'd struct, which will be at most
 * 'maxsize' bytes in size.  The number items in the returned segment is
 * returned in *nwritten. If it's not equal to nipd, not all the items fit
 * in 'maxsize', and only the first *nwritten were encoded.
 *
 * The allocated size of the returned struct is short-aligned, and the padding
 * byte at the end, if any, is zero.
 */
GinPostingList *
ginCompressPostingList(const ItemPointer ipd, int nipd, int maxsize,
					   int *nwritten)
{
	uint64		prev;
	int			totalpacked = 0;
	int			maxbytes;
	GinPostingList *result;
	unsigned char *ptr;
	unsigned char *endptr;

	maxsize = SHORTALIGN_DOWN(maxsize);

	result = palloc(maxsize);

	maxbytes = maxsize - offsetof(GinPostingList, bytes);
	Assert(maxbytes > 0);

	/* Store the first special item */
	result->first = ipd[0];

	prev = itemptr_to_uint64(&result->first);

	ptr = result->bytes;
	endptr = result->bytes + maxbytes;
	for (totalpacked = 1; totalpacked < nipd; totalpacked++)
	{
		uint64		val = itemptr_to_uint64(&ipd[totalpacked]);
		uint64		delta = val - prev;

		Assert(val > prev);

		if (endptr - ptr >= MaxBytesPerInteger)
			encode_varbyte(delta, &ptr);
		else
		{
			/*
			 * There are less than 7 bytes left. Have to check if the next
			 * item fits in that space before writing it out.
			 */
			unsigned char buf[MaxBytesPerInteger];
			unsigned char *p = buf;

			encode_varbyte(delta, &p);
			if (p - buf > (endptr - ptr))
				break;			/* output is full */

			memcpy(ptr, buf, p - buf);
			ptr += (p - buf);
		}
		prev = val;
	}
	result->nbytes = ptr - result->bytes;

	/*
	 * If we wrote an odd number of bytes, zero out the padding byte at the
	 * end.
	 */
	if (result->nbytes != SHORTALIGN(result->nbytes))
		result->bytes[result->nbytes] = 0;

	if (nwritten)
		*nwritten = totalpacked;

	Assert(SizeOfGinPostingList(result) <= maxsize);

	/*
	 * Check that the encoded segment decodes back to the original items.
	 */
#if defined (CHECK_ENCODING_ROUNDTRIP)
	{
		int			ndecoded;
		ItemPointer tmp = ginPostingListDecode(result, &ndecoded);

		Assert(ndecoded == totalpacked);
		Assert(memcmp(tmp, ipd, ndecoded * sizeof(ItemPointerData)) == 0);
		pfree(tmp);
	}
#endif

	return result;
}

/*
 * Decode a compressed posting list into an array of item pointers.
 * The number of items is returned in *ndecoded.
 */
ItemPointer
ginPostingListDecode(GinPostingList *plist, int *ndecoded)
{
	return ginPostingListDecodeAllSegments(plist,
										   SizeOfGinPostingList(plist),
										   ndecoded);
}

/*
 * Decode multiple posting list segments into an array of item pointers.
 * The number of items is returned in *ndecoded_out. The segments are stored
 * one after each other, with total size 'len' bytes.
 */
ItemPointer
ginPostingListDecodeAllSegments(GinPostingList *segment, int len, int *ndecoded_out)
{
	ItemPointer result;
	int			nallocated;
	uint64		val;
	char	   *endseg = ((char *) segment) + len;
	int			ndecoded;
	unsigned char *ptr;
	unsigned char *endptr;

	/*
	 * Guess an initial size of the array.
	 */
	nallocated = segment->nbytes * 2 + 1;
	result = palloc(nallocated * sizeof(ItemPointerData));

	ndecoded = 0;
	while ((char *) segment < endseg)
	{
		/* enlarge output array if needed */
		if (ndecoded >= nallocated)
		{
			nallocated *= 2;
			result = repalloc(result, nallocated * sizeof(ItemPointerData));
		}

		/* copy the first item */
		Assert(OffsetNumberIsValid(ItemPointerGetOffsetNumber(&segment->first)));
		Assert(ndecoded == 0 || ginCompareItemPointers(&segment->first, &result[ndecoded - 1]) > 0);
		result[ndecoded] = segment->first;
		ndecoded++;

		val = itemptr_to_uint64(&segment->first);
		ptr = segment->bytes;
		endptr = segment->bytes + segment->nbytes;
		while (ptr < endptr)
		{
			/* enlarge output array if needed */
			if (ndecoded >= nallocated)
			{
				nallocated *= 2;
				result = repalloc(result, nallocated * sizeof(ItemPointerData));
			}

			val += decode_varbyte(&ptr);

			uint64_to_itemptr(val, &result[ndecoded]);
			ndecoded++;
		}
		segment = GinNextPostingListSegment(segment);
	}

	if (ndecoded_out)
		*ndecoded_out = ndecoded;
	return result;
}

/*
 * Add all item pointers from a bunch of posting lists to a TIDBitmap.
 */
int
ginPostingListDecodeAllSegmentsToTbm(GinPostingList *ptr, int len,
									 TIDBitmap *tbm)
{
	int			ndecoded;
	ItemPointer items;

	items = ginPostingListDecodeAllSegments(ptr, len, &ndecoded);
	tbm_add_tuples(tbm, items, ndecoded, false);
	pfree(items);

	return ndecoded;
}

/*
 * Merge two ordered arrays of itempointers, eliminating any duplicates.
 *
 * Returns a palloc'd array, and *nmerged is set to the number of items in
 * the result, after eliminating duplicates.
 */
ItemPointer
ginMergeItemPointers(ItemPointerData *a, uint32 na,
					 ItemPointerData *b, uint32 nb,
					 int *nmerged)
{
	ItemPointerData *dst;

	dst = (ItemPointer) palloc((na + nb) * sizeof(ItemPointerData));

	/*
	 * If the argument arrays don't overlap, we can just append them to each
	 * other.
	 */
	if (na == 0 || nb == 0 || ginCompareItemPointers(&a[na - 1], &b[0]) < 0)
	{
		memcpy(dst, a, na * sizeof(ItemPointerData));
		memcpy(&dst[na], b, nb * sizeof(ItemPointerData));
		*nmerged = na + nb;
	}
	else if (ginCompareItemPointers(&b[nb - 1], &a[0]) < 0)
	{
		memcpy(dst, b, nb * sizeof(ItemPointerData));
		memcpy(&dst[nb], a, na * sizeof(ItemPointerData));
		*nmerged = na + nb;
	}
	else
	{
		ItemPointerData *dptr = dst;
		ItemPointerData *aptr = a;
		ItemPointerData *bptr = b;

		while (aptr - a < na && bptr - b < nb)
		{
			int			cmp = ginCompareItemPointers(aptr, bptr);

			if (cmp > 0)
				*dptr++ = *bptr++;
			else if (cmp == 0)
			{
				/* only keep one copy of the identical items */
				*dptr++ = *bptr++;
				aptr++;
			}
			else
				*dptr++ = *aptr++;
		}

		while (aptr - a < na)
			*dptr++ = *aptr++;

		while (bptr - b < nb)
			*dptr++ = *bptr++;

		*nmerged = dptr - dst;
	}

	return dst;
}
