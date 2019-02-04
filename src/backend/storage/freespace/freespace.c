/*-------------------------------------------------------------------------
 *
 * freespace.c
 *	  POSTGRES free space map for quickly finding free space in relations
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/freespace/freespace.c
 *
 *
 * NOTES:
 *
 *	Free Space Map keeps track of the amount of free space on pages, and
 *	allows quickly searching for a page with enough free space. The FSM is
 *	stored in a dedicated relation fork of all heap relations, and those
 *	index access methods that need it (see also indexfsm.c). See README for
 *	more information.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "storage/fsm_internals.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"


/*
 * We use just one byte to store the amount of free space on a page, so we
 * divide the amount of free space a page can have into 256 different
 * categories. The highest category, 255, represents a page with at least
 * MaxFSMRequestSize bytes of free space, and the second highest category
 * represents the range from 254 * FSM_CAT_STEP, inclusive, to
 * MaxFSMRequestSize, exclusive.
 *
 * MaxFSMRequestSize depends on the architecture and BLCKSZ, but assuming
 * default 8k BLCKSZ, and that MaxFSMRequestSize is 8164 bytes, the
 * categories look like this:
 *
 *
 * Range	 Category
 * 0	- 31   0
 * 32	- 63   1
 * ...    ...  ...
 * 8096 - 8127 253
 * 8128 - 8163 254
 * 8164 - 8192 255
 *
 * The reason that MaxFSMRequestSize is special is that if MaxFSMRequestSize
 * isn't equal to a range boundary, a page with exactly MaxFSMRequestSize
 * bytes of free space wouldn't satisfy a request for MaxFSMRequestSize
 * bytes. If there isn't more than MaxFSMRequestSize bytes of free space on a
 * completely empty page, that would mean that we could never satisfy a
 * request of exactly MaxFSMRequestSize bytes.
 */
#define FSM_CATEGORIES	256
#define FSM_CAT_STEP	(BLCKSZ / FSM_CATEGORIES)
#define MaxFSMRequestSize	MaxHeapTupleSize

/*
 * Depth of the on-disk tree. We need to be able to address 2^32-1 blocks,
 * and 1626 is the smallest number that satisfies X^3 >= 2^32-1. Likewise,
 * 216 is the smallest number that satisfies X^4 >= 2^32-1. In practice,
 * this means that 4096 bytes is the smallest BLCKSZ that we can get away
 * with a 3-level tree, and 512 is the smallest we support.
 */
#define FSM_TREE_DEPTH	((SlotsPerFSMPage >= 1626) ? 3 : 4)

#define FSM_ROOT_LEVEL	(FSM_TREE_DEPTH - 1)
#define FSM_BOTTOM_LEVEL 0

/* Status codes for the local map. */

/* Either already tried, or beyond the end of the relation */
#define FSM_LOCAL_NOT_AVAIL 0x00

/* Available to try */
#define FSM_LOCAL_AVAIL		0x01

/*
 * The internal FSM routines work on a logical addressing scheme. Each
 * level of the tree can be thought of as a separately addressable file.
 */
typedef struct
{
	int			level;			/* level */
	int			logpageno;		/* page number within the level */
} FSMAddress;

/* Address of the root page. */
static const FSMAddress FSM_ROOT_ADDRESS = {FSM_ROOT_LEVEL, 0};

/* Local map of block numbers for small heaps with no FSM. */
typedef struct
{
	BlockNumber nblocks;
	uint8		map[HEAP_FSM_CREATION_THRESHOLD];
}			FSMLocalMap;

static FSMLocalMap fsm_local_map =
{
	0,
	{
		FSM_LOCAL_NOT_AVAIL
	}
};

#define FSM_LOCAL_MAP_EXISTS (fsm_local_map.nblocks > 0)

/* functions to navigate the tree */
static FSMAddress fsm_get_child(FSMAddress parent, uint16 slot);
static FSMAddress fsm_get_parent(FSMAddress child, uint16 *slot);
static FSMAddress fsm_get_location(BlockNumber heapblk, uint16 *slot);
static BlockNumber fsm_get_heap_blk(FSMAddress addr, uint16 slot);
static BlockNumber fsm_logical_to_physical(FSMAddress addr);

static Buffer fsm_readbuf(Relation rel, FSMAddress addr, bool extend);
static void fsm_extend(Relation rel, BlockNumber fsm_nblocks);

/* functions to convert amount of free space to a FSM category */
static uint8 fsm_space_avail_to_cat(Size avail);
static uint8 fsm_space_needed_to_cat(Size needed);
static Size fsm_space_cat_to_avail(uint8 cat);

/* workhorse functions for various operations */
static int fsm_set_and_search(Relation rel, FSMAddress addr, uint16 slot,
				   uint8 newValue, uint8 minValue);
