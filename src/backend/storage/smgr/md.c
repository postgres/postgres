/*-------------------------------------------------------------------------
 *
 * md.c
 *	  This code manages relations that reside on magnetic disk.
 *
 * Or at least, that was what the Berkeley folk had in mind when they named
 * this file.  In reality, what this code provides is an interface from
 * the smgr API to Unix-like filesystem APIs, so it will work with any type
 * of device for which the operating system provides filesystem support.
 * It doesn't matter whether the bits are on spinning rust or some other
 * storage technology.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/md.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "access/xlog.h"
#include "access/xlogutils.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/md.h"
#include "storage/relfilenode.h"
#include "storage/smgr.h"
#include "storage/sync.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/*
 *	The magnetic disk storage manager keeps track of open file
 *	descriptors in its own descriptor pool.  This is done to make it
 *	easier to support relations that are larger than the operating
 *	system's file size limit (often 2GBytes).  In order to do that,
 *	we break relations up into "segment" files that are each shorter than
 *	the OS file size limit.  The segment size is set by the RELSEG_SIZE
 *	configuration constant in pg_config.h.
 *
 *	On disk, a relation must consist of consecutively numbered segment
 *	files in the pattern
 *		-- Zero or more full segments of exactly RELSEG_SIZE blocks each
 *		-- Exactly one partial segment of size 0 <= size < RELSEG_SIZE blocks
 *		-- Optionally, any number of inactive segments of size 0 blocks.
 *	The full and partial segments are collectively the "active" segments.
 *	Inactive segments are those that once contained data but are currently
 *	not needed because of an mdtruncate() operation.  The reason for leaving
 *	them present at size zero, rather than unlinking them, is that other
 *	backends and/or the checkpointer might be holding open file references to
 *	such segments.  If the relation expands again after mdtruncate(), such
 *	that a deactivated segment becomes active again, it is important that
 *	such file references still be valid --- else data might get written
 *	out to an unlinked old copy of a segment file that will eventually
 *	disappear.
 *
 *	File descriptors are stored in the per-fork md_seg_fds arrays inside
 *	SMgrRelation. The length of these arrays is stored in md_num_open_segs.
 *	Note that a fork's md_num_open_segs having a specific value does not
 *	necessarily mean the relation doesn't have additional segments; we may
 *	just not have opened the next segment yet.  (We could not have "all
 *	segments are in the array" as an invariant anyway, since another backend
 *	could extend the relation while we aren't looking.)  We do not have
 *	entries for inactive segments, however; as soon as we find a partial
 *	segment, we assume that any subsequent segments are inactive.
 *
 *	The entire MdfdVec array is palloc'd in the MdCxt memory context.
 */

typedef struct _MdfdVec
{
	File		mdfd_vfd;		/* fd number in fd.c's pool */
	BlockNumber mdfd_segno;		/* segment number, from 0 */
} MdfdVec;

static MemoryContext MdCxt;		/* context for all MdfdVec objects */


/* Populate a file tag describing an md.c segment file. */
#define INIT_MD_FILETAG(a,xx_rnode,xx_forknum,xx_segno) \
( \
	memset(&(a), 0, sizeof(FileTag)), \
	(a).handler = SYNC_HANDLER_MD, \
	(a).rnode = (xx_rnode), \
	(a).forknum = (xx_forknum), \
	(a).segno = (xx_segno) \
)


/*** behavior for mdopen & _mdfd_getseg ***/
/* ereport if segment not present */
#define EXTENSION_FAIL				(1 << 0)
/* return NULL if segment not present */
#define EXTENSION_RETURN_NULL		(1 << 1)
/* create new segments as needed */
#define EXTENSION_CREATE			(1 << 2)
/* create new segments if needed during recovery */
#define EXTENSION_CREATE_RECOVERY	(1 << 3)
/*
 * Allow opening segments which are preceded by segments smaller than
 * RELSEG_SIZE, e.g. inactive segments (see above). Note that this breaks
 * mdnblocks() and related functionality henceforth - which currently is ok,
 * because this is only required in the checkpointer which never uses
 * mdnblocks().
 */
#define EXTENSION_DONT_CHECK_SIZE	(1 << 4)


/* local routines */
static void mdunlinkfork(RelFileNodeBackend rnode, ForkNumber forkNum,
						 bool isRedo);
static MdfdVec *mdopenfork(SMgrRelation reln, ForkNumber forknum, int behavior);
static void register_dirty_segment(SMgrRelation reln, ForkNumber forknum,
								   MdfdVec *seg);
static void register_unlink_segment(RelFileNodeBackend rnode, ForkNumber forknum,
									BlockNumber segno);
static void register_forget_request(RelFileNodeBackend rnode, ForkNumber forknum,
									BlockNumber segno);
static void _fdvec_resize(SMgrRelation reln,
						  ForkNumber forknum,
						  int nseg);
static char *_mdfd_segpath(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber segno);
static MdfdVec *_mdfd_openseg(SMgrRelation reln, ForkNumber forkno,
							  BlockNumber segno, int oflags);
static MdfdVec *_mdfd_getseg(SMgrRelation reln, ForkNumber forkno,
							 BlockNumber blkno, bool skipFsync, int behavior);
static BlockNumber _mdnblocks(SMgrRelation reln, ForkNumber forknum,
							  MdfdVec *seg);


/*
 *	mdinit() -- Initialize private state for magnetic disk storage manager.
 */
