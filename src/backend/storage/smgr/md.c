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
 *	  $Header: /cvsroot/pgsql/src/backend/storage/smgr/md.c,v 1.98 2003/08/04 02:40:04 momjian Exp $
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
#include "storage/smgr.h"
#include "utils/inval.h"
#include "utils/memutils.h"


#undef DIAGNOSTIC

/*
 *	The magnetic disk storage manager keeps track of open file descriptors
 *	in its own descriptor pool.  This happens for two reasons.	First, at
 *	transaction boundaries, we walk the list of descriptors and flush
 *	anything that we've dirtied in the current transaction.  Second, we want
 *	to support relations larger than the OS' file size limit (often 2GBytes).
 *	In order to do that, we break relations up into chunks of < 2GBytes
 *	and store one chunk in each of several files that represent the relation.
 *	See the BLCKSZ and RELSEG_SIZE configuration constants in include/pg_config.h.
 *
 *	The file descriptor stored in the relation cache (see RelationGetFile())
 *	is actually an index into the Md_fdvec array.  -1 indicates not open.
 *
 *	When a relation is broken into multiple chunks, only the first chunk
 *	has its own entry in the Md_fdvec array; the remaining chunks have
 *	palloc'd MdfdVec objects that are chained onto the first chunk via the
 *	mdfd_chain links.  All chunks except the last MUST have size exactly
 *	equal to RELSEG_SIZE blocks --- see mdnblocks() and mdtruncate().
 */

typedef struct _MdfdVec
{
	int			mdfd_vfd;		/* fd number in vfd pool */
	int			mdfd_flags;		/* fd status flags */

/* these are the assigned bits in mdfd_flags: */
#define MDFD_FREE	(1 << 0)	/* unused entry */

	int			mdfd_nextFree;	/* link to next freelist member, if free */
#ifndef LET_OS_MANAGE_FILESIZE
	struct _MdfdVec *mdfd_chain;	/* for large relations */
#endif
} MdfdVec;

static int	Nfds = 100;			/* initial/current size of Md_fdvec array */
static MdfdVec *Md_fdvec = (MdfdVec *) NULL;
static int	Md_Free = -1;		/* head of freelist of unused fdvec
								 * entries */
static int	CurFd = 0;			/* first never-used fdvec index */
static MemoryContext MdCxt;		/* context for all my allocations */

/* routines declared here */
static void mdclose_fd(int fd);
static int	_mdfd_getrelnfd(Relation reln);
static MdfdVec *_mdfd_openseg(Relation reln, BlockNumber segno, int oflags);
static MdfdVec *_mdfd_getseg(Relation reln, BlockNumber blkno);

static int	_mdfd_blind_getseg(RelFileNode rnode, BlockNumber blkno);

static int	_fdvec_alloc(void);
static void _fdvec_free(int);
static BlockNumber _mdnblocks(File file, Size blcksz);

/*
 *	mdinit() -- Initialize private state for magnetic disk storage manager.
 *
 *		We keep a private table of all file descriptors.  Whenever we do
 *		a write to one, we mark it dirty in our table.	Whenever we force
 *		changes to disk, we mark the file descriptor clean.  At transaction
 *		commit, we force changes to disk for all dirty file descriptors.
 *		This routine allocates and initializes the table.
 *
 *		Returns SM_SUCCESS or SM_FAIL with errno set as appropriate.
 */
int
mdinit(void)
{
	int			i;

	MdCxt = AllocSetContextCreate(TopMemoryContext,
								  "MdSmgr",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);

	Md_fdvec = (MdfdVec *) MemoryContextAlloc(MdCxt, Nfds * sizeof(MdfdVec));

	MemSet(Md_fdvec, 0, Nfds * sizeof(MdfdVec));

	/* Set free list */
	for (i = 0; i < Nfds; i++)
	{
		Md_fdvec[i].mdfd_nextFree = i + 1;
		Md_fdvec[i].mdfd_flags = MDFD_FREE;
	}
	Md_Free = 0;
	Md_fdvec[Nfds - 1].mdfd_nextFree = -1;

	return SM_SUCCESS;
}