static void fsm_local_set(Relation rel, BlockNumber cur_nblocks);
static BlockNumber fsm_search(Relation rel, uint8 min_cat);
static BlockNumber fsm_local_search(void);
static uint8 fsm_vacuum_page(Relation rel, FSMAddress addr,
				BlockNumber start, BlockNumber end,
				bool *eof);
static bool fsm_allow_writes(Relation rel, BlockNumber heapblk,
				 BlockNumber nblocks, BlockNumber *get_nblocks);


/******** Public API ********/

/*
 * GetPageWithFreeSpace - try to find a page in the given relation with
 *		at least the specified amount of free space.
 *
 * If successful, return the block number; if not, return InvalidBlockNumber.
 *
 * The caller must be prepared for the possibility that the returned page
 * will turn out to have too little space available by the time the caller
 * gets a lock on it.  In that case, the caller should report the actual
 * amount of free space available on that page and then try again (see
 * RecordAndGetPageWithFreeSpace).  If InvalidBlockNumber is returned,
 * extend the relation.
 *
 * For very small heap relations that don't have a FSM, we try every other
 * page before extending the relation.  To keep track of which pages have
 * been tried, initialize a local in-memory map of pages.
 */
BlockNumber
GetPageWithFreeSpace(Relation rel, Size spaceNeeded, bool check_fsm_only)
{
	uint8		min_cat = fsm_space_needed_to_cat(spaceNeeded);
	BlockNumber target_block,
				nblocks;

	/* First try the FSM, if it exists. */
	target_block = fsm_search(rel, min_cat);

	if (target_block == InvalidBlockNumber &&
		(rel->rd_rel->relkind == RELKIND_RELATION ||
		 rel->rd_rel->relkind == RELKIND_TOASTVALUE) &&
		!check_fsm_only)
	{
		nblocks = RelationGetNumberOfBlocks(rel);

		if (nblocks > HEAP_FSM_CREATION_THRESHOLD)
		{
			/*
			 * If the FSM knows nothing of the rel, try the last page before
			 * we give up and extend.  This avoids one-tuple-per-page syndrome
			 * during bootstrapping or in a recently-started system.
			 */
			target_block = nblocks - 1;
		}
		else if (nblocks > 0)
		{
			/* Create or update local map and get first candidate block. */
			fsm_local_set(rel, nblocks);
			target_block = fsm_local_search();
		}
	}

	return target_block;
}

/*
 * RecordAndGetPageWithFreeSpace - update info about a page and try again.
 *
 * We provide this combo form to save some locking overhead, compared to
 * separate RecordPageWithFreeSpace + GetPageWithFreeSpace calls. There's
 * also some effort to return a page close to the old page; if there's a
 * page with enough free space on the same FSM page where the old one page
 * is located, it is preferred.
 *
 * For very small heap relations that don't have a FSM, we update the local
 * map to indicate we have tried a page, and return the next page to try.
 */
BlockNumber
RecordAndGetPageWithFreeSpace(Relation rel, BlockNumber oldPage,
							  Size oldSpaceAvail, Size spaceNeeded)
{
	int			old_cat;
	int			search_cat;
	FSMAddress	addr;
	uint16		slot;
	int			search_slot;
	BlockNumber nblocks = InvalidBlockNumber;

	/* First try the local map, if it exists. */
	if (FSM_LOCAL_MAP_EXISTS)
	{
		Assert((rel->rd_rel->relkind == RELKIND_RELATION ||
				rel->rd_rel->relkind == RELKIND_TOASTVALUE) &&
			   fsm_local_map.map[oldPage] == FSM_LOCAL_AVAIL);

		fsm_local_map.map[oldPage] = FSM_LOCAL_NOT_AVAIL;
		return fsm_local_search();
	}

	if (!fsm_allow_writes(rel, oldPage, InvalidBlockNumber, &nblocks))
	{
		/*
		 * If we have neither a local map nor a FSM, we probably just tried
		 * the target block in the smgr relation entry and failed, so we'll
		 * need to create the local map.
		 */
		fsm_local_set(rel, nblocks);
		return fsm_local_search();
	}

	/* Normal FSM logic follows */

	old_cat = fsm_space_avail_to_cat(oldSpaceAvail);
	search_cat = fsm_space_needed_to_cat(spaceNeeded);

	/* Get the location of the FSM byte representing the heap block */
	addr = fsm_get_location(oldPage, &slot);

	search_slot = fsm_set_and_search(rel, addr, slot, old_cat, search_cat);

	/*
	 * If fsm_set_and_search found a suitable new block, return that.
	 * Otherwise, search as usual.
	 */
	if (search_slot != -1)
		return fsm_get_heap_blk(addr, search_slot);
	else
		return fsm_search(rel, search_cat);
}