void
mdinit(void)
{
	MdCxt = AllocSetContextCreate(TopMemoryContext,
								  "MdSmgr",
								  ALLOCSET_DEFAULT_SIZES);
}

/*
 *	mdexists() -- Does the physical file exist?
 *
 * Note: this will return true for lingering files, with pending deletions
 */
bool
mdexists(SMgrRelation reln, ForkNumber forkNum)
{
	/*
	 * Close it first, to ensure that we notice if the fork has been unlinked
	 * since we opened it.
	 */
	mdclose(reln, forkNum);

	return (mdopenfork(reln, forkNum, EXTENSION_RETURN_NULL) != NULL);
}

/*
 *	mdcreate() -- Create a new relation on magnetic disk.
 *
 * If isRedo is true, it's okay for the relation to exist already.
 */
void
mdcreate(SMgrRelation reln, ForkNumber forkNum, bool isRedo)
{
	MdfdVec    *mdfd;
	char	   *path;
	File		fd;

	if (isRedo && reln->md_num_open_segs[forkNum] > 0)
		return;					/* created and opened already... */

	Assert(reln->md_num_open_segs[forkNum] == 0);

	/*
	 * We may be using the target table space for the first time in this
	 * database, so create a per-database subdirectory if needed.
	 *
	 * XXX this is a fairly ugly violation of module layering, but this seems
	 * to be the best place to put the check.  Maybe TablespaceCreateDbspace
	 * should be here and not in commands/tablespace.c?  But that would imply
	 * importing a lot of stuff that smgr.c oughtn't know, either.
	 */
	TablespaceCreateDbspace(reln->smgr_rnode.node.spcNode,
							reln->smgr_rnode.node.dbNode,
							isRedo);

	path = relpath(reln->smgr_rnode, forkNum);

	fd = PathNameOpenFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);

	if (fd < 0)
	{
		int			save_errno = errno;

		if (isRedo)
			fd = PathNameOpenFile(path, O_RDWR | PG_BINARY);
		if (fd < 0)
		{
			/* be sure to report the error reported by create, not open */
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m", path)));
		}
	}

	pfree(path);

	_fdvec_resize(reln, forkNum, 1);
	mdfd = &reln->md_seg_fds[forkNum][0];
	mdfd->mdfd_vfd = fd;
	mdfd->mdfd_segno = 0;
}

/*
 *	mdunlink() -- Unlink a relation.
 *
 * Note that we're passed a RelFileNodeBackend --- by the time this is called,
 * there won't be an SMgrRelation hashtable entry anymore.
 *
 * forkNum can be a fork number to delete a specific fork, or InvalidForkNumber
 * to delete all forks.
 *
 * For regular relations, we don't unlink the first segment file of the rel,
 * but just truncate it to zero length, and record a request to unlink it after
 * the next checkpoint.  Additional segments can be unlinked immediately,
 * however.  Leaving the empty file in place prevents that relfilenode
 * number from being reused.  The scenario this protects us from is:
 * 1. We delete a relation (and commit, and actually remove its file).
 * 2. We create a new relation, which by chance gets the same relfilenode as
 *	  the just-deleted one (OIDs must've wrapped around for that to happen).
 * 3. We crash before another checkpoint occurs.
 * During replay, we would delete the file and then recreate it, which is fine
 * if the contents of the file were repopulated by subsequent WAL entries.
 * But if we didn't WAL-log insertions, but instead relied on fsyncing the
 * file after populating it (as we do at wal_level=minimal), the contents of
 * the file would be lost forever.  By leaving the empty file until after the
 * next checkpoint, we prevent reassignment of the relfilenode number until
 * it's safe, because relfilenode assignment skips over any existing file.
 *
 * We do not need to go through this dance for temp relations, though, because
 * we never make WAL entries for temp rels, and so a temp rel poses no threat
 * to the health of a regular rel that has taken over its relfilenode number.
 * The fact that temp rels and regular rels have different file naming
 * patterns provides additional safety.
 *
 * All the above applies only to the relation's main fork; other forks can
 * just be removed immediately, since they are not needed to prevent the
 * relfilenode number from being recycled.  Also, we do not carefully
 * track whether other forks have been created or not, but just attempt to
 * unlink them unconditionally; so we should never complain about ENOENT.
 *
 * If isRedo is true, it's unsurprising for the relation to be already gone.
 * Also, we should remove the file immediately instead of queuing a request
 * for later, since during redo there's no possibility of creating a
 * conflicting relation.
 *
 * Note: any failure should be reported as WARNING not ERROR, because
 * we are usually not in a transaction anymore when this is called.
 */
void
mdunlink(RelFileNodeBackend rnode, ForkNumber forkNum, bool isRedo)
{
	/* Now do the per-fork work */
	if (forkNum == InvalidForkNumber)
	{
		for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
			mdunlinkfork(rnode, forkNum, isRedo);
	}
	else
		mdunlinkfork(rnode, forkNum, isRedo);
}

/*
 * Truncate a file to release disk space.
 */
static int
do_truncate(const char *path)
{
	int			save_errno;
	int			ret;

	ret = pg_truncate(path, 0);

	/* Log a warning here to avoid repetition in callers. */
	if (ret < 0 && errno != ENOENT)
	{
		save_errno = errno;
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m", path)));
		errno = save_errno;
	}

	return ret;
}

