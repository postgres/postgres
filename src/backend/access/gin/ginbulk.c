/*-------------------------------------------------------------------------
 *
 * ginbulk.c
 *    routines for fast build of inverted index 
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *          $PostgreSQL: pgsql/src/backend/access/gin/ginbulk.c,v 1.1 2006/05/02 11:28:54 teodor Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/genam.h"
#include "access/gin.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "utils/memutils.h"
#include "access/tuptoaster.h"

#define DEF_NENTRY	128
#define DEF_NPTR	4

void
ginInitBA(BuildAccumulator *accum) {
	
		accum->number = 0;
		accum->curget = 0;
		accum->length = DEF_NENTRY;
		accum->entries = (EntryAccumulator*)palloc0( sizeof(EntryAccumulator) * DEF_NENTRY );
		accum->allocatedMemory = sizeof(EntryAccumulator) * DEF_NENTRY;
}

/*
 * Stores heap item pointer. For robust, it checks that
 * item pointer are ordered
 */
static void
ginInsertData(BuildAccumulator *accum, EntryAccumulator *entry, ItemPointer heapptr) {
	if ( entry->number >= entry->length ) {
		accum->allocatedMemory += sizeof(ItemPointerData) * entry->length;
		entry->length *= 2;
		entry->list    = (ItemPointerData*)repalloc(entry->list,
											sizeof(ItemPointerData)*entry->length);
	}

	if ( entry->shouldSort==FALSE ) {  
		int res = compareItemPointers( entry->list + entry->number - 1, heapptr );

		Assert( res != 0 );

		if ( res > 0 )
			entry->shouldSort=TRUE;
	}

	entry->list[ entry->number ] = *heapptr;
	entry->number++;
}

/*
 * Find/store one entry from indexed value.
 * It supposes, that entry should be located between low and end of array of
 * entries. Returns position of found/inserted entry
 */
static uint32
ginInsertEntry(BuildAccumulator *accum, ItemPointer heapptr,  Datum entry, uint32 low) {
	uint32 high = accum->number, mid;
	int res;

	while(high>low) { 
		mid = low + ((high - low) / 2);

		res = compareEntries(accum->ginstate, entry, accum->entries[mid].value);

		if ( res == 0 ) {
			ginInsertData( accum, accum->entries+mid, heapptr );
			return mid;
		} else if ( res > 0 )
			low = mid + 1;
		else
			high = mid;
	}
	
	/* did not find an entry, insert */
	if ( accum->number >= accum->length ) {
		accum->allocatedMemory += sizeof(EntryAccumulator) * accum->length;
		accum->length *= 2;
		accum->entries = (EntryAccumulator*)repalloc( accum->entries, 
				sizeof(EntryAccumulator) * accum->length );
	}

	if ( high != accum->number ) 
		memmove( accum->entries+high+1, accum->entries+high, sizeof(EntryAccumulator) * (accum->number-high) );

	accum->entries[high].value  = entry;
	accum->entries[high].length = DEF_NPTR;
	accum->entries[high].number = 1;
	accum->entries[high].shouldSort = FALSE;
	accum->entries[high].list   = (ItemPointerData*)palloc(sizeof(ItemPointerData)*DEF_NPTR);
	accum->entries[high].list[0] = *heapptr;

	accum->allocatedMemory += sizeof(ItemPointerData)*DEF_NPTR;
	accum->number++;

	return high;
}


/*
 * Insert one heap pointer. Requires entries to be sorted!
 */
void
ginInsertRecordBA( BuildAccumulator *accum, ItemPointer heapptr, Datum *entries, uint32 nentry ) {
	uint32 start=0,i;

	for(i=0;i<nentry;i++)
		start = ginInsertEntry( accum, heapptr, entries[i], start);
}

static int 
qsortCompareItemPointers( const void *a, const void *b ) {
	int res = compareItemPointers( (ItemPointer)a, (ItemPointer)b );
	Assert( res!=0 );
	return res;
}

ItemPointerData*
ginGetEntry(BuildAccumulator *accum, Datum *value, uint32 *n) {
	EntryAccumulator	*entry;

	ItemPointerData *list;
	if ( accum->curget >= accum->number )
		return NULL;
	else if ( accum->curget > 0 )
		pfree( accum->entries[ accum->curget-1 ].list );

	entry = accum->entries + accum->curget;
	*n 		= entry->number;
	*value 	= entry->value;
	list 	= entry->list;
	accum->curget++;

	if ( entry->shouldSort && entry->number > 1 )
		qsort(list, *n, sizeof(ItemPointerData), qsortCompareItemPointers);


	return list;
}