/*
 * RecordPageWithFreeSpace - update info about a page.
 *
 * Note that if the new spaceAvail value is higher than the old value stored
 * in the FSM, the space might not become visible to searchers until the next
 * FreeSpaceMapVacuum call, which updates the upper level pages.
 *
 * Callers have no need for a local map.
 */
void
RecordPageWithFreeSpace(Relation rel, BlockNumber heapBlk,
						Size spaceAvail, BlockNumber nblocks)
{
	int			new_cat;
	FSMAddress	addr;
	uint16		slot;
	BlockNumber dummy;

	if (!fsm_allow_writes(rel, heapBlk, nblocks, &dummy))
		/* No FSM to update and no local map either */
		return;

	/* Get the location of the FSM byte representing the heap block */
	addr = fsm_get_location(heapBlk, &slot);

	new_cat = fsm_space_avail_to_cat(spaceAvail);
	fsm_set_and_search(rel, addr, slot, new_cat, 0);
}

/*
 * Clear the local map.  We must call this when we have found a block with
 * enough free space, when we extend the relation, or on transaction abort.
 */
void
FSMClearLocalMap(void)
{
	if (FSM_LOCAL_MAP_EXISTS)
	{
		fsm_local_map.nblocks = 0;
		memset(&fsm_local_map.map, FSM_LOCAL_NOT_AVAIL,
			   sizeof(fsm_local_map.map));
	}
}

/*
 * XLogRecordPageWithFreeSpace - like RecordPageWithFreeSpace, for use in
 *		WAL replay
 */
void
XLogRecordPageWithFreeSpace(RelFileNode rnode, BlockNumber heapBlk,
							Size spaceAvail)
{
	int			new_cat = fsm_space_avail_to_cat(spaceAvail);
	FSMAddress	addr;
	uint16		slot;
	BlockNumber blkno;
	Buffer		buf;
	Page		page;
	bool		write_to_fsm;

	/* This is meant to mirror the logic in fsm_allow_writes() */
	if (heapBlk >= HEAP_FSM_CREATION_THRESHOLD)
		write_to_fsm = true;
	else
	{
		/* Open the relation at smgr level */
		SMgrRelation smgr = smgropen(rnode, InvalidBackendId);

		if (smgrexists(smgr, FSM_FORKNUM))
			write_to_fsm = true;
		else
		{
			BlockNumber heap_nblocks = smgrnblocks(smgr, MAIN_FORKNUM);

			if (heap_nblocks > HEAP_FSM_CREATION_THRESHOLD)
				write_to_fsm = true;
			else
				write_to_fsm = false;
		}
	}

	if (!write_to_fsm)
		return;

	/* Get the location of the FSM byte representing the heap block */
	addr = fsm_get_location(heapBlk, &slot);
	blkno = fsm_logical_to_physical(addr);

	/* If the page doesn't exist already, extend */
	buf = XLogReadBufferExtended(rnode, FSM_FORKNUM, blkno, RBM_ZERO_ON_ERROR);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buf);
	if (PageIsNew(page))
		PageInit(page, BLCKSZ, 0);

	if (fsm_set_avail(page, slot, new_cat))
		MarkBufferDirtyHint(buf, false);
	UnlockReleaseBuffer(buf);
}

/*
 * GetRecordedFreePage - return the amount of free space on a particular page,
 *		according to the FSM.
 */
Size
GetRecordedFreeSpace(Relation rel, BlockNumber heapBlk)
{
	FSMAddress	addr;
	uint16		slot;
	Buffer		buf;
	uint8		cat;

	/* Get the location of the FSM byte representing the heap block */
	addr = fsm_get_location(heapBlk, &slot);

	buf = fsm_readbuf(rel, addr, false);
	if (!BufferIsValid(buf))
		return 0;
	cat = fsm_get_avail(BufferGetPage(buf), slot);
	ReleaseBuffer(buf);

	return fsm_space_cat_to_avail(cat);
}

/*
 * FreeSpaceMapTruncateRel - adjust for truncation of a relation.
 *
 * The caller must hold AccessExclusiveLock on the relation, to ensure that
 * other backends receive the smgr invalidation event that this function sends
 * before they access the FSM again.
 *
 * nblocks is the new size of the heap.
 */