static void
mdunlinkfork(RelFileNodeBackend rnode, ForkNumber forkNum, bool isRedo)
{
	char	   *path;
	int			ret;

	path = relpath(rnode, forkNum);

	/*
	 * Delete or truncate the first segment.
	 */
	if (isRedo || forkNum != MAIN_FORKNUM || RelFileNodeBackendIsTemp(rnode))
	{
		if (!RelFileNodeBackendIsTemp(rnode))
		{
			/* Prevent other backends' fds from holding on to the disk space */
			ret = do_truncate(path);

			/* Forget any pending sync requests for the first segment */
			register_forget_request(rnode, forkNum, 0 /* first seg */ );
		}
		else
			ret = 0;

		/* Next unlink the file, unless it was already found to be missing */
		if (ret == 0 || errno != ENOENT)
		{
			ret = unlink(path);
			if (ret < 0 && errno != ENOENT)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path)));
		}
	}
	else
	{
		/* Prevent other backends' fds from holding on to the disk space */
		ret = do_truncate(path);

		/* Register request to unlink first segment later */
		register_unlink_segment(rnode, forkNum, 0 /* first seg */ );
	}

	/*
	 * Delete any additional segments.
	 */
	if (ret >= 0)
	{
		char	   *segpath = (char *) palloc(strlen(path) + 12);
		BlockNumber segno;

		/*
		 * Note that because we loop until getting ENOENT, we will correctly
		 * remove all inactive segments as well as active ones.
		 */
		for (segno = 1;; segno++)
		{
			sprintf(segpath, "%s.%u", path, segno);

			if (!RelFileNodeBackendIsTemp(rnode))
			{
				/*
				 * Prevent other backends' fds from holding on to the disk
				 * space.
				 */
				if (do_truncate(segpath) < 0 && errno == ENOENT)
					break;

				/*
				 * Forget any pending sync requests for this segment before we
				 * try to unlink.
				 */
				register_forget_request(rnode, forkNum, segno);
			}

			if (unlink(segpath) < 0)
			{
				/* ENOENT is expected after the last segment... */
				if (errno != ENOENT)
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("could not remove file \"%s\": %m", segpath)));
				break;
			}
		}
		pfree(segpath);
	}

	pfree(path);
}

/*
 *	mdextend() -- Add a block to the specified relation.
 *
 *		The semantics are nearly the same as mdwrite(): write at the
 *		specified position.  However, this is to be used for the case of
 *		extending a relation (i.e., blocknum is at or beyond the current
 *		EOF).  Note that we assume writing a block beyond current EOF
 *		causes intervening file space to become filled with zeroes.
 */
void
mdextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 char *buffer, bool skipFsync)
{
	off_t		seekpos;
	int			nbytes;
	MdfdVec    *v;

	/* This assert is too expensive to have on normally ... */
#ifdef CHECK_WRITE_VS_EXTEND
	Assert(blocknum >= mdnblocks(reln, forknum));
#endif

	/*
	 * If a relation manages to grow to 2^32-1 blocks, refuse to extend it any
	 * more --- we mustn't create a block whose number actually is
	 * InvalidBlockNumber.
	 */
	if (blocknum == InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend file \"%s\" beyond %u blocks",
						relpath(reln->smgr_rnode, forknum),
						InvalidBlockNumber)));

	v = _mdfd_getseg(reln, forknum, blocknum, skipFsync, EXTENSION_CREATE);

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	if ((nbytes = FileWrite(v->mdfd_vfd, buffer, BLCKSZ, seekpos, WAIT_EVENT_DATA_FILE_EXTEND)) != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not extend file \"%s\": %m",
							FilePathName(v->mdfd_vfd)),
					 errhint("Check free disk space.")));
		/* short write: complain appropriately */
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not extend file \"%s\": wrote only %d of %d bytes at block %u",
						FilePathName(v->mdfd_vfd),
						nbytes, BLCKSZ, blocknum),
				 errhint("Check free disk space.")));
	}

	if (!skipFsync && !SmgrIsTemp(reln))
		register_dirty_segment(reln, forknum, v);

	Assert(_mdnblocks(reln, forknum, v) <= ((BlockNumber) RELSEG_SIZE));
}

/*
 *	mdopenfork() -- Open one fork of the specified relation.
 *
 * Note we only open the first segment, when there are multiple segments.
 *
 * If first segment is not present, either ereport or return NULL according
 * to "behavior".  We treat EXTENSION_CREATE the same as EXTENSION_FAIL;
 * EXTENSION_CREATE means it's OK to extend an existing relation, not to
 * invent one out of whole cloth.
 */
