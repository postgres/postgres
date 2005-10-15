/*-------------------------------------------------------------------------
 *
 * md.c
 *	  This code manages relations that reside on magnetic disk.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/smgr/md.c,v 1.118 2005/10/15 02:49:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "catalog/catalog.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"


/*
 *	The magnetic disk storage manager keeps track of open file
 *	descriptors in its own descriptor pool.  This is done to make it
 *	easier to support relations that are larger than the operating
 *	system's file size limit (often 2GBytes).  In order to do that,
 *	we break relations up into chunks of < 2GBytes and store one chunk
 *	in each of several files that represent the relation.  See the
 *	BLCKSZ and RELSEG_SIZE configuration constants in pg_config_manual.h.
 *	All chunks except the last MUST have size exactly equal to RELSEG_SIZE
 *	blocks --- see mdnblocks() and mdtruncate().
 *
 *	The file descriptor pointer (md_fd field) stored in the SMgrRelation
 *	cache is, therefore, just the head of a list of MdfdVec objects.
 *	But note the md_fd pointer can be NULL, indicating relation not open.
 *
 *	Note that mdfd_chain == NULL does not necessarily mean the relation
 *	doesn't have another segment after this one; we may just not have
 *	opened the next segment yet.  (We could not have "all segments are
 *	in the chain" as an invariant anyway, since another backend could
 *	extend the relation when we weren't looking.)
 *
 *	All MdfdVec objects are palloc'd in the MdCxt memory context.
 */

typedef struct _MdfdVec
{
	File		mdfd_vfd;		/* fd number in fd.c's pool */
	BlockNumber mdfd_segno;		/* segment number, from 0 */
#ifndef LET_OS_MANAGE_FILESIZE	/* for large relations */
	struct _MdfdVec *mdfd_chain;	/* next segment, or NULL */
#endif
} MdfdVec;

static MemoryContext MdCxt;		/* context for all md.c allocations */


/*
 * In some contexts (currently, standalone backends and the bgwriter process)
 * we keep track of pending fsync operations: we need to remember all relation
 * segments that have been written since the last checkpoint, so that we can
 * fsync them down to disk before completing the next checkpoint.  This hash
 * table remembers the pending operations.	We use a hash table not because
 * we want to look up individual operations, but simply as a convenient way
 * of eliminating duplicate requests.
 *
 * (Regular backends do not track pending operations locally, but forward
 * them to the bgwriter.)
 *
 * XXX for WIN32, may want to expand this to track pending deletes, too.
 */
typedef struct
{
	RelFileNode rnode;			/* the targeted relation */
	BlockNumber segno;			/* which segment */
} PendingOperationEntry;

static HTAB *pendingOpsTable = NULL;


/* local routines */
static MdfdVec *mdopen(SMgrRelation reln, bool allowNotFound);
static bool register_dirty_segment(SMgrRelation reln, MdfdVec *seg);
static MdfdVec *_fdvec_alloc(void);

#ifndef LET_OS_MANAGE_FILESIZE
static MdfdVec *_mdfd_openseg(SMgrRelation reln, BlockNumber segno,
			  int oflags);
#endif
static MdfdVec *_mdfd_getseg(SMgrRelation reln, BlockNumber blkno,
			 bool allowNotFound);
static BlockNumber _mdnblocks(File file, Size blcksz);


/*
 *	mdinit() -- Initialize private state for magnetic disk storage manager.
 */