void
FreeSpaceMapTruncateRel(Relation rel, BlockNumber nblocks)
{
	BlockNumber new_nfsmblocks;
	FSMAddress	first_removed_address;
	uint16		first_removed_slot;
	Buffer		buf;

	RelationOpenSmgr(rel);

	/*
	 * If no FSM has been created yet for this relation, there's nothing to
	 * truncate.
	 */
	if (!smgrexists(rel->rd_smgr, FSM_FORKNUM))
		return;

	/* Get the location in the FSM of the first removed heap block */
	first_removed_address = fsm_get_location(nblocks, &first_removed_slot);

	/*
	 * Zero out the tail of the last remaining FSM page. If the slot
	 * representing the first removed heap block is at a page boundary, as the
	 * first slot on the FSM page that first_removed_address points to, we can
	 * just truncate that page altogether.
	 */
	if (first_removed_slot > 0)
	{
		buf = fsm_readbuf(rel, first_removed_address, false);
		if (!BufferIsValid(buf))
			return;				/* nothing to do; the FSM was already smaller */
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		/* NO EREPORT(ERROR) from here till changes are logged */
		START_CRIT_SECTION();

		fsm_truncate_avail(BufferGetPage(buf), first_removed_slot);

		/*
		 * Truncation of a relation is WAL-logged at a higher-level, and we
		 * will be called at WAL replay. But if checksums are enabled, we need
		 * to still write a WAL record to protect against a torn page, if the
		 * page is flushed to disk before the truncation WAL record. We cannot
		 * use MarkBufferDirtyHint here, because that will not dirty the page
		 * during recovery.
		 */
		MarkBufferDirty(buf);
		if (!InRecovery && RelationNeedsWAL(rel) && XLogHintBitIsNeeded())
			log_newpage_buffer(buf, false);

		END_CRIT_SECTION();

		UnlockReleaseBuffer(buf);

		new_nfsmblocks = fsm_logical_to_physical(first_removed_address) + 1;
	}
	else
	{
		new_nfsmblocks = fsm_logical_to_physical(first_removed_address);
		if (smgrnblocks(rel->rd_smgr, FSM_FORKNUM) <= new_nfsmblocks)
			return;				/* nothing to do; the FSM was already smaller */
	}

	/* Truncate the unused FSM pages, and send smgr inval message */
	smgrtruncate(rel->rd_smgr, FSM_FORKNUM, new_nfsmblocks);

	/*
	 * We might as well update the local smgr_fsm_nblocks setting.
	 * smgrtruncate sent an smgr cache inval message, which will cause other
	 * backends to invalidate their copy of smgr_fsm_nblocks, and this one too
	 * at the next command boundary.  But this ensures it isn't outright wrong
	 * until then.
	 */
	if (rel->rd_smgr)
		rel->rd_smgr->smgr_fsm_nblocks = new_nfsmblocks;

	/*
	 * Update upper-level FSM pages to account for the truncation.  This is
	 * important because the just-truncated pages were likely marked as
	 * all-free, and would be preferentially selected.
	 */
	FreeSpaceMapVacuumRange(rel, nblocks, InvalidBlockNumber);
}

/*
 * FreeSpaceMapVacuum - update upper-level pages in the rel's FSM
 *
 * We assume that the bottom-level pages have already been updated with
 * new free-space information.
 */
void
FreeSpaceMapVacuum(Relation rel)
{
	bool		dummy;

	/* Recursively scan the tree, starting at the root */
	(void) fsm_vacuum_page(rel, FSM_ROOT_ADDRESS,
						   (BlockNumber) 0, InvalidBlockNumber,
						   &dummy);
}

/*
 * FreeSpaceMapVacuumRange - update upper-level pages in the rel's FSM
 *
 * As above, but assume that only heap pages between start and end-1 inclusive
 * have new free-space information, so update only the upper-level slots
 * covering that block range.  end == InvalidBlockNumber is equivalent to
 * "all the rest of the relation".
 */
void
FreeSpaceMapVacuumRange(Relation rel, BlockNumber start, BlockNumber end)
{
	bool		dummy;

	/* Recursively scan the tree, starting at the root */
	if (end > start)
		(void) fsm_vacuum_page(rel, FSM_ROOT_ADDRESS, start, end, &dummy);
}

/******** Internal routines ********/

/*
 * Return category corresponding x bytes of free space
 */
static uint8
fsm_space_avail_to_cat(Size avail)
{
	int			cat;

	Assert(avail < BLCKSZ);

	if (avail >= MaxFSMRequestSize)
		return 255;

	cat = avail / FSM_CAT_STEP;

	/*
	 * The highest category, 255, is reserved for MaxFSMRequestSize bytes or
	 * more.
	 */
	if (cat > 254)
		cat = 254;

	return (uint8) cat;
}

/*
 * Return the lower bound of the range of free space represented by given
 * category.
 */
static Size
fsm_space_cat_to_avail(uint8 cat)
{
	/* The highest category represents exactly MaxFSMRequestSize bytes. */
	if (cat == 255)
		return MaxFSMRequestSize;
	else
		return cat * FSM_CAT_STEP;
}

/*
 * Which category does a page need to have, to accommodate x bytes of data?
 * While fsm_size_to_avail_cat() rounds down, this needs to round up.
 */