static MdfdVec *
mdopenfork(SMgrRelation reln, ForkNumber forknum, int behavior)
{
	MdfdVec    *mdfd;
	char	   *path;
	File		fd;

	/* No work if already open */
	if (reln->md_num_open_segs[forknum] > 0)
		return &reln->md_seg_fds[forknum][0];

	path = relpath(reln->smgr_rnode, forknum);

	fd = PathNameOpenFile(path, O_RDWR | PG_BINARY);

	if (fd < 0)
	{
		if ((behavior & EXTENSION_RETURN_NULL) &&
			FILE_POSSIBLY_DELETED(errno))
		{
			pfree(path);
			return NULL;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	}

	pfree(path);

	_fdvec_resize(reln, forknum, 1);
	mdfd = &reln->md_seg_fds[forknum][0];
	mdfd->mdfd_vfd = fd;
	mdfd->mdfd_segno = 0;

	Assert(_mdnblocks(reln, forknum, mdfd) <= ((BlockNumber) RELSEG_SIZE));

	return mdfd;
}

/*
 *  mdopen() -- Initialize newly-opened relation.
 */
void
mdopen(SMgrRelation reln)
{
	/* mark it not open */
	for (int forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		reln->md_num_open_segs[forknum] = 0;
}

/*
 *	mdclose() -- Close the specified relation, if it isn't closed already.
 */
void
mdclose(SMgrRelation reln, ForkNumber forknum)
{
	int			nopensegs = reln->md_num_open_segs[forknum];

	/* No work if already closed */
	if (nopensegs == 0)
		return;

	/* close segments starting from the end */
	while (nopensegs > 0)
	{
		MdfdVec    *v = &reln->md_seg_fds[forknum][nopensegs - 1];

		FileClose(v->mdfd_vfd);
		_fdvec_resize(reln, forknum, nopensegs - 1);
		nopensegs--;
	}
}

/*
 *	mdprefetch() -- Initiate asynchronous read of the specified block of a relation
 */
bool
mdprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
#ifdef USE_PREFETCH
	off_t		seekpos;
	MdfdVec    *v;

	v = _mdfd_getseg(reln, forknum, blocknum, false,
					 InRecovery ? EXTENSION_RETURN_NULL : EXTENSION_FAIL);
	if (v == NULL)
		return false;

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	(void) FilePrefetch(v->mdfd_vfd, seekpos, BLCKSZ, WAIT_EVENT_DATA_FILE_PREFETCH);
#endif							/* USE_PREFETCH */

	return true;
}

/*
 * mdwriteback() -- Tell the kernel to write pages back to storage.
 *
 * This accepts a range of blocks because flushing several pages at once is
 * considerably more efficient than doing so individually.
 */
void
mdwriteback(SMgrRelation reln, ForkNumber forknum,
			BlockNumber blocknum, BlockNumber nblocks)
{
	/*
	 * Issue flush requests in as few requests as possible; have to split at
	 * segment boundaries though, since those are actually separate files.
	 */
	while (nblocks > 0)
	{
		BlockNumber nflush = nblocks;
		off_t		seekpos;
		MdfdVec    *v;
		int			segnum_start,
					segnum_end;

		v = _mdfd_getseg(reln, forknum, blocknum, true /* not used */ ,
						 EXTENSION_RETURN_NULL);

		/*
		 * We might be flushing buffers of already removed relations, that's
		 * ok, just ignore that case.
		 */
		if (!v)
			return;

		/* compute offset inside the current segment */
		segnum_start = blocknum / RELSEG_SIZE;

		/* compute number of desired writes within the current segment */
		segnum_end = (blocknum + nblocks - 1) / RELSEG_SIZE;
		if (segnum_start != segnum_end)
			nflush = RELSEG_SIZE - (blocknum % ((BlockNumber) RELSEG_SIZE));

		Assert(nflush >= 1);
		Assert(nflush <= nblocks);

		seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

		FileWriteback(v->mdfd_vfd, seekpos, (off_t) BLCKSZ * nflush, WAIT_EVENT_DATA_FILE_FLUSH);

		nblocks -= nflush;
		blocknum += nflush;
	}
}

/*
 *	mdread() -- Read the specified block from a relation.
 */
void
mdread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
	   char *buffer)
{
	off_t		seekpos;
	int			nbytes;
	MdfdVec    *v;

	TRACE_POSTGRESQL_SMGR_MD_READ_START(forknum, blocknum,
										reln->smgr_rnode.node.spcNode,
										reln->smgr_rnode.node.dbNode,
										reln->smgr_rnode.node.relNode,
										reln->smgr_rnode.backend);

	v = _mdfd_getseg(reln, forknum, blocknum, false,
					 EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	nbytes = FileRead(v->mdfd_vfd, buffer, BLCKSZ, seekpos, WAIT_EVENT_DATA_FILE_READ);

	TRACE_POSTGRESQL_SMGR_MD_READ_DONE(forknum, blocknum,
									   reln->smgr_rnode.node.spcNode,
									   reln->smgr_rnode.node.dbNode,
									   reln->smgr_rnode.node.relNode,
									   reln->smgr_rnode.backend,
									   nbytes,
									   BLCKSZ);

	if (nbytes != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read block %u in file \"%s\": %m",
							blocknum, FilePathName(v->mdfd_vfd))));

		/*
		 * Short read: we are at or past EOF, or we read a partial block at
		 * EOF.  Normally this is an error; upper levels should never try to
		 * read a nonexistent block.  However, if zero_damaged_pages is ON or
		 * we are InRecovery, we should instead return zeroes without
		 * complaining.  This allows, for example, the case of trying to
		 * update a block that was later truncated away.
		 */
		if (zero_damaged_pages || InRecovery)
			MemSet(buffer, 0, BLCKSZ);
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read block %u in file \"%s\": read only %d of %d bytes",
							blocknum, FilePathName(v->mdfd_vfd),
							nbytes, BLCKSZ)));
	}
}

/*
 *	mdwrite() -- Write the supplied block at the appropriate location.
 *
 *		This is to be used only for updating already-existing blocks of a
 *		relation (ie, those before the current EOF).  To extend a relation,
 *		use mdextend().
 */