bool
mdinit(void)
{
	MdCxt = AllocSetContextCreate(TopMemoryContext,
								  "MdSmgr",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * Create pending-operations hashtable if we need it.  Currently, we need
	 * it if we are standalone (not under a postmaster) OR if we are a
	 * bootstrap-mode subprocess of a postmaster (that is, a startup or
	 * bgwriter process).
	 */
	if (!IsUnderPostmaster || IsBootstrapProcessingMode())
	{
		HASHCTL		hash_ctl;

		MemSet(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = sizeof(PendingOperationEntry);
		hash_ctl.entrysize = sizeof(PendingOperationEntry);
		hash_ctl.hash = tag_hash;
		hash_ctl.hcxt = MdCxt;
		pendingOpsTable = hash_create("Pending Ops Table",
									  100L,
									  &hash_ctl,
								   HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
	}

	return true;
}

/*
 *	mdcreate() -- Create a new relation on magnetic disk.
 *
 * If isRedo is true, it's okay for the relation to exist already.
 */
bool
mdcreate(SMgrRelation reln, bool isRedo)
{
	char	   *path;
	File		fd;

	if (isRedo && reln->md_fd != NULL)
		return true;			/* created and opened already... */

	Assert(reln->md_fd == NULL);

	path = relpath(reln->smgr_rnode);

	fd = PathNameOpenFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, 0600);

	if (fd < 0)
	{
		int			save_errno = errno;

		/*
		 * During bootstrap, there are cases where a system relation will be
		 * accessed (by internal backend processes) before the bootstrap
		 * script nominally creates it.  Therefore, allow the file to exist
		 * already, even if isRedo is not set.	(See also mdopen)
		 */
		if (isRedo || IsBootstrapProcessingMode())
			fd = PathNameOpenFile(path, O_RDWR | PG_BINARY, 0600);
		if (fd < 0)
		{
			pfree(path);
			/* be sure to return the error reported by create, not open */
			errno = save_errno;
			return false;
		}
		errno = 0;
	}

	pfree(path);

	reln->md_fd = _fdvec_alloc();

	reln->md_fd->mdfd_vfd = fd;
	reln->md_fd->mdfd_segno = 0;
#ifndef LET_OS_MANAGE_FILESIZE
	reln->md_fd->mdfd_chain = NULL;
#endif

	return true;
}

/*
 *	mdunlink() -- Unlink a relation.
 *
 * Note that we're passed a RelFileNode --- by the time this is called,
 * there won't be an SMgrRelation hashtable entry anymore.
 *
 * If isRedo is true, it's okay for the relation to be already gone.
 */
bool
mdunlink(RelFileNode rnode, bool isRedo)
{
	bool		status = true;
	int			save_errno = 0;
	char	   *path;

	path = relpath(rnode);

	/* Delete the first segment, or only segment if not doing segmenting */
	if (unlink(path) < 0)
	{
		if (!isRedo || errno != ENOENT)
		{
			status = false;
			save_errno = errno;
		}
	}

#ifndef LET_OS_MANAGE_FILESIZE
	/* Get the additional segments, if any */
	if (status)
	{
		char	   *segpath = (char *) palloc(strlen(path) + 12);
		BlockNumber segno;

		for (segno = 1;; segno++)
		{
			sprintf(segpath, "%s.%u", path, segno);
			if (unlink(segpath) < 0)
			{
				/* ENOENT is expected after the last segment... */
				if (errno != ENOENT)
				{
					status = false;
					save_errno = errno;
				}
				break;
			}
		}
		pfree(segpath);
	}
#endif

	pfree(path);

	errno = save_errno;
	return status;
}

/*
 *	mdextend() -- Add a block to the specified relation.
 *
 *		The semantics are basically the same as mdwrite(): write at the
 *		specified position.  However, we are expecting to extend the
 *		relation (ie, blocknum is the current EOF), and so in case of
 *		failure we clean up by truncating.
 *
 *		This routine returns true or false, with errno set as appropriate.
 *
 * Note: this routine used to call mdnblocks() to get the block position
 * to write at, but that's pretty silly since the caller needs to know where
 * the block will be written, and accordingly must have done mdnblocks()
 * already.  Might as well pass in the position and save a seek.
 */
bool
mdextend(SMgrRelation reln, BlockNumber blocknum, char *buffer, bool isTemp)
{
	long		seekpos;
	int			nbytes;
	MdfdVec    *v;

	v = _mdfd_getseg(reln, blocknum, false);

#ifndef LET_OS_MANAGE_FILESIZE
	seekpos = (long) (BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE)));
	Assert(seekpos < BLCKSZ * RELSEG_SIZE);
#else
	seekpos = (long) (BLCKSZ * (blocknum));
#endif

	/*
	 * Note: because caller obtained blocknum by calling _mdnblocks, which did
	 * a seek(SEEK_END), this seek is often redundant and will be optimized
	 * away by fd.c.  It's not redundant, however, if there is a partial page
	 * at the end of the file.	In that case we want to try to overwrite the
	 * partial page with a full page.  It's also not redundant if bufmgr.c had
	 * to dump another buffer of the same file to make room for the new page's
	 * buffer.
	 */
	if (FileSeek(v->mdfd_vfd, seekpos, SEEK_SET) != seekpos)
		return false;

	if ((nbytes = FileWrite(v->mdfd_vfd, buffer, BLCKSZ)) != BLCKSZ)
	{
		if (nbytes > 0)
		{
			int			save_errno = errno;

			/* Remove the partially-written page */
			FileTruncate(v->mdfd_vfd, seekpos);
			FileSeek(v->mdfd_vfd, seekpos, SEEK_SET);
			errno = save_errno;
		}
		return false;
	}

	if (!isTemp)
	{
		if (!register_dirty_segment(reln, v))
			return false;
	}

#ifndef LET_OS_MANAGE_FILESIZE
	Assert(_mdnblocks(v->mdfd_vfd, BLCKSZ) <= ((BlockNumber) RELSEG_SIZE));
#endif

	return true;
}

