/*-------------------------------------------------------------------------
 *
 * ginbulk.c
 *	  routines for fast build of inverted index
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gin/ginbulk.c,v 1.14 2009/01/01 17:23:34 momjian Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin.h"
#include "utils/datum.h"
#include "utils/memutils.h"


#define DEF_NENTRY	2048
#define DEF_NPTR	4

void
ginInitBA(BuildAccumulator *accum)
{
	accum->maxdepth = 1;
	accum->stackpos = 0;
	accum->entries = NULL;
	accum->stack = NULL;
	accum->allocatedMemory = 0;
	accum->entryallocator = NULL;
}

static EntryAccumulator *
EAAllocate(BuildAccumulator *accum)
{
	if (accum->entryallocator == NULL || accum->length >= DEF_NENTRY)
	{
		accum->entryallocator = palloc(sizeof(EntryAccumulator) * DEF_NENTRY);
		accum->allocatedMemory += GetMemoryChunkSpace(accum->entryallocator);
		accum->length = 0;
	}

	accum->length++;
	return accum->entryallocator + accum->length - 1;
}

/*
 * Stores heap item pointer. For robust, it checks that
 * item pointer are ordered
 */
static void
ginInsertData(BuildAccumulator *accum, EntryAccumulator *entry, ItemPointer heapptr)
{
	if (entry->number >= entry->length)
	{
		accum->allocatedMemory -= GetMemoryChunkSpace(entry->list);
		entry->length *= 2;
		entry->list = (ItemPointerData *) repalloc(entry->list,
									sizeof(ItemPointerData) * entry->length);
		accum->allocatedMemory += GetMemoryChunkSpace(entry->list);
	}

	if (entry->shouldSort == FALSE)
	{
		int			res = compareItemPointers(entry->list + entry->number - 1, heapptr);

		Assert(res != 0);

		if (res > 0)
			entry->shouldSort = TRUE;
	}

	entry->list[entry->number] = *heapptr;
	entry->number++;
}

/*
 * This is basically the same as datumCopy(), but modified to count
 * palloc'd space in accum.
 */
static Datum
getDatumCopy(BuildAccumulator *accum, OffsetNumber attnum, Datum value)
{
	Form_pg_attribute att = accum->ginstate->origTupdesc->attrs[ attnum - 1 ];
	Datum		res;

	if (att->attbyval)
		res = value;
	else
	{
		res = datumCopy(value, false, att->attlen);
		accum->allocatedMemory += GetMemoryChunkSpace(DatumGetPointer(res));
	}
	return res;
}

/*
 * Find/store one entry from indexed value.
 */
static void
ginInsertEntry(BuildAccumulator *accum, ItemPointer heapptr, OffsetNumber attnum, Datum entry)
{
	EntryAccumulator *ea = accum->entries,
			   *pea = NULL;
	int			res = 0;
	uint32		depth = 1;

	while (ea)
	{
		res = compareAttEntries(accum->ginstate, attnum, entry, ea->attnum, ea->value);
		if (res == 0)
			break;				/* found */
		else
		{
			pea = ea;
			if (res < 0)
				ea = ea->left;
			else
				ea = ea->right;
		}
		depth++;
	}

	if (depth > accum->maxdepth)
		accum->maxdepth = depth;

	if (ea == NULL)
	{
		ea = EAAllocate(accum);

		ea->left = ea->right = NULL;
		ea->attnum = attnum;
		ea->value = getDatumCopy(accum, attnum, entry);
		ea->length = DEF_NPTR;
		ea->number = 1;
		ea->shouldSort = FALSE;
		ea->list = (ItemPointerData *) palloc(sizeof(ItemPointerData) * DEF_NPTR);
		accum->allocatedMemory += GetMemoryChunkSpace(ea->list);
		ea->list[0] = *heapptr;

		if (pea == NULL)
			accum->entries = ea;
		else
		{
			Assert(res != 0);
			if (res < 0)
				pea->left = ea;
			else
				pea->right = ea;
		}
	}
	else
		ginInsertData(accum, ea, heapptr);
}

/*
 * insert middle of left part the middle of right one,
 * then calls itself for each parts
 */