int
mdcreate(Relation reln)
{
	char	   *path;
	int			fd,
				vfd;

	Assert(reln->rd_fd < 0);

	path = relpath(reln->rd_node);

	fd = FileNameOpenFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, 0600);

	if (fd < 0)
	{
		int			save_errno = errno;

		/*
		 * During bootstrap, there are cases where a system relation will
		 * be accessed (by internal backend processes) before the
		 * bootstrap script nominally creates it.  Therefore, allow the
		 * file to exist already, but in bootstrap mode only.  (See also
		 * mdopen)
		 */
		if (IsBootstrapProcessingMode())
			fd = FileNameOpenFile(path, O_RDWR | PG_BINARY, 0600);
		if (fd < 0)
		{
			pfree(path);
			/* be sure to return the error reported by create, not open */
			errno = save_errno;
			return -1;
		}
		errno = 0;
	}

	pfree(path);

	vfd = _fdvec_alloc();
	if (vfd < 0)
		return -1;

	Md_fdvec[vfd].mdfd_vfd = fd;
	Md_fdvec[vfd].mdfd_flags = (uint16) 0;
#ifndef LET_OS_MANAGE_FILESIZE
	Md_fdvec[vfd].mdfd_chain = (MdfdVec *) NULL;
#endif

	return vfd;
}

/*
 *	mdunlink() -- Unlink a relation.
 */
int
mdunlink(RelFileNode rnode)
{
	int			status = SM_SUCCESS;
	int			save_errno = 0;
	char	   *path;

	path = relpath(rnode);

	/* Delete the first segment, or only segment if not doing segmenting */
	if (unlink(path) < 0)
	{
		status = SM_FAIL;
		save_errno = errno;
	}

#ifndef LET_OS_MANAGE_FILESIZE
	/* Get the additional segments, if any */
	if (status == SM_SUCCESS)
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
					status = SM_FAIL;
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
 *		This routine returns SM_FAIL or SM_SUCCESS, with errno set as
 *		appropriate.
 *
 * Note: this routine used to call mdnblocks() to get the block position
 * to write at, but that's pretty silly since the caller needs to know where
 * the block will be written, and accordingly must have done mdnblocks()
 * already.  Might as well pass in the position and save a seek.
 */
int
mdextend(Relation reln, BlockNumber blocknum, char *buffer)
{
	long		seekpos;
	int			nbytes;
	MdfdVec    *v;

	v = _mdfd_getseg(reln, blocknum);

#ifndef LET_OS_MANAGE_FILESIZE
	seekpos = (long) (BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE)));
#ifdef DIAGNOSTIC
	if (seekpos >= BLCKSZ * RELSEG_SIZE)
		elog(FATAL, "seekpos too big");
#endif
#else
	seekpos = (long) (BLCKSZ * (blocknum));
#endif

	/*
	 * Note: because caller obtained blocknum by calling mdnblocks, which
	 * did a seek(SEEK_END), this seek is often redundant and will be
	 * optimized away by fd.c.	It's not redundant, however, if there is a
	 * partial page at the end of the file.  In that case we want to try
	 * to overwrite the partial page with a full page.	It's also not
	 * redundant if bufmgr.c had to dump another buffer of the same file
	 * to make room for the new page's buffer.
	 */
	if (FileSeek(v->mdfd_vfd, seekpos, SEEK_SET) != seekpos)
		return SM_FAIL;

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
		return SM_FAIL;
	}

#ifndef LET_OS_MANAGE_FILESIZE
#ifdef DIAGNOSTIC
	if (_mdnblocks(v->mdfd_vfd, BLCKSZ) > ((BlockNumber) RELSEG_SIZE))
		elog(FATAL, "segment too big");
#endif
#endif

	return SM_SUCCESS;
}

/*
 *	mdopen() -- Open the specified relation.
 */
int
mdopen(Relation reln)
{
	char	   *path;
	int			fd;
	int			vfd;

	Assert(reln->rd_fd < 0);

	path = relpath(reln->rd_node);

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
			return -1;
		}
	}

	pfree(path);

	vfd = _fdvec_alloc();
	if (vfd < 0)
		return -1;

	Md_fdvec[vfd].mdfd_vfd = fd;
	Md_fdvec[vfd].mdfd_flags = (uint16) 0;
#ifndef LET_OS_MANAGE_FILESIZE
	Md_fdvec[vfd].mdfd_chain = (MdfdVec *) NULL;

#ifdef DIAGNOSTIC
	if (_mdnblocks(fd, BLCKSZ) > ((BlockNumber) RELSEG_SIZE))
		elog(FATAL, "segment too big");
#endif
#endif

	return vfd;
}

/*
 *	mdclose() -- Close the specified relation, if it isn't closed already.
 *
 *		AND FREE fd vector! It may be re-used for other relation!
 *		reln should be flushed from cache after closing !..
 *
 *		Returns SM_SUCCESS or SM_FAIL with errno set as appropriate.
 */
int
mdclose(Relation reln)
{
	int			fd;

	fd = RelationGetFile(reln);
	if (fd < 0)
		return SM_SUCCESS;		/* already closed, so no work */

	mdclose_fd(fd);

	reln->rd_fd = -1;

	return SM_SUCCESS;
}