/*
 *	mdopen() -- Open the specified relation.  ereport's on failure.
 *		(Optionally, can return NULL instead of ereport for ENOENT.)
 *
 * Note we only open the first segment, when there are multiple segments.
 */
static MdfdVec *
mdopen(SMgrRelation reln, bool allowNotFound)
{
	MdfdVec    *mdfd;
	char	   *path;
	File		fd;

	/* No work if already open */
	if (reln->md_fd)
		return reln->md_fd;

	path = relpath(reln->smgr_rnode);

	fd = PathNameOpenFile(path, O_RDWR | PG_BINARY, 0600);

	if (fd < 0)
	{
		/*
		 * During bootstrap, there are cases where a system relation will be
		 * accessed (by internal backend processes) before the bootstrap
		 * script nominally creates it.  Therefore, accept mdopen() as a
		 * substitute for mdcreate() in bootstrap mode only. (See mdcreate)
		 */
		if (IsBootstrapProcessingMode())
			fd = PathNameOpenFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, 0600);
		if (fd < 0)
		{
			pfree(path);
			if (allowNotFound && errno == ENOENT)
				return NULL;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open relation %u/%u/%u: %m",
							reln->smgr_rnode.spcNode,
							reln->smgr_rnode.dbNode,
							reln->smgr_rnode.relNode)));
		}
	}

	pfree(path);

	reln->md_fd = mdfd = _fdvec_alloc();

	mdfd->mdfd_vfd = fd;
	mdfd->mdfd_segno = 0;
#ifndef LET_OS_MANAGE_FILESIZE
	mdfd->mdfd_chain = NULL;
	Assert(_mdnblocks(fd, BLCKSZ) <= ((BlockNumber) RELSEG_SIZE));
#endif

	return mdfd;
}

/*
 *	mdclose() -- Close the specified relation, if it isn't closed already.
 *
 *		Returns true or false with errno set as appropriate.
 */
bool
mdclose(SMgrRelation reln)
{
	MdfdVec    *v = reln->md_fd;

	/* No work if already closed */
	if (v == NULL)
		return true;

	reln->md_fd = NULL;			/* prevent dangling pointer after error */

#ifndef LET_OS_MANAGE_FILESIZE
	while (v != NULL)
	{
		MdfdVec    *ov = v;

		/* if not closed already */
		if (v->mdfd_vfd >= 0)
			FileClose(v->mdfd_vfd);
		/* Now free vector */
		v = v->mdfd_chain;
		pfree(ov);
	}
#else
	if (v->mdfd_vfd >= 0)
		FileClose(v->mdfd_vfd);
	pfree(v);
#endif

	return true;
}

/*
 *	mdread() -- Read the specified block from a relation.
 */
bool
mdread(SMgrRelation reln, BlockNumber blocknum, char *buffer)
{
	bool		status;
	long		seekpos;
	int			nbytes;
	MdfdVec    *v;

	v = _mdfd_getseg(reln, blocknum, false);

#ifndef LET_OS_MANAGE_FILESIZE
	seekpos = (long) (BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE)));
	Assert(seekpos < BLCKSZ * RELSEG_SIZE);
#else
	seekpos = (long) (BLCKSZ * (blocknum));