void
mdwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		char *buffer, bool skipFsync)
{
	off_t		seekpos;
	int			nbytes;
	MdfdVec    *v;

	/* This assert is too expensive to have on normally ... */
#ifdef CHECK_WRITE_VS_EXTEND
	Assert(blocknum < mdnblocks(reln, forknum));
#endif

	TRACE_POSTGRESQL_SMGR_MD_WRITE_START(forknum, blocknum,
										 reln->smgr_rnode.node.spcNode,
										 reln->smgr_rnode.node.dbNode,
										 reln->smgr_rnode.node.relNode,
										 reln->smgr_rnode.backend);

	v = _mdfd_getseg(reln, forknum, blocknum, skipFsync,
					 EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	nbytes = FileWrite(v->mdfd_vfd, buffer, BLCKSZ, seekpos, WAIT_EVENT_DATA_FILE_WRITE);

	TRACE_POSTGRESQL_SMGR_MD_WRITE_DONE(forknum, blocknum,
										reln->smgr_rnode.node.spcNode,
										reln->smgr_rnode.node.dbNode,
										reln->smgr_rnode.node.relNode,
										reln->smgr_rnode.backend,
										nbytes,
										BLCKSZ);

	if (nbytes != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write block %u in file \"%s\": %m",
							blocknum, FilePathName(v->mdfd_vfd))));
		/* short write: complain appropriately */
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not write block %u in file \"%s\": wrote only %d of %d bytes",
						blocknum,
						FilePathName(v->mdfd_vfd),
						nbytes, BLCKSZ),
				 errhint("Check free disk space.")));
	}

	if (!skipFsync && !SmgrIsTemp(reln))
		register_dirty_segment(reln, forknum, v);
}

/*
 *	mdnblocks() -- Get the number of blocks stored in a relation.
 *
 *		Important side effect: all active segments of the relation are opened
 *		and added to the md_seg_fds array.  If this routine has not been
 *		called, then only segments up to the last one actually touched
 *		are present in the array.
 */
BlockNumber
mdnblocks(SMgrRelation reln, ForkNumber forknum)
{
	MdfdVec    *v;
	BlockNumber nblocks;
	BlockNumber segno;

	mdopenfork(reln, forknum, EXTENSION_FAIL);

	/* mdopen has opened the first segment */
	Assert(reln->md_num_open_segs[forknum] > 0);

	/*
	 * Start from the last open segments, to avoid redundant seeks.  We have
	 * previously verified that these segments are exactly RELSEG_SIZE long,
	 * and it's useless to recheck that each time.
	 *
	 * NOTE: this assumption could only be wrong if another backend has
	 * truncated the relation.  We rely on higher code levels to handle that
	 * scenario by closing and re-opening the md fd, which is handled via
	 * relcache flush.  (Since the checkpointer doesn't participate in
	 * relcache flush, it could have segment entries for inactive segments;
	 * that's OK because the checkpointer never needs to compute relation
	 * size.)
	 */
	segno = reln->md_num_open_segs[forknum] - 1;
	v = &reln->md_seg_fds[forknum][segno];

	for (;;)
	{
		nblocks = _mdnblocks(reln, forknum, v);
		if (nblocks > ((BlockNumber) RELSEG_SIZE))
			elog(FATAL, "segment too big");
		if (nblocks < ((BlockNumber) RELSEG_SIZE))
			return (segno * ((BlockNumber) RELSEG_SIZE)) + nblocks;

		/*
		 * If segment is exactly RELSEG_SIZE, advance to next one.
		 */
		segno++;

		/*
		 * We used to pass O_CREAT here, but that has the disadvantage that it
		 * might create a segment which has vanished through some operating
		 * system misadventure.  In such a case, creating the segment here
		 * undermines _mdfd_getseg's attempts to notice and report an error
		 * upon access to a missing segment.
		 */
		v = _mdfd_openseg(reln, forknum, segno, 0);
		if (v == NULL)
			return segno * ((BlockNumber) RELSEG_SIZE);
	}
}

/*
 *	mdtruncate() -- Truncate relation to specified number of blocks.
 */
void
mdtruncate(SMgrRelation reln, ForkNumber forknum, BlockNumber nblocks)
{
	BlockNumber curnblk;
	BlockNumber priorblocks;
	int			curopensegs;

	/*
	 * NOTE: mdnblocks makes sure we have opened all active segments, so that
	 * truncation loop will get them all!
	 */
	curnblk = mdnblocks(reln, forknum);
	if (nblocks > curnblk)
	{
		/* Bogus request ... but no complaint if InRecovery */
		if (InRecovery)
			return;
		ereport(ERROR,
				(errmsg("could not truncate file \"%s\" to %u blocks: it's only %u blocks now",
						relpath(reln->smgr_rnode, forknum),
						nblocks, curnblk)));
	}
	if (nblocks == curnblk)
		return;					/* no work */

	/*
	 * Truncate segments, starting at the last one. Starting at the end makes
	 * managing the memory for the fd array easier, should there be errors.
	 */
	curopensegs = reln->md_num_open_segs[forknum];
	while (curopensegs > 0)
	{
		MdfdVec    *v;

		priorblocks = (curopensegs - 1) * RELSEG_SIZE;

		v = &reln->md_seg_fds[forknum][curopensegs - 1];

		if (priorblocks > nblocks)
		{
			/*
			 * This segment is no longer active. We truncate the file, but do
			 * not delete it, for reasons explained in the header comments.
			 */
			if (FileTruncate(v->mdfd_vfd, 0, WAIT_EVENT_DATA_FILE_TRUNCATE) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not truncate file \"%s\": %m",
								FilePathName(v->mdfd_vfd))));

			if (!SmgrIsTemp(reln))
				register_dirty_segment(reln, forknum, v);

			/* we never drop the 1st segment */
			Assert(v != &reln->md_seg_fds[forknum][0]);

			FileClose(v->mdfd_vfd);
			_fdvec_resize(reln, forknum, curopensegs - 1);
		}
		else if (priorblocks + ((BlockNumber) RELSEG_SIZE) > nblocks)
		{
			/*
			 * This is the last segment we want to keep. Truncate the file to
			 * the right length. NOTE: if nblocks is exactly a multiple K of
			 * RELSEG_SIZE, we will truncate the K+1st segment to 0 length but
			 * keep it. This adheres to the invariant given in the header
			 * comments.
			 */
			BlockNumber lastsegblocks = nblocks - priorblocks;

			if (FileTruncate(v->mdfd_vfd, (off_t) lastsegblocks * BLCKSZ, WAIT_EVENT_DATA_FILE_TRUNCATE) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not truncate file \"%s\" to %u blocks: %m",
								FilePathName(v->mdfd_vfd),
								nblocks)));
			if (!SmgrIsTemp(reln))
				register_dirty_segment(reln, forknum, v);
		}
		else
		{
			/*
			 * We still need this segment, so nothing to do for this and any
			 * earlier segment.
			 */
			break;
		}
		curopensegs--;
	}
}