static uint8
fsm_space_needed_to_cat(Size needed)
{
	int			cat;

	/* Can't ask for more space than the highest category represents */
	if (needed > MaxFSMRequestSize)
		elog(ERROR, "invalid FSM request size %zu", needed);

	if (needed == 0)
		return 1;

	cat = (needed + FSM_CAT_STEP - 1) / FSM_CAT_STEP;

	if (cat > 255)
		cat = 255;

	return (uint8) cat;
}

/*
 * Returns the physical block number of a FSM page
 */
static BlockNumber
fsm_logical_to_physical(FSMAddress addr)
{
	BlockNumber pages;
	int			leafno;
	int			l;

	/*
	 * Calculate the logical page number of the first leaf page below the
	 * given page.
	 */
	leafno = addr.logpageno;
	for (l = 0; l < addr.level; l++)
		leafno *= SlotsPerFSMPage;

	/* Count upper level nodes required to address the leaf page */
	pages = 0;
	for (l = 0; l < FSM_TREE_DEPTH; l++)
	{
		pages += leafno + 1;
		leafno /= SlotsPerFSMPage;
	}

	/*
	 * If the page we were asked for wasn't at the bottom level, subtract the
	 * additional lower level pages we counted above.
	 */
	pages -= addr.level;

	/* Turn the page count into 0-based block number */
	return pages - 1;
}

/*
 * Return the FSM location corresponding to given heap block.
 */
static FSMAddress
fsm_get_location(BlockNumber heapblk, uint16 *slot)
{
	FSMAddress	addr;

	addr.level = FSM_BOTTOM_LEVEL;
	addr.logpageno = heapblk / SlotsPerFSMPage;
	*slot = heapblk % SlotsPerFSMPage;

	return addr;
}

/*
 * Return the heap block number corresponding to given location in the FSM.
 */
static BlockNumber
fsm_get_heap_blk(FSMAddress addr, uint16 slot)
{
	Assert(addr.level == FSM_BOTTOM_LEVEL);
	return ((unsigned int) addr.logpageno) * SlotsPerFSMPage + slot;
}

/*
 * Given a logical address of a child page, get the logical page number of
 * the parent, and the slot within the parent corresponding to the child.
 */
static FSMAddress
fsm_get_parent(FSMAddress child, uint16 *slot)
{
	FSMAddress	parent;

	Assert(child.level < FSM_ROOT_LEVEL);

	parent.level = child.level + 1;
	parent.logpageno = child.logpageno / SlotsPerFSMPage;
	*slot = child.logpageno % SlotsPerFSMPage;

	return parent;
}

/*
 * Given a logical address of a parent page and a slot number, get the
 * logical address of the corresponding child page.
 */
static FSMAddress
fsm_get_child(FSMAddress parent, uint16 slot)
{
	FSMAddress	child;

	Assert(parent.level > FSM_BOTTOM_LEVEL);

	child.level = parent.level - 1;
	child.logpageno = parent.logpageno * SlotsPerFSMPage + slot;

	return child;
}

/*
 * Read a FSM page.
 *
 * If the page doesn't exist, InvalidBuffer is returned, or if 'extend' is
 * true, the FSM file is extended.
 */
static Buffer
fsm_readbuf(Relation rel, FSMAddress addr, bool extend)
{
	BlockNumber blkno = fsm_logical_to_physical(addr);
	Buffer		buf;

	RelationOpenSmgr(rel);

	/*
	 * If we haven't cached the size of the FSM yet, check it first.  Also
	 * recheck if the requested block seems to be past end, since our cached
	 * value might be stale.  (We send smgr inval messages on truncation, but
	 * not on extension.)
	 */
	if (rel->rd_smgr->smgr_fsm_nblocks == InvalidBlockNumber ||
		blkno >= rel->rd_smgr->smgr_fsm_nblocks)
	{
		if (smgrexists(rel->rd_smgr, FSM_FORKNUM))
			rel->rd_smgr->smgr_fsm_nblocks = smgrnblocks(rel->rd_smgr,
														 FSM_FORKNUM);
		else
			rel->rd_smgr->smgr_fsm_nblocks = 0;
	}

	/* Handle requests beyond EOF */
	if (blkno >= rel->rd_smgr->smgr_fsm_nblocks)
	{
		if (extend)
			fsm_extend(rel, blkno + 1);
		else
			return InvalidBuffer;
	}

	/*
	 * Use ZERO_ON_ERROR mode, and initialize the page if necessary. The FSM
	 * information is not accurate anyway, so it's better to clear corrupt
	 * pages than error out. Since the FSM changes are not WAL-logged, the
	 * so-called torn page problem on crash can lead to pages with corrupt
	 * headers, for example.
	 *
	 * The initialize-the-page part is trickier than it looks, because of the
	 * possibility of multiple backends doing this concurrently, and our
	 * desire to not uselessly take the buffer lock in the normal path where
	 * the page is OK.  We must take the lock to initialize the page, so
	 * recheck page newness after we have the lock, in case someone else
	 * already did it.  Also, because we initially check PageIsNew with no
	 * lock, it's possible to fall through and return the buffer while someone
	 * else is still initializing the page (i.e., we might see pd_upper as set
	 * but other page header fields are still zeroes).  This is harmless for
	 * callers that will take a buffer lock themselves, but some callers
	 * inspect the page without any lock at all.  The latter is OK only so
	 * long as it doesn't depend on the page header having correct contents.
	 * Current usage is safe because PageGetContents() does not require that.
	 */
	buf = ReadBufferExtended(rel, FSM_FORKNUM, blkno, RBM_ZERO_ON_ERROR, NULL);
	if (PageIsNew(BufferGetPage(buf)))
	{
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		if (PageIsNew(BufferGetPage(buf)))
			PageInit(BufferGetPage(buf), BLCKSZ, 0);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	}
	return buf;
}