#endif

	if (FileSeek(v->mdfd_vfd, seekpos, SEEK_SET) != seekpos)
		return false;

	status = true;
	if ((nbytes = FileRead(v->mdfd_vfd, buffer, BLCKSZ)) != BLCKSZ)
	{
		/*
		 * If we are at or past EOF, return zeroes without complaining. Also
		 * substitute zeroes if we found a partial block at EOF.
		 *
		 * XXX this is really ugly, bad design.  However the current
		 * implementation of hash indexes requires it, because hash index
		 * pages are initialized out-of-order.
		 */
		if (nbytes == 0 ||
			(nbytes > 0 && mdnblocks(reln) == blocknum))
			MemSet(buffer, 0, BLCKSZ);
		else
			status = false;
	}

	return status;
}

/*
 *	mdwrite() -- Write the supplied block at the appropriate location.
 */
bool
mdwrite(SMgrRelation reln, BlockNumber blocknum, char *buffer, bool isTemp)
{
	long		seekpos;
	MdfdVec    *v;

	v = _mdfd_getseg(reln, blocknum, false);

#ifndef LET_OS_MANAGE_FILESIZE
	seekpos = (long) (BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE)));
	Assert(seekpos < BLCKSZ * RELSEG_SIZE);
#else
	seekpos = (long) (BLCKSZ * (blocknum));
#endif

	if (FileSeek(v->mdfd_vfd, seekpos, SEEK_SET) != seekpos)
		return false;

	if (FileWrite(v->mdfd_vfd, buffer, BLCKSZ) != BLCKSZ)
		return false;

	if (!isTemp)
	{
		if (!register_dirty_segment(reln, v))
			return false;
	}

	return true;
}

/*
 *	mdnblocks() -- Get the number of blocks stored in a relation.
 *
 *		Important side effect: all segments of the relation are opened
 *		and added to the mdfd_chain list.  If this routine has not been
 *		called, then only segments up to the last one actually touched
 *		are present in the chain...
 *
 *		Returns # of blocks, or InvalidBlockNumber on error.
 */
BlockNumber
mdnblocks(SMgrRelation reln)
{
	MdfdVec    *v = mdopen(reln, false);

#ifndef LET_OS_MANAGE_FILESIZE
	BlockNumber nblocks;
	BlockNumber segno = 0;

	/*
	 * Skip through any segments that aren't the last one, to avoid redundant
	 * seeks on them.  We have previously verified that these segments are
	 * exactly RELSEG_SIZE long, and it's useless to recheck that each time.
	 * (NOTE: this assumption could only be wrong if another backend has
	 * truncated the relation.	We rely on higher code levels to handle that
	 * scenario by closing and re-opening the md fd.)
	 */
	while (v->mdfd_chain != NULL)
	{
		segno++;
		v = v->mdfd_chain;
	}

	for (;;)
	{
		nblocks = _mdnblocks(v->mdfd_vfd, BLCKSZ);
		if (nblocks > ((BlockNumber) RELSEG_SIZE))
			elog(FATAL, "segment too big");
		if (nblocks < ((BlockNumber) RELSEG_SIZE))
			return (segno * ((BlockNumber) RELSEG_SIZE)) + nblocks;

		/*
		 * If segment is exactly RELSEG_SIZE, advance to next one.
		 */
		segno++;

		if (v->mdfd_chain == NULL)
		{
			/*
			 * Because we pass O_CREAT, we will create the next segment (with
			 * zero length) immediately, if the last segment is of length
			 * REL_SEGSIZE.  This is unnecessary but harmless, and testing for
			 * the case would take more cycles than it seems worth.
			 */
			v->mdfd_chain = _mdfd_openseg(reln, segno, O_CREAT);
			if (v->mdfd_chain == NULL)
				return InvalidBlockNumber;		/* failed? */
		}

		v = v->mdfd_chain;
	}
#else
	return _mdnblocks(v->mdfd_vfd, BLCKSZ);
#endif
}

/*
 *	mdtruncate() -- Truncate relation to specified number of blocks.
 *
 *		Returns # of blocks or InvalidBlockNumber on error.
 */
