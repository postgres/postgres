/*-------------------------------------------------------------------------
 *
 * md.c
 *	  This code manages relations that reside on magnetic disk.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/smgr/md.c,v 1.103 2004/02/11 22:55:25 tgl Exp $
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
#include "storage/fd.h"
#include "storage/smgr.h"
#include "utils/memutils.h"


/*
 *	The magnetic disk storage manager keeps track of open file
 *	descriptors in its own descriptor pool.  This is done to make it
 *	easier to support relations that are larger than the operating
 *	system's file size limit (often 2GBytes).  In order to do that,
 *	we break relations up into chunks of < 2GBytes and store one chunk
 *	in each of several files that represent the relation.  See the
 *	BLCKSZ and RELSEG_SIZE configuration constants in
 *	include/pg_config.h.  All chunks except the last MUST have size exactly
 *	equal to RELSEG_SIZE blocks --- see mdnblocks() and mdtruncate().
 *
 *	The file descriptor pointer (md_fd field) stored in the SMgrRelation
 *	cache is, therefore, just the head of a list of MdfdVec objects.
 *	But note the md_fd pointer can be NULL, indicating relation not open.
 *
 *	All MdfdVec objects are palloc'd in the MdCxt memory context.
 */

typedef struct _MdfdVec
{
	File		mdfd_vfd;			/* fd number in fd.c's pool */

#ifndef LET_OS_MANAGE_FILESIZE
	struct _MdfdVec *mdfd_chain;	/* for large relations */
#endif
} MdfdVec;

static MemoryContext MdCxt;		/* context for all md.c allocations */


/* routines declared here */
static MdfdVec *mdopen(SMgrRelation reln);
static MdfdVec *_fdvec_alloc(void);
#ifndef LET_OS_MANAGE_FILESIZE
static MdfdVec *_mdfd_openseg(SMgrRelation reln, BlockNumber segno,
							  int oflags);
#endif
static MdfdVec *_mdfd_getseg(SMgrRelation reln, BlockNumber blkno);
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

	fd = FileNameOpenFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, 0600);

	if (fd < 0)
	{
		int			save_errno = errno;

		/*
		 * During bootstrap, there are cases where a system relation will
		 * be accessed (by internal backend processes) before the
		 * bootstrap script nominally creates it.  Therefore, allow the
		 * file to exist already, even if isRedo is not set.  (See also
		 * mdopen)
		 */
		if (isRedo || IsBootstrapProcessingMode())
			fd = FileNameOpenFile(path, O_RDWR | PG_BINARY, 0600);
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
mdextend(SMgrRelation reln, BlockNumber blocknum, char *buffer)
{
	long		seekpos;
	int			nbytes;
	MdfdVec    *v;

	v = _mdfd_getseg(reln, blocknum);

#ifndef LET_OS_MANAGE_FILESIZE
	seekpos = (long) (BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE)));
	Assert(seekpos < BLCKSZ * RELSEG_SIZE);
#else
	seekpos = (long) (BLCKSZ * (blocknum));
#endif

	/*
	 * Note: because caller obtained blocknum by calling _mdnblocks, which
	 * did a seek(SEEK_END), this seek is often redundant and will be
	 * optimized away by fd.c.	It's not redundant, however, if there is a
	 * partial page at the end of the file.  In that case we want to try
	 * to overwrite the partial page with a full page.	It's also not
	 * redundant if bufmgr.c had to dump another buffer of the same file
	 * to make room for the new page's buffer.
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

#ifndef LET_OS_MANAGE_FILESIZE
	Assert(_mdnblocks(v->mdfd_vfd, BLCKSZ) <= ((BlockNumber) RELSEG_SIZE));
#endif

	return true;
}

/*
 *	mdopen() -- Open the specified relation.  ereport's on failure.
 *
 * Note we only open the first segment, when there are multiple segments.
 */