static void
ginChooseElem(BuildAccumulator *accum, ItemPointer heapptr, OffsetNumber attnum, 
										Datum *entries, uint32 nentry,
			  uint32 low, uint32 high, uint32 offset)
{
	uint32		pos;
	uint32		middle = (low + high) >> 1;

	pos = (low + middle) >> 1;
	if (low != middle && pos >= offset && pos - offset < nentry)
		ginInsertEntry(accum, heapptr, attnum, entries[pos - offset]);
	pos = (high + middle + 1) >> 1;
	if (middle + 1 != high && pos >= offset && pos - offset < nentry)
		ginInsertEntry(accum, heapptr, attnum, entries[pos - offset]);

	if (low != middle)
		ginChooseElem(accum, heapptr, attnum, entries, nentry, low, middle, offset);
	if (high != middle + 1)
		ginChooseElem(accum, heapptr, attnum, entries, nentry, middle + 1, high, offset);
}

/*
 * Insert one heap pointer. Suppose entries is sorted.
 * Insertion order tries to get binary tree balanced: first insert middle value,
 * next middle on left part and middle of right part.
 */
void
ginInsertRecordBA(BuildAccumulator *accum, ItemPointer heapptr, OffsetNumber attnum, 
						Datum *entries, int32 nentry)
{
	uint32		i,
				nbit = 0,
				offset;

	if (nentry <= 0)
		return;

	i = nentry - 1;
	for (; i > 0; i >>= 1)
		nbit++;

	nbit = 1 << nbit;
	offset = (nbit - nentry) / 2;

	ginInsertEntry(accum, heapptr, attnum, entries[(nbit >> 1) - offset]);
	ginChooseElem(accum, heapptr, attnum, entries, nentry, 0, nbit, offset);
}

static int
qsortCompareItemPointers(const void *a, const void *b)
{
	int			res = compareItemPointers((ItemPointer) a, (ItemPointer) b);

	Assert(res != 0);
	return res;
}

/*
 * walk on binary tree and returns ordered nodes
 */
static EntryAccumulator *
walkTree(BuildAccumulator *accum)
{
	EntryAccumulator *entry = accum->stack[accum->stackpos];

	if (entry->list != NULL)
	{
		/* return entry itself: we already was at left sublink */
		return entry;
	}
	else if (entry->right && entry->right != accum->stack[accum->stackpos + 1])
	{
		/* go on right sublink */
		accum->stackpos++;
		entry = entry->right;

		/* find most-left value */
		for (;;)
		{
			accum->stack[accum->stackpos] = entry;
			if (entry->left)
			{
				accum->stackpos++;
				entry = entry->left;
			}
			else
				break;
		}
	}
	else
	{
		/* we already return all left subtree, itself and  right subtree */
		if (accum->stackpos == 0)
			return 0;
		accum->stackpos--;
		return walkTree(accum);
	}

	return entry;
}

ItemPointerData *
ginGetEntry(BuildAccumulator *accum, OffsetNumber *attnum, Datum *value, uint32 *n)
{
	EntryAccumulator *entry;
	ItemPointerData *list;

	if (accum->stack == NULL)
	{
		/* first call */
		accum->stack = palloc0(sizeof(EntryAccumulator *) * (accum->maxdepth + 1));
		accum->allocatedMemory += GetMemoryChunkSpace(accum->stack);
		entry = accum->entries;

		if (entry == NULL)
			return NULL;

		/* find most-left value */
		for (;;)
		{
			accum->stack[accum->stackpos] = entry;
			if (entry->left)
			{
				accum->stackpos++;
				entry = entry->left;
			}
			else
				break;
		}
	}
	else
	{
		accum->allocatedMemory -= GetMemoryChunkSpace(accum->stack[accum->stackpos]->list);
		pfree(accum->stack[accum->stackpos]->list);
		accum->stack[accum->stackpos]->list = NULL;
		entry = walkTree(accum);
	}

	if (entry == NULL)
		return NULL;

	*n = entry->number;
	*attnum = entry->attnum;
	*value = entry->value;
	list = entry->list;

	Assert(list != NULL);

	if (entry->shouldSort && entry->number > 1)
		qsort(list, *n, sizeof(ItemPointerData), qsortCompareItemPointers);

	return list;
}