/*
 * Ensure that the FSM fork is at least fsm_nblocks long, extending
 * it if necessary with empty pages. And by empty, I mean pages filled
 * with zeros, meaning there's no free space.
 */
static void
fsm_extend(Relation rel, BlockNumber fsm_nblocks)
{
	BlockNumber fsm_nblocks_now;
	PGAlignedBlock pg;

	PageInit((Page) pg.data, BLCKSZ, 0);

	/*
	 * We use the relation extension lock to lock out other backends trying to
	 * extend the FSM at the same time. It also locks out extension of the
	 * main fork, unnecessarily, but extending the FSM happens seldom enough
	 * that it doesn't seem worthwhile to have a separate lock tag type for
	 * it.
	 *
	 * Note that another backend might have extended or created the relation
	 * by the time we get the lock.
	 */
	LockRelationForExtension(rel, ExclusiveLock);

	/* Might have to re-open if a cache flush happened */
	RelationOpenSmgr(rel);

	/*
	 * Create the FSM file first if it doesn't exist.  If smgr_fsm_nblocks is
	 * positive then it must exist, no need for an smgrexists call.
	 */
	if ((rel->rd_smgr->smgr_fsm_nblocks == 0 ||
		 rel->rd_smgr->smgr_fsm_nblocks == InvalidBlockNumber) &&
		!smgrexists(rel->rd_smgr, FSM_FORKNUM))
		smgrcreate(rel->rd_smgr, FSM_FORKNUM, false);

	fsm_nblocks_now = smgrnblocks(rel->rd_smgr, FSM_FORKNUM);

	while (fsm_nblocks_now < fsm_nblocks)
	{
		PageSetChecksumInplace((Page) pg.data, fsm_nblocks_now);

		smgrextend(rel->rd_smgr, FSM_FORKNUM, fsm_nblocks_now,
				   pg.data, false);
		fsm_nblocks_now++;
	}

	/* Update local cache with the up-to-date size */
	rel->rd_smgr->smgr_fsm_nblocks = fsm_nblocks_now;

	UnlockRelationForExtension(rel, ExclusiveLock);
}

/*
 * Set value in given FSM page and slot.
 *
 * If minValue > 0, the updated page is also searched for a page with at
 * least minValue of free space. If one is found, its slot number is
 * returned, -1 otherwise.
 */
static int
fsm_set_and_search(Relation rel, FSMAddress addr, uint16 slot,
				   uint8 newValue, uint8 minValue)
{
	Buffer		buf;
	Page		page;
	int			newslot = -1;

	buf = fsm_readbuf(rel, addr, true);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buf);

	if (fsm_set_avail(page, slot, newValue))
		MarkBufferDirtyHint(buf, false);

	if (minValue != 0)
	{
		/* Search while we still hold the lock */
		newslot = fsm_search_avail(buf, minValue,
								   addr.level == FSM_BOTTOM_LEVEL,
								   true);
	}

	UnlockReleaseBuffer(buf);

	return newslot;
}

/*
 * Search the tree for a heap page with at least min_cat of free space
 */