static MdfdVec *
mdopen(SMgrRelation reln)
{
	char	   *path;
	File		fd;

	/* No work if already open */
	if (reln->md_fd)
		return reln->md_fd;

	path = relpath(reln->smgr_rnode);

	fd = FileNameOpenFile(path, O_RDWR | PG_BINARY, 0600);

	if (fd < 0)
	{
		/*
		 * During bootstrap, there are cases where a system relation will
		 * be accessed (by internal backend processes) before the
		 * bootstrap script nominally creates it.  Therefore, accept
		 * mdopen() as a substitute for mdcreate() in bootstrap mode only.
		 * (See mdcreate)
		 */
		if (IsBootstrapProcessingMode())
			fd = FileNameOpenFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, 0600);
		if (fd < 0)
		{
			pfree(path);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open relation %u/%u: %m",
							reln->smgr_rnode.tblNode,
							reln->smgr_rnode.relNode)));
		}
	}

	pfree(path);

	reln->md_fd = _fdvec_alloc();

	reln->md_fd->mdfd_vfd = fd;
#ifndef LET_OS_MANAGE_FILESIZE
	reln->md_fd->mdfd_chain = NULL;
	Assert(_mdnblocks(fd, BLCKSZ) <= ((BlockNumber) RELSEG_SIZE));
#endif

	return reln->md_fd;
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

	v = _mdfd_getseg(reln, blocknum);

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
		 * If we are at or past EOF, return zeroes without complaining.
		 * Also substitute zeroes if we found a partial block at EOF.
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
mdwrite(SMgrRelation reln, BlockNumber blocknum, char *buffer)
{
	long		seekpos;
	MdfdVec    *v;

	v = _mdfd_getseg(reln, blocknum);

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
	MdfdVec    *v = mdopen(reln);

#ifndef LET_OS_MANAGE_FILESIZE
	BlockNumber nblocks;
	BlockNumber segno = 0;

	/*
	 * Skip through any segments that aren't the last one, to avoid
	 * redundant seeks on them.  We have previously verified that these
	 * segments are exactly RELSEG_SIZE long, and it's useless to recheck
	 * that each time. (NOTE: this assumption could only be wrong if
	 * another backend has truncated the relation.	We rely on higher code
	 * levels to handle that scenario by closing and re-opening the md
	 * fd.)
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
			 * Because we pass O_CREAT, we will create the next segment
			 * (with zero length) immediately, if the last segment is of
			 * length REL_SEGSIZE.	This is unnecessary but harmless, and
			 * testing for the case would take more cycles than it seems
			 * worth.
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
mdtruncate(SMgrRelation reln, BlockNumber nblocks)
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

	v = mdopen(reln);

#ifndef LET_OS_MANAGE_FILESIZE
	priorblocks = 0;
	while (v != NULL)
	{
		MdfdVec    *ov = v;

		if (priorblocks > nblocks)
		{
			/*
			 * This segment is no longer wanted at all (and has already
			 * been unlinked from the mdfd_chain). We truncate the file
			 * before deleting it because if other backends are holding
			 * the file open, the unlink will fail on some platforms.
			 * Better a zero-size file gets left around than a big file...
			 */
			FileTruncate(v->mdfd_vfd, 0);
			FileUnlink(v->mdfd_vfd);
			v = v->mdfd_chain;
			Assert(ov != reln->md_fd);			/* we never drop the 1st
												 * segment */
			pfree(ov);
		}
		else if (priorblocks + ((BlockNumber) RELSEG_SIZE) > nblocks)
		{
			/*
			 * This is the last segment we want to keep. Truncate the file
			 * to the right length, and clear chain link that points to
			 * any remaining segments (which we shall zap). NOTE: if
			 * nblocks is exactly a multiple K of RELSEG_SIZE, we will
			 * truncate the K+1st segment to 0 length but keep it. This is
			 * mainly so that the right thing happens if nblocks==0.
			 */
			BlockNumber lastsegblocks = nblocks - priorblocks;

			if (FileTruncate(v->mdfd_vfd, lastsegblocks * BLCKSZ) < 0)
				return InvalidBlockNumber;
			v = v->mdfd_chain;
			ov->mdfd_chain = NULL;
		}
		else
		{
			/*
			 * We still need this segment and 0 or more blocks beyond it,
			 * so nothing to do here.
			 */
			v = v->mdfd_chain;
		}
		priorblocks += RELSEG_SIZE;
	}