/*
 *	mdimmedsync() -- Immediately sync a relation to stable storage.
 *
 * Note that only writes already issued are synced; this routine knows
 * nothing of dirty buffers that may exist inside the buffer manager.  We
 * sync active and inactive segments; smgrDoPendingSyncs() relies on this.
 * Consider a relation skipping WAL.  Suppose a checkpoint syncs blocks of
 * some segment, then mdtruncate() renders that segment inactive.  If we
 * crash before the next checkpoint syncs the newly-inactive segment, that
 * segment may survive recovery, reintroducing unwanted data into the table.
 */
void
mdimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	int			segno;
	int			min_inactive_seg;

	/*
	 * NOTE: mdnblocks makes sure we have opened all active segments, so that
	 * fsync loop will get them all!
	 */
	mdnblocks(reln, forknum);

	min_inactive_seg = segno = reln->md_num_open_segs[forknum];

	/*
	 * Temporarily open inactive segments, then close them after sync.  There
	 * may be some inactive segments left opened after fsync() error, but that
	 * is harmless.  We don't bother to clean them up and take a risk of
	 * further trouble.  The next mdclose() will soon close them.
	 */
	while (_mdfd_openseg(reln, forknum, segno, 0) != NULL)
		segno++;

	while (segno > 0)
	{
		MdfdVec    *v = &reln->md_seg_fds[forknum][segno - 1];

		if (FileSync(v->mdfd_vfd, WAIT_EVENT_DATA_FILE_IMMEDIATE_SYNC) < 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m",
							FilePathName(v->mdfd_vfd))));

		/* Close inactive segments immediately */
		if (segno > min_inactive_seg)
		{
			FileClose(v->mdfd_vfd);
			_fdvec_resize(reln, forknum, segno - 1);
		}

		segno--;
	}
}

/*
 * register_dirty_segment() -- Mark a relation segment as needing fsync
 *
 * If there is a local pending-ops table, just make an entry in it for
 * ProcessSyncRequests to process later.  Otherwise, try to pass off the
 * fsync request to the checkpointer process.  If that fails, just do the
 * fsync locally before returning (we hope this will not happen often
 * enough to be a performance problem).
 */
static void
register_dirty_segment(SMgrRelation reln, ForkNumber forknum, MdfdVec *seg)
{
	FileTag		tag;

	INIT_MD_FILETAG(tag, reln->smgr_rnode.node, forknum, seg->mdfd_segno);

	/* Temp relations should never be fsync'd */
	Assert(!SmgrIsTemp(reln));

	if (!RegisterSyncRequest(&tag, SYNC_REQUEST, false /* retryOnError */ ))
	{
		ereport(DEBUG1,
				(errmsg_internal("could not forward fsync request because request queue is full")));

		if (FileSync(seg->mdfd_vfd, WAIT_EVENT_DATA_FILE_SYNC) < 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m",
							FilePathName(seg->mdfd_vfd))));
	}
}

/*
 * register_unlink_segment() -- Schedule a file to be deleted after next checkpoint
 */
static void
register_unlink_segment(RelFileNodeBackend rnode, ForkNumber forknum,
						BlockNumber segno)
{
	FileTag		tag;

	INIT_MD_FILETAG(tag, rnode.node, forknum, segno);

	/* Should never be used with temp relations */
	Assert(!RelFileNodeBackendIsTemp(rnode));

	RegisterSyncRequest(&tag, SYNC_UNLINK_REQUEST, true /* retryOnError */ );
}

/*
 * register_forget_request() -- forget any fsyncs for a relation fork's segment
 */
static void
register_forget_request(RelFileNodeBackend rnode, ForkNumber forknum,
						BlockNumber segno)
{
	FileTag		tag;

	INIT_MD_FILETAG(tag, rnode.node, forknum, segno);

	RegisterSyncRequest(&tag, SYNC_FORGET_REQUEST, true /* retryOnError */ );
}

/*
 * ForgetDatabaseSyncRequests -- forget any fsyncs and unlinks for a DB
 */
void
ForgetDatabaseSyncRequests(Oid dbid)
{
	FileTag		tag;
	RelFileNode rnode;

	rnode.dbNode = dbid;
	rnode.spcNode = 0;
	rnode.relNode = 0;

	INIT_MD_FILETAG(tag, rnode, InvalidForkNumber, InvalidBlockNumber);

	RegisterSyncRequest(&tag, SYNC_FILTER_REQUEST, true /* retryOnError */ );
}