static BlockNumber
fsm_search(Relation rel, uint8 min_cat)
{
	int			restarts = 0;
	FSMAddress	addr = FSM_ROOT_ADDRESS;

	for (;;)
	{
		int			slot;
		Buffer		buf;
		uint8		max_avail = 0;

		/* Read the FSM page. */
		buf = fsm_readbuf(rel, addr, false);

		/* Search within the page */
		if (BufferIsValid(buf))
		{
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			slot = fsm_search_avail(buf, min_cat,
									(addr.level == FSM_BOTTOM_LEVEL),
									false);
			if (slot == -1)
				max_avail = fsm_get_max_avail(BufferGetPage(buf));
			UnlockReleaseBuffer(buf);
		}
		else
			slot = -1;

		if (slot != -1)
		{
			/*
			 * Descend the tree, or return the found block if we're at the
			 * bottom.
			 */
			if (addr.level == FSM_BOTTOM_LEVEL)
				return fsm_get_heap_blk(addr, slot);

			addr = fsm_get_child(addr, slot);
		}
		else if (addr.level == FSM_ROOT_LEVEL)
		{
			/*
			 * At the root, failure means there's no page with enough free
			 * space in the FSM. Give up.
			 */
			return InvalidBlockNumber;
		}
		else
		{
			uint16		parentslot;
			FSMAddress	parent;

			/*
			 * At lower level, failure can happen if the value in the upper-
			 * level node didn't reflect the value on the lower page. Update
			 * the upper node, to avoid falling into the same trap again, and
			 * start over.
			 *
			 * There's a race condition here, if another backend updates this
			 * page right after we release it, and gets the lock on the parent
			 * page before us. We'll then update the parent page with the now
			 * stale information we had. It's OK, because it should happen
			 * rarely, and will be fixed by the next vacuum.
			 */
			parent = fsm_get_parent(addr, &parentslot);
			fsm_set_and_search(rel, parent, parentslot, max_avail, 0);

			/*
			 * If the upper pages are badly out of date, we might need to loop
			 * quite a few times, updating them as we go. Any inconsistencies
			 * should eventually be corrected and the loop should end. Looping
			 * indefinitely is nevertheless scary, so provide an emergency
			 * valve.
			 */
			if (restarts++ > 10000)
				return InvalidBlockNumber;

			/* Start search all over from the root */
			addr = FSM_ROOT_ADDRESS;
		}
	}
}


/*
 * Recursive guts of FreeSpaceMapVacuum
 *
 * Examine the FSM page indicated by addr, as well as its children, updating
 * upper-level nodes that cover the heap block range from start to end-1.
 * (It's okay if end is beyond the actual end of the map.)
 * Return the maximum freespace value on this page.
 *
 * If addr is past the end of the FSM, set *eof_p to true and return 0.
 *
 * This traverses the tree in depth-first order.  The tree is stored
 * physically in depth-first order, so this should be pretty I/O efficient.
 */
static uint8
fsm_vacuum_page(Relation rel, FSMAddress addr,
				BlockNumber start, BlockNumber end,
				bool *eof_p)
{
	Buffer		buf;
	Page		page;
	uint8		max_avail;

	/* Read the page if it exists, or return EOF */
	buf = fsm_readbuf(rel, addr, false);
	if (!BufferIsValid(buf))
	{
		*eof_p = true;
		return 0;
	}
	else
		*eof_p = false;

	page = BufferGetPage(buf);

	/*
	 * If we're above the bottom level, recurse into children, and fix the
	 * information stored about them at this level.
	 */
	if (addr.level > FSM_BOTTOM_LEVEL)
	{
		FSMAddress	fsm_start,
					fsm_end;
		uint16		fsm_start_slot,
					fsm_end_slot;
		int			slot,
					start_slot,
					end_slot;
		bool		eof = false;

		/*
		 * Compute the range of slots we need to update on this page, given
		 * the requested range of heap blocks to consider.  The first slot to
		 * update is the one covering the "start" block, and the last slot is
		 * the one covering "end - 1".  (Some of this work will be duplicated
		 * in each recursive call, but it's cheap enough to not worry about.)
		 */
		fsm_start = fsm_get_location(start, &fsm_start_slot);
		fsm_end = fsm_get_location(end - 1, &fsm_end_slot);

		while (fsm_start.level < addr.level)
		{
			fsm_start = fsm_get_parent(fsm_start, &fsm_start_slot);
			fsm_end = fsm_get_parent(fsm_end, &fsm_end_slot);
		}
		Assert(fsm_start.level == addr.level);

		if (fsm_start.logpageno == addr.logpageno)
			start_slot = fsm_start_slot;
		else if (fsm_start.logpageno > addr.logpageno)
			start_slot = SlotsPerFSMPage;	/* shouldn't get here... */
		else
			start_slot = 0;

		if (fsm_end.logpageno == addr.logpageno)
			end_slot = fsm_end_slot;
		else if (fsm_end.logpageno > addr.logpageno)
			end_slot = SlotsPerFSMPage - 1;
		else
			end_slot = -1;		/* shouldn't get here... */

		for (slot = start_slot; slot <= end_slot; slot++)
		{
			int			child_avail;

			CHECK_FOR_INTERRUPTS();

			/* After we hit end-of-file, just clear the rest of the slots */
			if (!eof)
				child_avail = fsm_vacuum_page(rel, fsm_get_child(addr, slot),
											  start, end,
											  &eof);
			else
				child_avail = 0;

			/* Update information about the child */
			if (fsm_get_avail(page, slot) != child_avail)
			{
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				fsm_set_avail(page, slot, child_avail);
				MarkBufferDirtyHint(buf, false);
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			}
		}
	}

	/* Now get the maximum value on the page, to return to caller */
	max_avail = fsm_get_max_avail(page);

	/*
	 * Reset the next slot pointer. This encourages the use of low-numbered
	 * pages, increasing the chances that a later vacuum can truncate the
	 * relation.  We don't bother with a lock here, nor with marking the page
	 * dirty if it wasn't already, since this is just a hint.
	 */
	((FSMPage) PageGetContents(page))->fp_next_slot = 0;

	ReleaseBuffer(buf);

	return max_avail;
}