static void
mdclose_fd(int fd)
{
	MdfdVec    *v;

#ifndef LET_OS_MANAGE_FILESIZE
	for (v = &Md_fdvec[fd]; v != (MdfdVec *) NULL;)
	{
		MdfdVec    *ov = v;

		/* if not closed already */
		if (v->mdfd_vfd >= 0)
			FileClose(v->mdfd_vfd);
		/* Now free vector */
		v = v->mdfd_chain;
		if (ov != &Md_fdvec[fd])
			pfree(ov);
	}

	Md_fdvec[fd].mdfd_chain = (MdfdVec *) NULL;
#else
	v = &Md_fdvec[fd];
	if (v != (MdfdVec *) NULL)
	{
		if (v->mdfd_vfd >= 0)
			FileClose(v->mdfd_vfd);
	}
#endif

	_fdvec_free(fd);
}

/*
 *	mdread() -- Read the specified block from a relation.
 *
 *		Returns SM_SUCCESS or SM_FAIL.
 */
int
mdread(Relation reln, BlockNumber blocknum, char *buffer)
{
	int			status;
	long		seekpos;
	int			nbytes;
	MdfdVec    *v;

	v = _mdfd_getseg(reln, blocknum);

#ifndef LET_OS_MANAGE_FILESIZE
	seekpos = (long) (BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE)));

#ifdef DIAGNOSTIC
	if (seekpos >= BLCKSZ * RELSEG_SIZE)
		elog(FATAL, "seekpos too big");
#endif
#else
	seekpos = (long) (BLCKSZ * (blocknum));
#endif

	if (FileSeek(v->mdfd_vfd, seekpos, SEEK_SET) != seekpos)
		return SM_FAIL;

	status = SM_SUCCESS;
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
			status = SM_FAIL;
	}

	return status;
}

/*
 *	mdwrite() -- Write the supplied block at the appropriate location.
 *
 *		Returns SM_SUCCESS or SM_FAIL.
 */
int
mdwrite(Relation reln, BlockNumber blocknum, char *buffer)
{
	long		seekpos;
	MdfdVec    *v;

	v = _mdfd_getseg(reln, blocknum);

#ifndef LET_OS_MANAGE_FILESIZE
	seekpos = (long) (BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE)));
#ifdef DIAGNOSTIC
	if (seekpos >= BLCKSZ * RELSEG_SIZE)
		elog(FATAL, "seekpos too big");
#endif
#else
	seekpos = (long) (BLCKSZ * (blocknum));
#endif

	if (FileSeek(v->mdfd_vfd, seekpos, SEEK_SET) != seekpos)
		return SM_FAIL;

	if (FileWrite(v->mdfd_vfd, buffer, BLCKSZ) != BLCKSZ)
		return SM_FAIL;

	return SM_SUCCESS;
}

/*
 *	mdblindwrt() -- Write a block to disk blind.
 *
 *		We have to be able to do this using only the rnode of the relation
 *		in which the block belongs.  Otherwise this is much like mdwrite().
 */
int
mdblindwrt(RelFileNode rnode,
		   BlockNumber blkno,
		   char *buffer)
{
	int			status;
	long		seekpos;
	int			fd;

	fd = _mdfd_blind_getseg(rnode, blkno);

	if (fd < 0)
		return SM_FAIL;

#ifndef LET_OS_MANAGE_FILESIZE
	seekpos = (long) (BLCKSZ * (blkno % ((BlockNumber) RELSEG_SIZE)));
#ifdef DIAGNOSTIC
	if (seekpos >= BLCKSZ * RELSEG_SIZE)
		elog(FATAL, "seekpos too big");
#endif
#else
	seekpos = (long) (BLCKSZ * (blkno));
#endif

	errno = 0;
	if (lseek(fd, seekpos, SEEK_SET) != seekpos)
	{
		elog(LOG, "lseek(%ld) failed: %m", seekpos);
		close(fd);
		return SM_FAIL;
	}

	status = SM_SUCCESS;

	/* write the block */
	errno = 0;
	if (write(fd, buffer, BLCKSZ) != BLCKSZ)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		elog(LOG, "write() failed: %m");
		status = SM_FAIL;
	}

	if (close(fd) < 0)
	{
		elog(LOG, "close() failed: %m");
		status = SM_FAIL;
	}

	return status;
}

/*
 *	mdnblocks() -- Get the number of blocks stored in a relation.
 *
 *		Important side effect: all segments of the relation are opened
 *		and added to the mdfd_chain list.  If this routine has not been
 *		called, then only segments up to the last one actually touched
 *		are present in the chain...
 *
 *		Returns # of blocks, ereport's on error.
 */