/*
 * DropRelationFiles -- drop files of all given relations
 */
void
DropRelationFiles(RelFileNode *delrels, int ndelrels, bool isRedo)
{
	SMgrRelation *srels;
	int			i;

	srels = palloc(sizeof(SMgrRelation) * ndelrels);
	for (i = 0; i < ndelrels; i++)
	{
		SMgrRelation srel = smgropen(delrels[i], InvalidBackendId);

		if (isRedo)
		{
			ForkNumber	fork;

			for (fork = 0; fork <= MAX_FORKNUM; fork++)
				XLogDropRelation(delrels[i], fork);
		}
		srels[i] = srel;
	}

	smgrdounlinkall(srels, ndelrels, isRedo);

	for (i = 0; i < ndelrels; i++)
		smgrclose(srels[i]);
	pfree(srels);
}


/*
 *	_fdvec_resize() -- Resize the fork's open segments array
 */
static void
_fdvec_resize(SMgrRelation reln,
			  ForkNumber forknum,
			  int nseg)
{
	if (nseg == 0)
	{
		if (reln->md_num_open_segs[forknum] > 0)
		{
			pfree(reln->md_seg_fds[forknum]);
			reln->md_seg_fds[forknum] = NULL;
		}
	}
	else if (reln->md_num_open_segs[forknum] == 0)
	{
		reln->md_seg_fds[forknum] =
			MemoryContextAlloc(MdCxt, sizeof(MdfdVec) * nseg);
	}
	else
	{
		/*
		 * It doesn't seem worthwhile complicating the code to amortize
		 * repalloc() calls.  Those are far faster than PathNameOpenFile() or
		 * FileClose(), and the memory context internally will sometimes avoid
		 * doing an actual reallocation.
		 */
		reln->md_seg_fds[forknum] =
			repalloc(reln->md_seg_fds[forknum],
					 sizeof(MdfdVec) * nseg);
	}

	reln->md_num_open_segs[forknum] = nseg;
}

/*
 * Return the filename for the specified segment of the relation. The
 * returned string is palloc'd.
 */
static char *
_mdfd_segpath(SMgrRelation reln, ForkNumber forknum, BlockNumber segno)
{
	char	   *path,
			   *fullpath;

	path = relpath(reln->smgr_rnode, forknum);

	if (segno > 0)
	{
		fullpath = psprintf("%s.%u", path, segno);
		pfree(path);
	}
	else
		fullpath = path;

	return fullpath;
}

/*
 * Open the specified segment of the relation,
 * and make a MdfdVec object for it.  Returns NULL on failure.
 */
static MdfdVec *
_mdfd_openseg(SMgrRelation reln, ForkNumber forknum, BlockNumber segno,
			  int oflags)
{
	MdfdVec    *v;
	File		fd;
	char	   *fullpath;

	fullpath = _mdfd_segpath(reln, forknum, segno);

	/* open the file */
	fd = PathNameOpenFile(fullpath, O_RDWR | PG_BINARY | oflags);

	pfree(fullpath);

	if (fd < 0)
		return NULL;

	/*
	 * Segments are always opened in order from lowest to highest, so we must
	 * be adding a new one at the end.
	 */
	Assert(segno == reln->md_num_open_segs[forknum]);

	_fdvec_resize(reln, forknum, segno + 1);

	/* fill the entry */
	v = &reln->md_seg_fds[forknum][segno];
	v->mdfd_vfd = fd;
	v->mdfd_segno = segno;

	Assert(_mdnblocks(reln, forknum, v) <= ((BlockNumber) RELSEG_SIZE));

	/* all done */
	return v;
}

/*
 *	_mdfd_getseg() -- Find the segment of the relation holding the
 *		specified block.
 *
 * If the segment doesn't exist, we ereport, return NULL, or create the
 * segment, according to "behavior".  Note: skipFsync is only used in the
 * EXTENSION_CREATE case.
 */