BlockNumber
mdtruncate(SMgrRelation reln, BlockNumber nblocks, bool isTemp)
{
	MdfdVec    *v;
	BlockNumber curnblk;

#ifndef LET_OS_MANAGE_FILESIZE
	BlockNumber priorblocks;
#endif

	/*
	 * NOTE: mdnblocks makes sure we have opened all existing segments, so
	 * that truncate/delete loop will get them all!
	 */
	curnblk = mdnblocks(reln);
	if (curnblk == InvalidBlockNumber)
		return InvalidBlockNumber;		/* mdnblocks failed */
	if (nblocks > curnblk)
		return InvalidBlockNumber;		/* bogus request */
	if (nblocks == curnblk)
		return nblocks;			/* no work */

	v = mdopen(reln, false);

#ifndef LET_OS_MANAGE_FILESIZE
	priorblocks = 0;
	while (v != NULL)
	{
		MdfdVec    *ov = v;

		if (priorblocks > nblocks)
		{
			/*
			 * This segment is no longer wanted at all (and has already been
			 * unlinked from the mdfd_chain). We truncate the file before
			 * deleting it because if other backends are holding the file
			 * open, the unlink will fail on some platforms. Better a
			 * zero-size file gets left around than a big file...
			 */
			FileTruncate(v->mdfd_vfd, 0);
			FileUnlink(v->mdfd_vfd);
			v = v->mdfd_chain;
			Assert(ov != reln->md_fd);	/* we never drop the 1st segment */
			pfree(ov);
		}
		else if (priorblocks + ((BlockNumber) RELSEG_SIZE) > nblocks)
		{
			/*
			 * This is the last segment we want to keep. Truncate the file to
			 * the right length, and clear chain link that points to any
			 * remaining segments (which we shall zap). NOTE: if nblocks is
			 * exactly a multiple K of RELSEG_SIZE, we will truncate the K+1st
			 * segment to 0 length but keep it. This is mainly so that the
			 * right thing happens if nblocks==0.
			 */
			BlockNumber lastsegblocks = nblocks - priorblocks;

			if (FileTruncate(v->mdfd_vfd, lastsegblocks * BLCKSZ) < 0)
				return InvalidBlockNumber;
			if (!isTemp)
			{
				if (!register_dirty_segment(reln, v))
					return InvalidBlockNumber;
			}
			v = v->mdfd_chain;
			ov->mdfd_chain = NULL;
		}
		else
		{
			/*
			 * We still need this segment and 0 or more blocks beyond it, so
			 * nothing to do here.
			 */
			v = v->mdfd_chain;
		}
		priorblocks += RELSEG_SIZE;
	}
#else
	if (FileTruncate(v->mdfd_vfd, nblocks * BLCKSZ) < 0)
		return InvalidBlockNumber;
	if (!isTemp)
	{
		if (!register_dirty_segment(reln, v))
			return InvalidBlockNumber;
	}
#endif

	return nblocks;
}

/*
 *	mdimmedsync() -- Immediately sync a relation to stable storage.
 *
 * Note that only writes already issued are synced; this routine knows
 * nothing of dirty buffers that may exist inside the buffer manager.
 */
bool
mdimmedsync(SMgrRelation reln)
{
	MdfdVec    *v;
	BlockNumber curnblk;

	/*
	 * NOTE: mdnblocks makes sure we have opened all existing segments, so
	 * that fsync loop will get them all!
	 */
	curnblk = mdnblocks(reln);
	if (curnblk == InvalidBlockNumber)
		return false;			/* mdnblocks failed */

	v = mdopen(reln, false);

#ifndef LET_OS_MANAGE_FILESIZE
	while (v != NULL)
	{
		if (FileSync(v->mdfd_vfd) < 0)
			return false;
		v = v->mdfd_chain;
	}
#else
	if (FileSync(v->mdfd_vfd) < 0)
		return false;
#endif

	return true;
}

/*
 *	mdsync() -- Sync previous writes to stable storage.
 *
 * This is only called during checkpoints, and checkpoints should only
 * occur in processes that have created a pendingOpsTable.
 */
