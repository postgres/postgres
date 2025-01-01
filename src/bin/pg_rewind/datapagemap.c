/*-------------------------------------------------------------------------
 *
 * datapagemap.c
 *	  A data structure for keeping track of data pages that have changed.
 *
 * This is a fairly simple bitmap.
 *
 * Copyright (c) 2013-2025, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/logging.h"
#include "datapagemap.h"

struct datapagemap_iterator
{
	datapagemap_t *map;
	BlockNumber nextblkno;
};

/*****
 * Public functions
 */

/*
 * Add a block to the bitmap.
 */
void
datapagemap_add(datapagemap_t *map, BlockNumber blkno)
{
	int			offset;
	int			bitno;

	offset = blkno / 8;
	bitno = blkno % 8;

	/* enlarge or create bitmap if needed */
	if (map->bitmapsize <= offset)
	{
		int			oldsize = map->bitmapsize;
		int			newsize;

		/*
		 * The minimum to hold the new bit is offset + 1. But add some
		 * headroom, so that we don't need to repeatedly enlarge the bitmap in
		 * the common case that blocks are modified in order, from beginning
		 * of a relation to the end.
		 */
		newsize = offset + 1;
		newsize += 10;

		map->bitmap = pg_realloc(map->bitmap, newsize);

		/* zero out the newly allocated region */
		memset(&map->bitmap[oldsize], 0, newsize - oldsize);

		map->bitmapsize = newsize;
	}

	/* Set the bit */
	map->bitmap[offset] |= (1 << bitno);
}

/*
 * Start iterating through all entries in the page map.
 *
 * After datapagemap_iterate, call datapagemap_next to return the entries,
 * until it returns false. After you're done, use pg_free() to destroy the
 * iterator.
 */
datapagemap_iterator_t *
datapagemap_iterate(datapagemap_t *map)
{
	datapagemap_iterator_t *iter;

	iter = pg_malloc(sizeof(datapagemap_iterator_t));
	iter->map = map;
	iter->nextblkno = 0;

	return iter;
}

bool
datapagemap_next(datapagemap_iterator_t *iter, BlockNumber *blkno)
{
	datapagemap_t *map = iter->map;

	for (;;)
	{
		BlockNumber blk = iter->nextblkno;
		int			nextoff = blk / 8;
		int			bitno = blk % 8;

		if (nextoff >= map->bitmapsize)
			break;

		iter->nextblkno++;

		if (map->bitmap[nextoff] & (1 << bitno))
		{
			*blkno = blk;
			return true;
		}
	}

	/* no more set bits in this bitmap. */
	return false;
}

/*
 * A debugging aid. Prints out the contents of the page map.
 */
void
datapagemap_print(datapagemap_t *map)
{
	datapagemap_iterator_t *iter;
	BlockNumber blocknum;

	iter = datapagemap_iterate(map);
	while (datapagemap_next(iter, &blocknum))
		pg_log_debug("block %u", blocknum);

	pg_free(iter);
}