BlockNumber
mdnblocks(Relation reln)
{
	int			fd;
	MdfdVec    *v;

#ifndef LET_OS_MANAGE_FILESIZE
	BlockNumber nblocks;
	BlockNumber segno;
#endif

	fd = _mdfd_getrelnfd(reln);
	v = &Md_fdvec[fd];

#ifndef LET_OS_MANAGE_FILESIZE
	segno = 0;

	/*
	 * Skip through any segments that aren't the last one, to avoid
	 * redundant seeks on them.  We have previously verified that these
	 * segments are exactly RELSEG_SIZE long, and it's useless to recheck
	 * that each time. (NOTE: this assumption could only be wrong if
	 * another backend has truncated the relation.	We rely on higher code
	 * levels to handle that scenario by closing and re-opening the md
	 * fd.)
	 */
	while (v->mdfd_chain != (MdfdVec *) NULL)
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

		if (v->mdfd_chain == (MdfdVec *) NULL)
		{
			/*
			 * Because we pass O_CREAT, we will create the next segment
			 * (with zero length) immediately, if the last segment is of
			 * length REL_SEGSIZE.	This is unnecessary but harmless, and
			 * testing for the case would take more cycles than it seems
			 * worth.
			 */
			v->mdfd_chain = _mdfd_openseg(reln, segno, O_CREAT);
			if (v->mdfd_chain == (MdfdVec *) NULL)
				elog(ERROR, "could not count blocks of \"%s\": %m",
					 RelationGetRelationName(reln));
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
mdtruncate(Relation reln, BlockNumber nblocks)
{
	int			fd;
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
	if (nblocks > curnblk)
		return InvalidBlockNumber;		/* bogus request */
	if (nblocks == curnblk)
		return nblocks;			/* no work */

	fd = _mdfd_getrelnfd(reln);
	v = &Md_fdvec[fd];

#ifndef LET_OS_MANAGE_FILESIZE
	priorblocks = 0;
	while (v != (MdfdVec *) NULL)
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
			Assert(ov != &Md_fdvec[fd]);		/* we never drop the 1st
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
			ov->mdfd_chain = (MdfdVec *) NULL;
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
 *
 *		Returns SM_SUCCESS or SM_FAIL with errno set as appropriate.
 */
int
mdcommit(void)
{
	/*
	 * We don't actually have to do anything here...
	 */
	return SM_SUCCESS;
}

/*
 *	mdabort() -- Abort a transaction.
 *
 *		Changes need not be forced to disk at transaction abort.
 */
int
mdabort(void)
{
	/*
	 * We don't actually have to do anything here...
	 */
	return SM_SUCCESS;
}

/*
 *	mdsync() -- Sync previous writes to stable storage.
 */
int
mdsync(void)
{
	sync();
	if (IsUnderPostmaster)
		sleep(2);
	sync();
	return SM_SUCCESS;
}

/*
 *	_fdvec_alloc () -- grab a free (or new) md file descriptor vector.
 */
static int
_fdvec_alloc(void)
{
	MdfdVec    *nvec;
	int			fdvec,
				i;

	if (Md_Free >= 0)			/* get from free list */
	{
		fdvec = Md_Free;
		Md_Free = Md_fdvec[fdvec].mdfd_nextFree;
		Assert(Md_fdvec[fdvec].mdfd_flags == MDFD_FREE);
		Md_fdvec[fdvec].mdfd_flags = 0;
		if (fdvec >= CurFd)
		{
			Assert(fdvec == CurFd);
			CurFd++;
		}
		return fdvec;
	}

	/* Must allocate more room */

	if (Nfds != CurFd)
		elog(FATAL, "_fdvec_alloc error");

	Nfds *= 2;

	nvec = (MdfdVec *) MemoryContextAlloc(MdCxt, Nfds * sizeof(MdfdVec));
	MemSet(nvec, 0, Nfds * sizeof(MdfdVec));
	memcpy(nvec, (char *) Md_fdvec, CurFd * sizeof(MdfdVec));
	pfree(Md_fdvec);

	Md_fdvec = nvec;

	/* Set new free list */
	for (i = CurFd; i < Nfds; i++)
	{
		Md_fdvec[i].mdfd_nextFree = i + 1;
		Md_fdvec[i].mdfd_flags = MDFD_FREE;
	}
	Md_fdvec[Nfds - 1].mdfd_nextFree = -1;
	Md_Free = CurFd + 1;

	fdvec = CurFd;
	CurFd++;
	Md_fdvec[fdvec].mdfd_flags = 0;

	return fdvec;
}

/*
 *	_fdvec_free () -- free md file descriptor vector.
 *
 */
static
void
_fdvec_free(int fdvec)
{

	Assert(Md_Free < 0 || Md_fdvec[Md_Free].mdfd_flags == MDFD_FREE);
	Assert(Md_fdvec[fdvec].mdfd_flags != MDFD_FREE);
	Md_fdvec[fdvec].mdfd_nextFree = Md_Free;
	Md_fdvec[fdvec].mdfd_flags = MDFD_FREE;
	Md_Free = fdvec;
}

static MdfdVec *
_mdfd_openseg(Relation reln, BlockNumber segno, int oflags)
{
	MdfdVec    *v;
	int			fd;
	char	   *path,
			   *fullpath;

	/* be sure we have enough space for the '.segno', if any */
	path = relpath(reln->rd_node);

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
		return (MdfdVec *) NULL;

	/* allocate an mdfdvec entry for it */
	v = (MdfdVec *) MemoryContextAlloc(MdCxt, sizeof(MdfdVec));

	/* fill the entry */
	v->mdfd_vfd = fd;
	v->mdfd_flags = (uint16) 0;
#ifndef LET_OS_MANAGE_FILESIZE
	v->mdfd_chain = (MdfdVec *) NULL;

#ifdef DIAGNOSTIC
	if (_mdnblocks(fd, BLCKSZ) > ((BlockNumber) RELSEG_SIZE))
		elog(FATAL, "segment too big");
#endif
#endif

	/* all done */
	return v;
}

/* Get the fd for the relation, opening it if it's not already open */

static int
_mdfd_getrelnfd(Relation reln)
{
	int			fd;

	fd = RelationGetFile(reln);
	if (fd < 0)
	{
		if ((fd = mdopen(reln)) < 0)
			elog(ERROR, "could not open relation \"%s\": %m",
				 RelationGetRelationName(reln));
		reln->rd_fd = fd;
	}
	return fd;
}

/* Find the segment of the relation holding the specified block */

static MdfdVec *
_mdfd_getseg(Relation reln, BlockNumber blkno)
{
	MdfdVec    *v;
	int			fd;

#ifndef LET_OS_MANAGE_FILESIZE
	BlockNumber segno;
	BlockNumber i;
#endif

	fd = _mdfd_getrelnfd(reln);

#ifndef LET_OS_MANAGE_FILESIZE
	for (v = &Md_fdvec[fd], segno = blkno / ((BlockNumber) RELSEG_SIZE), i = 1;
		 segno > 0;
		 i++, segno--)
	{

		if (v->mdfd_chain == (MdfdVec *) NULL)
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

			if (v->mdfd_chain == (MdfdVec *) NULL)
				elog(ERROR, "could not open segment %u of relation \"%s\" (target block %u): %m",
					 i, RelationGetRelationName(reln), blkno);
		}
		v = v->mdfd_chain;
	}
#else
	v = &Md_fdvec[fd];
#endif

	return v;
}

/*
 * Find the segment of the relation holding the specified block.
 *
 * This performs the same work as _mdfd_getseg() except that we must work
 * "blind" with no Relation struct.  We assume that we are not likely to
 * touch the same relation again soon, so we do not create an FD entry for
 * the relation --- we just open a kernel file descriptor which will be
 * used and promptly closed.  We also assume that the target block already
 * exists, ie, we need not extend the relation.
 *
 * The return value is the kernel descriptor, or -1 on failure.
 */

static int
_mdfd_blind_getseg(RelFileNode rnode, BlockNumber blkno)
{
	char	   *path;
	int			fd;

#ifndef LET_OS_MANAGE_FILESIZE
	BlockNumber segno;
#endif

	path = relpath(rnode);

#ifndef LET_OS_MANAGE_FILESIZE
	/* append the '.segno', if needed */
	segno = blkno / ((BlockNumber) RELSEG_SIZE);
	if (segno > 0)
	{
		char	   *segpath = (char *) palloc(strlen(path) + 12);

		sprintf(segpath, "%s.%u", path, segno);
		pfree(path);
		path = segpath;
	}
#endif

	/* call fd.c to allow other FDs to be closed if needed */
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY, 0600);
	if (fd < 0)
		elog(LOG, "could not open \"%s\": %m", path);

	pfree(path);

	return fd;
}

static BlockNumber
_mdnblocks(File file, Size blcksz)
{
	long		len;

	len = FileSeek(file, 0L, SEEK_END);
	if (len < 0)
		return 0;				/* on failure, assume file is empty */
	return (BlockNumber) (len / blcksz);
}