static MdfdVec *
_mdfd_getseg(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
			 bool skipFsync, int behavior)
{
	MdfdVec    *v;
	BlockNumber targetseg;
	BlockNumber nextsegno;

	/* some way to handle non-existent segments needs to be specified */
	Assert(behavior &
		   (EXTENSION_FAIL | EXTENSION_CREATE | EXTENSION_RETURN_NULL));

	targetseg = blkno / ((BlockNumber) RELSEG_SIZE);

	/* if an existing and opened segment, we're done */
	if (targetseg < reln->md_num_open_segs[forknum])
	{
		v = &reln->md_seg_fds[forknum][targetseg];
		return v;
	}

	/*
	 * The target segment is not yet open. Iterate over all the segments
	 * between the last opened and the target segment. This way missing
	 * segments either raise an error, or get created (according to
	 * 'behavior'). Start with either the last opened, or the first segment if
	 * none was opened before.
	 */
	if (reln->md_num_open_segs[forknum] > 0)
		v = &reln->md_seg_fds[forknum][reln->md_num_open_segs[forknum] - 1];
	else
	{
		v = mdopenfork(reln, forknum, behavior);
		if (!v)
			return NULL;		/* if behavior & EXTENSION_RETURN_NULL */
	}

	for (nextsegno = reln->md_num_open_segs[forknum];
		 nextsegno <= targetseg; nextsegno++)
	{
		BlockNumber nblocks = _mdnblocks(reln, forknum, v);
		int			flags = 0;

		Assert(nextsegno == v->mdfd_segno + 1);

		if (nblocks > ((BlockNumber) RELSEG_SIZE))
			elog(FATAL, "segment too big");

		if ((behavior & EXTENSION_CREATE) ||
			(InRecovery && (behavior & EXTENSION_CREATE_RECOVERY)))
		{
			/*
			 * Normally we will create new segments only if authorized by the
			 * caller (i.e., we are doing mdextend()).  But when doing WAL
			 * recovery, create segments anyway; this allows cases such as
			 * replaying WAL data that has a write into a high-numbered
			 * segment of a relation that was later deleted. We want to go
			 * ahead and create the segments so we can finish out the replay.
			 *
			 * We have to maintain the invariant that segments before the last
			 * active segment are of size RELSEG_SIZE; therefore, if
			 * extending, pad them out with zeroes if needed.  (This only
			 * matters if in recovery, or if the caller is extending the
			 * relation discontiguously, but that can happen in hash indexes.)
			 */
			if (nblocks < ((BlockNumber) RELSEG_SIZE))
			{
				char	   *zerobuf = palloc0(BLCKSZ);

				mdextend(reln, forknum,
						 nextsegno * ((BlockNumber) RELSEG_SIZE) - 1,
						 zerobuf, skipFsync);
				pfree(zerobuf);
			}
			flags = O_CREAT;
		}
		else if (!(behavior & EXTENSION_DONT_CHECK_SIZE) &&
				 nblocks < ((BlockNumber) RELSEG_SIZE))
		{
			/*
			 * When not extending (or explicitly including truncated
			 * segments), only open the next segment if the current one is
			 * exactly RELSEG_SIZE.  If not (this branch), either return NULL
			 * or fail.
			 */
			if (behavior & EXTENSION_RETURN_NULL)
			{
				/*
				 * Some callers discern between reasons for _mdfd_getseg()
				 * returning NULL based on errno. As there's no failing
				 * syscall involved in this case, explicitly set errno to
				 * ENOENT, as that seems the closest interpretation.
				 */
				errno = ENOENT;
				return NULL;
			}

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" (target block %u): previous segment is only %u blocks",
							_mdfd_segpath(reln, forknum, nextsegno),
							blkno, nblocks)));
		}

		v = _mdfd_openseg(reln, forknum, nextsegno, flags);

		if (v == NULL)
		{
			if ((behavior & EXTENSION_RETURN_NULL) &&
				FILE_POSSIBLY_DELETED(errno))
				return NULL;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" (target block %u): %m",
							_mdfd_segpath(reln, forknum, nextsegno),
							blkno)));
		}
	}

	return v;
}

/*
 * Get number of blocks present in a single disk file
 */
static BlockNumber
_mdnblocks(SMgrRelation reln, ForkNumber forknum, MdfdVec *seg)
{
	off_t		len;

	len = FileSize(seg->mdfd_vfd);
	if (len < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to end of file \"%s\": %m",
						FilePathName(seg->mdfd_vfd))));
	/* note that this calculation will ignore any partial block at EOF */
	return (BlockNumber) (len / BLCKSZ);
}

/*
 * Sync a file to disk, given a file tag.  Write the path into an output
 * buffer so the caller can use it in error messages.
 *
 * Return 0 on success, -1 on failure, with errno set.
 */
int
mdsyncfiletag(const FileTag *ftag, char *path)
{
	SMgrRelation reln = smgropen(ftag->rnode, InvalidBackendId);
	File		file;
	bool		need_to_close;
	int			result,
				save_errno;

	/* See if we already have the file open, or need to open it. */
	if (ftag->segno < reln->md_num_open_segs[ftag->forknum])
	{
		file = reln->md_seg_fds[ftag->forknum][ftag->segno].mdfd_vfd;
		strlcpy(path, FilePathName(file), MAXPGPATH);
		need_to_close = false;
	}
	else
	{
		char	   *p;

		p = _mdfd_segpath(reln, ftag->forknum, ftag->segno);
		strlcpy(path, p, MAXPGPATH);
		pfree(p);

		file = PathNameOpenFile(path, O_RDWR | PG_BINARY);
		if (file < 0)
			return -1;
		need_to_close = true;
	}

	/* Sync the file. */
	result = FileSync(file, WAIT_EVENT_DATA_FILE_SYNC);
	save_errno = errno;

	if (need_to_close)
		FileClose(file);

	errno = save_errno;
	return result;
}

/*
 * Unlink a file, given a file tag.  Write the path into an output
 * buffer so the caller can use it in error messages.
 *
 * Return 0 on success, -1 on failure, with errno set.
 */
int
mdunlinkfiletag(const FileTag *ftag, char *path)
{
	char	   *p;

	/* Compute the path. */
	p = relpathperm(ftag->rnode, MAIN_FORKNUM);
	strlcpy(path, p, MAXPGPATH);
	pfree(p);

	/* Try to unlink the file. */
	return unlink(path);
}

/*
 * Check if a given candidate request matches a given tag, when processing
 * a SYNC_FILTER_REQUEST request.  This will be called for all pending
 * requests to find out whether to forget them.
 */
bool
mdfiletagmatches(const FileTag *ftag, const FileTag *candidate)
{
	/*
	 * For now we only use filter requests as a way to drop all scheduled
	 * callbacks relating to a given database, when dropping the database.
	 * We'll return true for all candidates that have the same database OID as
	 * the ftag from the SYNC_FILTER_REQUEST request, so they're forgotten.
	 */
	return ftag->rnode.dbNode == candidate->rnode.dbNode;
}