bool
mdsync(void)
{
	HASH_SEQ_STATUS hstat;
	PendingOperationEntry *entry;

	if (!pendingOpsTable)
		return false;

	/*
	 * If we are in the bgwriter, the sync had better include all fsync
	 * requests that were queued by backends before the checkpoint REDO point
	 * was determined.	We go that a little better by accepting all requests
	 * queued up to the point where we start fsync'ing.
	 */
	AbsorbFsyncRequests();

	hash_seq_init(&hstat, pendingOpsTable);
	while ((entry = (PendingOperationEntry *) hash_seq_search(&hstat)) != NULL)
	{
		/*
		 * If fsync is off then we don't have to bother opening the file at
		 * all.  (We delay checking until this point so that changing fsync on
		 * the fly behaves sensibly.)
		 */
		if (enableFsync)
		{
			SMgrRelation reln;
			MdfdVec    *seg;

			/*
			 * Find or create an smgr hash entry for this relation. This may
			 * seem a bit unclean -- md calling smgr?  But it's really the
			 * best solution.  It ensures that the open file reference isn't
			 * permanently leaked if we get an error here. (You may say "but
			 * an unreferenced SMgrRelation is still a leak!" Not really,
			 * because the only case in which a checkpoint is done by a
			 * process that isn't about to shut down is in the bgwriter, and
			 * it will periodically do smgrcloseall().	This fact justifies
			 * our not closing the reln in the success path either, which is a
			 * good thing since in non-bgwriter cases we couldn't safely do
			 * that.)  Furthermore, in many cases the relation will have been
			 * dirtied through this same smgr relation, and so we can save a
			 * file open/close cycle.
			 */
			reln = smgropen(entry->rnode);

			/*
			 * It is possible that the relation has been dropped or truncated
			 * since the fsync request was entered.  Therefore, we have to
			 * allow file-not-found errors.  This applies both during
			 * _mdfd_getseg() and during FileSync, since fd.c might have
			 * closed the file behind our back.
			 */
			seg = _mdfd_getseg(reln,
							   entry->segno * ((BlockNumber) RELSEG_SIZE),
							   true);
			if (seg)
			{
				if (FileSync(seg->mdfd_vfd) < 0 &&
					errno != ENOENT)
				{
					ereport(LOG,
							(errcode_for_file_access(),
							 errmsg("could not fsync segment %u of relation %u/%u/%u: %m",
									entry->segno,
									entry->rnode.spcNode,
									entry->rnode.dbNode,
									entry->rnode.relNode)));
					return false;
				}
			}
		}

		/* Okay, delete this entry */
		if (hash_search(pendingOpsTable, entry,
						HASH_REMOVE, NULL) == NULL)
			elog(ERROR, "pendingOpsTable corrupted");
	}

	return true;
}

/*
 * register_dirty_segment() -- Mark a relation segment as needing fsync
 *
 * If there is a local pending-ops table, just make an entry in it for
 * mdsync to process later.  Otherwise, try to pass off the fsync request
 * to the background writer process.  If that fails, just do the fsync
 * locally before returning (we expect this will not happen often enough
 * to be a performance problem).
 *
 * A false result implies I/O failure during local fsync.  errno will be
 * valid for error reporting.
 */
static bool
register_dirty_segment(SMgrRelation reln, MdfdVec *seg)
{
	if (pendingOpsTable)
	{
		PendingOperationEntry entry;

		/* ensure any pad bytes in the struct are zeroed */
		MemSet(&entry, 0, sizeof(entry));
		entry.rnode = reln->smgr_rnode;
		entry.segno = seg->mdfd_segno;

		(void) hash_search(pendingOpsTable, &entry, HASH_ENTER, NULL);
		return true;
	}
	else
	{
		if (ForwardFsyncRequest(reln->smgr_rnode, seg->mdfd_segno))
			return true;
	}

	if (FileSync(seg->mdfd_vfd) < 0)
		return false;
	return true;
}

/*
 * RememberFsyncRequest() -- callback from bgwriter side of fsync request
 *
 * We stuff the fsync request into the local hash table for execution
 * during the bgwriter's next checkpoint.
 */
void
RememberFsyncRequest(RelFileNode rnode, BlockNumber segno)
{
	PendingOperationEntry entry;

	Assert(pendingOpsTable);

	/* ensure any pad bytes in the struct are zeroed */
	MemSet(&entry, 0, sizeof(entry));
	entry.rnode = rnode;
	entry.segno = segno;

	(void) hash_search(pendingOpsTable, &entry, HASH_ENTER, NULL);
}