/*
 * For heaps, we prevent creation of the FSM unless the number of pages
 * exceeds HEAP_FSM_CREATION_THRESHOLD.  For tables that don't already have
 * a FSM, this will save an inode and a few kB of space.
 *
 * XXX The API is a little awkward -- if the caller passes a valid nblocks
 * value, it can avoid invoking a system call.  If the caller passes
 * InvalidBlockNumber and receives a false return value, it can get an
 * up-to-date relation size from get_nblocks.  This saves a few cycles in
 * the caller, which would otherwise need to get the relation size by itself.
 */
static bool
fsm_allow_writes(Relation rel, BlockNumber heapblk,
				 BlockNumber nblocks, BlockNumber *get_nblocks)
{
	bool		skip_get_nblocks;

	if (heapblk >= HEAP_FSM_CREATION_THRESHOLD)
		return true;

	/* Non-heap rels can always create a FSM. */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_TOASTVALUE)
		return true;

	/*
	 * If the caller knows nblocks, we can avoid a system call later. If it
	 * doesn't, maybe we have relpages from a previous VACUUM. Since the table
	 * may have extended since then, we still have to count the pages later if
	 * we can't return now.
	 */
	if (nblocks != InvalidBlockNumber)
	{
		if (nblocks > HEAP_FSM_CREATION_THRESHOLD)
			return true;
		else
			skip_get_nblocks = true;
	}
	else
	{
		if (rel->rd_rel->relpages != InvalidBlockNumber &&
			rel->rd_rel->relpages > HEAP_FSM_CREATION_THRESHOLD)
			return true;
		else
			skip_get_nblocks = false;
	}

	RelationOpenSmgr(rel);
	if (smgrexists(rel->rd_smgr, FSM_FORKNUM))
		return true;

	if (skip_get_nblocks)
		return false;

	/* last resort */
	*get_nblocks = RelationGetNumberOfBlocks(rel);
	if (*get_nblocks > HEAP_FSM_CREATION_THRESHOLD)
		return true;
	else
		return false;
}

/*
 * Initialize or update the local map of blocks to try, for when there is
 * no FSM.
 *
 * When we initialize the map, the whole heap is potentially available to
 * try.  Testing revealed that trying every block can cause a small
 * performance dip compared to when we use a FSM, so we try every other
 * block instead.
 */
static void
fsm_local_set(Relation rel, BlockNumber cur_nblocks)
{
	BlockNumber blkno,
				cached_target_block;

	/* The local map must not be set already. */
	Assert(!FSM_LOCAL_MAP_EXISTS);

	/*
	 * Starting at the current last block in the relation and working
	 * backwards, mark alternating blocks as available.
	 */
	blkno = cur_nblocks - 1;
	while (true)
	{
		fsm_local_map.map[blkno] = FSM_LOCAL_AVAIL;
		if (blkno >= 2)
			blkno -= 2;
		else
			break;
	}

	/* Cache the number of blocks. */
	fsm_local_map.nblocks = cur_nblocks;

	/* Set the status of the cached target block to 'unavailable'. */
	cached_target_block = RelationGetTargetBlock(rel);
	if (cached_target_block != InvalidBlockNumber &&
		cached_target_block < cur_nblocks)
		fsm_local_map.map[cached_target_block] = FSM_LOCAL_NOT_AVAIL;
}

/*
 * Search the local map for an available block to try, in descending order.
 * As such, there is no heuristic available to decide which order will be
 * better to try, but the probability of having space in the last block in the
 * map is higher because that is the most recent block added to the heap.
 *
 * This function is used when there is no FSM.
 */
static BlockNumber
fsm_local_search(void)
{
	BlockNumber target_block;

	/* Local map must be set by now. */
	Assert(FSM_LOCAL_MAP_EXISTS);

	target_block = fsm_local_map.nblocks;
	do
	{
		target_block--;
		if (fsm_local_map.map[target_block] == FSM_LOCAL_AVAIL)
			return target_block;
	} while (target_block > 0);

	return InvalidBlockNumber;
}