#else
	if (FileTruncate(v->mdfd_vfd, nblocks * BLCKSZ) < 0)
		return InvalidBlockNumber;
#endif

	return nblocks;
}

/*
 *	mdcommit() -- Commit a transaction.
 */
bool
mdcommit(void)
{
	/*
	 * We don't actually have to do anything here...
	 */
	return true;
}

/*
 *	mdabort() -- Abort a transaction.
 */
bool
mdabort(void)
{
	/*
	 * We don't actually have to do anything here...
	 */
	return true;
}

/*
 *	mdsync() -- Sync previous writes to stable storage.
 */
bool
mdsync(void)
{
	sync();
	if (IsUnderPostmaster)
		sleep(2);
	sync();
	return true;
}

/*
 *	_fdvec_alloc() -- Make a MdfdVec object.
 */
static MdfdVec *
_fdvec_alloc(void)
{
	MdfdVec *v;

	v = (MdfdVec *) MemoryContextAlloc(MdCxt, sizeof(MdfdVec));
	v->mdfd_vfd = -1;
#ifndef LET_OS_MANAGE_FILESIZE
	v->mdfd_chain = NULL;
#endif

	return v;
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

	/* be sure we have enough space for the '.segno', if any */
	path = relpath(reln->smgr_rnode);

	if (segno > 0)
	{
		fullpath = (char *) palloc(strlen(path) + 12);
		sprintf(fullpath, "%s.%u", path, segno);
		pfree(path);
	}
	else
		fullpath = path;

	/* open the file */
	fd = FileNameOpenFile(fullpath, O_RDWR | PG_BINARY | oflags, 0600);

	pfree(fullpath);

	if (fd < 0)
		return NULL;

	/* allocate an mdfdvec entry for it */
	v = _fdvec_alloc();

	/* fill the entry */
	v->mdfd_vfd = fd;
	v->mdfd_chain = NULL;
	Assert(_mdnblocks(fd, BLCKSZ) <= ((BlockNumber) RELSEG_SIZE));

	/* all done */
	return v;
}
#endif

/*
 *	_mdfd_getseg() -- Find the segment of the relation holding the
 *					  specified block.  ereport's on failure.
 */
static MdfdVec *
_mdfd_getseg(SMgrRelation reln, BlockNumber blkno)
{
	MdfdVec    *v = mdopen(reln);

#ifndef LET_OS_MANAGE_FILESIZE
	BlockNumber segno;
	BlockNumber i;

	for (segno = blkno / ((BlockNumber) RELSEG_SIZE), i = 1;
		 segno > 0;
		 i++, segno--)
	{

		if (v->mdfd_chain == NULL)
		{
			/*
			 * We will create the next segment only if the target block is
			 * within it.  This prevents Sorcerer's Apprentice syndrome if
			 * a bug at higher levels causes us to be handed a
			 * ridiculously large blkno --- otherwise we could create many
			 * thousands of empty segment files before reaching the
			 * "target" block.	We should never need to create more than
			 * one new segment per call, so this restriction seems
			 * reasonable.
			 */
			v->mdfd_chain = _mdfd_openseg(reln, i, (segno == 1) ? O_CREAT : 0);

			if (v->mdfd_chain == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open segment %u of relation %u/%u (target block %u): %m",
								i,
								reln->smgr_rnode.tblNode,
								reln->smgr_rnode.relNode,
								blkno)));
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