/*
 *	_fdvec_alloc() -- Make a MdfdVec object.
 */
static MdfdVec *
_fdvec_alloc(void)
{
	return (MdfdVec *) MemoryContextAlloc(MdCxt, sizeof(MdfdVec));
}

#ifndef LET_OS_MANAGE_FILESIZE

/*
 * Open the specified segment of the relation,
 * and make a MdfdVec object for it.  Returns NULL on failure.
 */
static MdfdVec *
_mdfd_openseg(SMgrRelation reln, BlockNumber segno, int oflags)
{
	MdfdVec    *v;
	int			fd;
	char	   *path,
			   *fullpath;

	path = relpath(reln->smgr_rnode);

	if (segno > 0)
	{
		/* be sure we have enough space for the '.segno' */
		fullpath = (char *) palloc(strlen(path) + 12);
		sprintf(fullpath, "%s.%u", path, segno);
		pfree(path);
	}
	else
		fullpath = path;

	/* open the file */
	fd = PathNameOpenFile(fullpath, O_RDWR | PG_BINARY | oflags, 0600);

	pfree(fullpath);

	if (fd < 0)
		return NULL;

	/* allocate an mdfdvec entry for it */
	v = _fdvec_alloc();

	/* fill the entry */
	v->mdfd_vfd = fd;
	v->mdfd_segno = segno;
	v->mdfd_chain = NULL;
	Assert(_mdnblocks(fd, BLCKSZ) <= ((BlockNumber) RELSEG_SIZE));

	/* all done */
	return v;
}
#endif   /* LET_OS_MANAGE_FILESIZE */

/*
 *	_mdfd_getseg() -- Find the segment of the relation holding the
 *		specified block.  ereport's on failure.
 *		(Optionally, can return NULL instead of ereport for ENOENT.)
 */
static MdfdVec *
_mdfd_getseg(SMgrRelation reln, BlockNumber blkno, bool allowNotFound)
{
	MdfdVec    *v = mdopen(reln, allowNotFound);

#ifndef LET_OS_MANAGE_FILESIZE
	BlockNumber segstogo;
	BlockNumber nextsegno;

	if (!v)
		return NULL;			/* only possible if allowNotFound */

	for (segstogo = blkno / ((BlockNumber) RELSEG_SIZE), nextsegno = 1;
		 segstogo > 0;
		 nextsegno++, segstogo--)
	{
		if (v->mdfd_chain == NULL)
		{
			/*
			 * We will create the next segment only if the target block is
			 * within it.  This prevents Sorcerer's Apprentice syndrome if a
			 * bug at higher levels causes us to be handed a ridiculously
			 * large blkno --- otherwise we could create many thousands of
			 * empty segment files before reaching the "target" block.	We
			 * should never need to create more than one new segment per call,
			 * so this restriction seems reasonable.
			 *
			 * BUT: when doing WAL recovery, disable this logic and create
			 * segments unconditionally.  In this case it seems better to
			 * assume the given blkno is good (it presumably came from a
			 * CRC-checked WAL record); furthermore this lets us cope in the
			 * case where we are replaying WAL data that has a write into a
			 * high-numbered segment of a relation that was later deleted.	We
			 * want to go ahead and create the segments so we can finish out
			 * the replay.
			 */
			v->mdfd_chain = _mdfd_openseg(reln,
										  nextsegno,
								(segstogo == 1 || InRecovery) ? O_CREAT : 0);
			if (v->mdfd_chain == NULL)
			{
				if (allowNotFound && errno == ENOENT)
					return NULL;
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open segment %u of relation %u/%u/%u (target block %u): %m",
								nextsegno,
								reln->smgr_rnode.spcNode,
								reln->smgr_rnode.dbNode,
								reln->smgr_rnode.relNode,
								blkno)));
			}
		}
		v = v->mdfd_chain;
	}
#endif

	return v;
}

/*
 * Get number of blocks present in a single disk file
 */
static BlockNumber
_mdnblocks(File file, Size blcksz)
{
	long		len;

	len = FileSeek(file, 0L, SEEK_END);
	if (len < 0)
		return 0;				/* on failure, assume file is empty */
	return (BlockNumber) (len / blcksz);
}
