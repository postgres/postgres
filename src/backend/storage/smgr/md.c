/*-------------------------------------------------------------------------
 *
 * md.c
 *	  This code manages relations that reside on magnetic disk.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/smgr/md.c,v 1.77 2000/10/28 16:20:57 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "postgres.h"

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
 *	See the BLCKSZ and RELSEG_SIZE configuration constants in include/config.h.
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
#define MDFD_FREE		(1 << 0)/* unused entry */

	int			mdfd_lstbcnt;	/* most recent block count */
	int			mdfd_nextFree;	/* next free vector */
#ifndef LET_OS_MANAGE_FILESIZE
	struct _MdfdVec *mdfd_chain;/* for large relations */
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
static MdfdVec *_mdfd_openseg(Relation reln, int segno, int oflags);
static MdfdVec *_mdfd_getseg(Relation reln, int blkno);

static int _mdfd_blind_getseg(RelFileNode rnode, int blkno);

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
mdinit()
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
	int			fd,
				vfd;
	char	   *path;

	Assert(reln->rd_unlinked && reln->rd_fd < 0);

	path = relpath(reln->rd_node);
	fd = FileNameOpenFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, 0600);

	/*
	 * For cataloged relations, pg_class is guaranteed to have a unique
	 * record with the same relname by the unique index. So we are able to
	 * reuse existent files for new cataloged relations. Currently we reuse
	 * them in the following cases. 1. they are empty. 2. they are used
	 * for Index relations and their size == BLCKSZ * 2.
	 *
	 * During bootstrap processing, we skip that check, because pg_time,
	 * pg_variable, and pg_log get created before their .bki file entries
	 * are processed.
	 */

	if (fd < 0)
	{
		int		save_errno = errno;

		if (!IsBootstrapProcessingMode() &&
			reln->rd_rel->relkind == RELKIND_UNCATALOGED)
			return -1;

		fd = FileNameOpenFile(path, O_RDWR | PG_BINARY, 0600);
		if (fd < 0)
		{
			/* be sure to return the error reported by create, not open */
			errno = save_errno;
			return -1;
		}
		if (!IsBootstrapProcessingMode())
		{
			bool		reuse = false;
			long		len = FileSeek(fd, 0L, SEEK_END);

			if (len == 0)
				reuse = true;
			else if (reln->rd_rel->relkind == RELKIND_INDEX &&
					 len == BLCKSZ * 2)
				reuse = true;
			if (!reuse)
			{
				FileClose(fd);
				/* be sure to return the error reported by create */
				errno = save_errno;
				return -1;
			}
		}
		errno = 0;
	}
	reln->rd_unlinked = false;

	vfd = _fdvec_alloc();
	if (vfd < 0)
		return -1;

	Md_fdvec[vfd].mdfd_vfd = fd;
	Md_fdvec[vfd].mdfd_flags = (uint16) 0;
#ifndef LET_OS_MANAGE_FILESIZE
	Md_fdvec[vfd].mdfd_chain = (MdfdVec *) NULL;
#endif
	Md_fdvec[vfd].mdfd_lstbcnt = 0;

	pfree(path);

	return vfd;
}

/*
 *	mdunlink() -- Unlink a relation.
 */
int
mdunlink(Relation reln)
{
	int			nblocks;
	int			fd;
	MdfdVec    *v;

	/*
	 * If the relation is already unlinked,we have nothing to do any more.
	 */
	if (reln->rd_unlinked && reln->rd_fd < 0)
		return SM_SUCCESS;

	/*
	 * Force all segments of the relation to be opened, so that we won't
	 * miss deleting any of them.
	 */
	nblocks = mdnblocks(reln);

	/*
	 * Clean out the mdfd vector, letting fd.c unlink the physical files.
	 *
	 * NOTE: We truncate the file(s) before deleting 'em, because if other
	 * backends are holding the files open, the unlink will fail on some
	 * platforms (think Microsoft).  Better a zero-size file gets left
	 * around than a big file.	Those other backends will be forced to
	 * close the relation by cache invalidation, but that probably hasn't
	 * happened yet.
	 */
	fd = RelationGetFile(reln);
	if (fd < 0)					/* should not happen */
		elog(ERROR, "mdunlink: mdnblocks didn't open relation");

	Md_fdvec[fd].mdfd_flags = (uint16) 0;

#ifndef LET_OS_MANAGE_FILESIZE
	for (v = &Md_fdvec[fd]; v != (MdfdVec *) NULL;)
	{
		MdfdVec    *ov = v;

		FileTruncate(v->mdfd_vfd, 0);
		FileUnlink(v->mdfd_vfd);
		v = v->mdfd_chain;
		if (ov != &Md_fdvec[fd])
			pfree(ov);
	}
	Md_fdvec[fd].mdfd_chain = (MdfdVec *) NULL;
#else
	v = &Md_fdvec[fd];
	FileTruncate(v->mdfd_vfd, 0);
	FileUnlink(v->mdfd_vfd);
#endif

	_fdvec_free(fd);

	/* be sure to mark relation closed && unlinked */
	reln->rd_fd = -1;
	reln->rd_unlinked = true;

	return SM_SUCCESS;
}

/*
 *	mdextend() -- Add a block to the specified relation.
 *
 *		This routine returns SM_FAIL or SM_SUCCESS, with errno set as
 *		appropriate.
 */
int
mdextend(Relation reln, char *buffer)
{
	long		pos,
				nbytes;
	int			nblocks;
	MdfdVec    *v;

	nblocks = mdnblocks(reln);
	v = _mdfd_getseg(reln, nblocks);

	if ((pos = FileSeek(v->mdfd_vfd, 0L, SEEK_END)) < 0)
		return SM_FAIL;

	if (pos % BLCKSZ != 0)		/* the last block is incomplete */
	{
		pos -= pos % BLCKSZ;
		if (FileSeek(v->mdfd_vfd, pos, SEEK_SET) < 0)
			return SM_FAIL;
	}

	if ((nbytes = FileWrite(v->mdfd_vfd, buffer, BLCKSZ)) != BLCKSZ)
	{
		if (nbytes > 0)
		{
			FileTruncate(v->mdfd_vfd, pos);
			FileSeek(v->mdfd_vfd, pos, SEEK_SET);
		}
		return SM_FAIL;
	}

	/* try to keep the last block count current, though it's just a hint */
#ifndef LET_OS_MANAGE_FILESIZE
	if ((v->mdfd_lstbcnt = (++nblocks % RELSEG_SIZE)) == 0)
		v->mdfd_lstbcnt = RELSEG_SIZE;

#ifdef DIAGNOSTIC
	if (_mdnblocks(v->mdfd_vfd, BLCKSZ) > RELSEG_SIZE
		|| v->mdfd_lstbcnt > RELSEG_SIZE)
		elog(FATAL, "segment too big!");
#endif
#else
	v->mdfd_lstbcnt = ++nblocks;
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
		/* in bootstrap mode, accept mdopen as substitute for mdcreate */
		if (IsBootstrapProcessingMode())
			fd = FileNameOpenFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, 0600);
		if (fd < 0)
		{
			elog(NOTICE, "mdopen: couldn't open %s: %m", path);
			/* mark relation closed and unlinked */
			reln->rd_fd = -1;
			reln->rd_unlinked = true;
			return -1;
		}
	}
	reln->rd_unlinked = false;

	vfd = _fdvec_alloc();
	if (vfd < 0)
		return -1;

	Md_fdvec[vfd].mdfd_vfd = fd;
	Md_fdvec[vfd].mdfd_flags = (uint16) 0;
	Md_fdvec[vfd].mdfd_lstbcnt = _mdnblocks(fd, BLCKSZ);
#ifndef LET_OS_MANAGE_FILESIZE
	Md_fdvec[vfd].mdfd_chain = (MdfdVec *) NULL;

#ifdef DIAGNOSTIC
	if (Md_fdvec[vfd].mdfd_lstbcnt > RELSEG_SIZE)
		elog(FATAL, "segment too big on relopen!");
#endif
#endif

	pfree(path);

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
		{

			/*
			 * We sync the file descriptor so that we don't need to reopen
			 * it at transaction commit to force changes to disk.  (This
			 * is not really optional, because we are about to forget that
			 * the file even exists...)
			 */
			FileSync(v->mdfd_vfd);
			FileClose(v->mdfd_vfd);
		}
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
		{

			/*
			 * We sync the file descriptor so that we don't need to reopen
			 * it at transaction commit to force changes to disk.  (This
			 * is not really optional, because we are about to forget that
			 * the file even exists...)
			 */
			FileSync(v->mdfd_vfd);
			FileClose(v->mdfd_vfd);
		}
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
	seekpos = (long) (BLCKSZ * (blocknum % RELSEG_SIZE));

#ifdef DIAGNOSTIC
	if (seekpos >= BLCKSZ * RELSEG_SIZE)
		elog(FATAL, "seekpos too big!");
#endif
#else
	seekpos = (long) (BLCKSZ * (blocknum));
#endif

	if (FileSeek(v->mdfd_vfd, seekpos, SEEK_SET) != seekpos)
		return SM_FAIL;

	status = SM_SUCCESS;
	if ((nbytes = FileRead(v->mdfd_vfd, buffer, BLCKSZ)) != BLCKSZ)
	{
		if (nbytes == 0)
			MemSet(buffer, 0, BLCKSZ);
		else if (blocknum == 0 && nbytes > 0 && mdnblocks(reln) == 0)
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
	int			status;
	long		seekpos;
	MdfdVec    *v;

	v = _mdfd_getseg(reln, blocknum);

#ifndef LET_OS_MANAGE_FILESIZE
	seekpos = (long) (BLCKSZ * (blocknum % RELSEG_SIZE));
#ifdef DIAGNOSTIC
	if (seekpos >= BLCKSZ * RELSEG_SIZE)
		elog(FATAL, "seekpos too big!");
#endif
#else
	seekpos = (long) (BLCKSZ * (blocknum));
#endif

	if (FileSeek(v->mdfd_vfd, seekpos, SEEK_SET) != seekpos)
		return SM_FAIL;

	status = SM_SUCCESS;
	if (FileWrite(v->mdfd_vfd, buffer, BLCKSZ) != BLCKSZ)
		status = SM_FAIL;

	return status;
}

/*
 *	mdflush() -- Synchronously write a block to disk.
 *
 *		This is exactly like mdwrite(), but doesn't return until the file
 *		system buffer cache has been flushed.
 */
int
mdflush(Relation reln, BlockNumber blocknum, char *buffer)
{
	int			status;
	long		seekpos;
	MdfdVec    *v;

	v = _mdfd_getseg(reln, blocknum);

#ifndef LET_OS_MANAGE_FILESIZE
	seekpos = (long) (BLCKSZ * (blocknum % RELSEG_SIZE));
#ifdef DIAGNOSTIC
	if (seekpos >= BLCKSZ * RELSEG_SIZE)
		elog(FATAL, "seekpos too big!");
#endif
#else
	seekpos = (long) (BLCKSZ * (blocknum));
#endif

	if (FileSeek(v->mdfd_vfd, seekpos, SEEK_SET) != seekpos)
		return SM_FAIL;

	/* write and sync the block */
	status = SM_SUCCESS;
	if (FileWrite(v->mdfd_vfd, buffer, BLCKSZ) != BLCKSZ
		|| FileSync(v->mdfd_vfd) < 0)
		status = SM_FAIL;

	return status;
}

/*
 *	mdblindwrt() -- Write a block to disk blind.
 *
 *		We have to be able to do this using only the name and OID of
 *		the database and relation in which the block belongs.  Otherwise
 *		this is much like mdwrite().  If dofsync is TRUE, then we fsync
 *		the file, making it more like mdflush().
 */
int
mdblindwrt(RelFileNode rnode,
		   BlockNumber blkno,
		   char *buffer,
		   bool dofsync)
{
	int			status;
	long		seekpos;
	int			fd;

	fd = _mdfd_blind_getseg(rnode, blkno);

	if (fd < 0)
		return SM_FAIL;

#ifndef LET_OS_MANAGE_FILESIZE
	seekpos = (long) (BLCKSZ * (blkno % RELSEG_SIZE));
#ifdef DIAGNOSTIC
	if (seekpos >= BLCKSZ * RELSEG_SIZE)
		elog(FATAL, "seekpos too big!");
#endif
#else
	seekpos = (long) (BLCKSZ * (blkno));
#endif

	errno = 0;

	if (lseek(fd, seekpos, SEEK_SET) != seekpos)
	{
		elog(DEBUG, "mdblindwrt: lseek(%ld) failed: %m", seekpos);
		close(fd);
		return SM_FAIL;
	}

	status = SM_SUCCESS;

	/* write and optionally sync the block */
	if (write(fd, buffer, BLCKSZ) != BLCKSZ)
	{
		elog(DEBUG, "mdblindwrt: write() failed: %m");
		status = SM_FAIL;
	}
	else if (dofsync &&
			 pg_fsync(fd) < 0)
	{
		elog(DEBUG, "mdblindwrt: fsync() failed: %m");
		status = SM_FAIL;
	}

	if (close(fd) < 0)
	{
		elog(DEBUG, "mdblindwrt: close() failed: %m");
		status = SM_FAIL;
	}

	return status;
}

/*
 *	mdmarkdirty() -- Mark the specified block "dirty" (ie, needs fsync).
 *
 *		Returns SM_SUCCESS or SM_FAIL.
 */
int
mdmarkdirty(Relation reln, BlockNumber blkno)
{
	MdfdVec    *v;

	v = _mdfd_getseg(reln, blkno);

	FileMarkDirty(v->mdfd_vfd);

	return SM_SUCCESS;
}

/*
 *	mdblindmarkdirty() -- Mark the specified block "dirty" (ie, needs fsync).
 *
 *		We have to be able to do this using only the name and OID of
 *		the database and relation in which the block belongs.  Otherwise
 *		this is much like mdmarkdirty().  However, we do the fsync immediately
 *		rather than building md/fd datastructures to postpone it till later.
 */
int
mdblindmarkdirty(RelFileNode rnode,
				 BlockNumber blkno)
{
	int			status;
	int			fd;

	fd = _mdfd_blind_getseg(rnode, blkno);

	if (fd < 0)
		return SM_FAIL;

	status = SM_SUCCESS;

	if (pg_fsync(fd) < 0)
		status = SM_FAIL;

	if (close(fd) < 0)
		status = SM_FAIL;

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
 *		Returns # of blocks, elog's on error.
 */
int
mdnblocks(Relation reln)
{
	int			fd;
	MdfdVec    *v;

#ifndef LET_OS_MANAGE_FILESIZE
	int			nblocks;
	int			segno;

#endif

	fd = _mdfd_getrelnfd(reln);
	v = &Md_fdvec[fd];

#ifndef LET_OS_MANAGE_FILESIZE
	segno = 0;
	for (;;)
	{
		nblocks = _mdnblocks(v->mdfd_vfd, BLCKSZ);
		if (nblocks > RELSEG_SIZE)
			elog(FATAL, "segment too big in mdnblocks!");
		v->mdfd_lstbcnt = nblocks;
		if (nblocks == RELSEG_SIZE)
		{
			segno++;

			if (v->mdfd_chain == (MdfdVec *) NULL)
			{
				/*
				 * Because we pass O_CREAT, we will create the next segment
				 * (with zero length) immediately, if the last segment is of
				 * length REL_SEGSIZE.  This is unnecessary but harmless, and
				 * testing for the case would take more cycles than it seems
				 * worth.
				 */
				v->mdfd_chain = _mdfd_openseg(reln, segno, O_CREAT);
				if (v->mdfd_chain == (MdfdVec *) NULL)
					elog(ERROR, "cannot count blocks for %s -- open failed: %m",
						 RelationGetRelationName(reln));
			}

			v = v->mdfd_chain;
		}
		else
			return (segno * RELSEG_SIZE) + nblocks;
	}
#else
	return _mdnblocks(v->mdfd_vfd, BLCKSZ);
#endif
}

/*
 *	mdtruncate() -- Truncate relation to specified number of blocks.
 *
 *		Returns # of blocks or -1 on error.
 */
int
mdtruncate(Relation reln, int nblocks)
{
	int			curnblk;
	int			fd;
	MdfdVec    *v;
#ifndef LET_OS_MANAGE_FILESIZE
	int			priorblocks;
#endif

	/*
	 * NOTE: mdnblocks makes sure we have opened all existing segments, so
	 * that truncate/delete loop will get them all!
	 */
	curnblk = mdnblocks(reln);
	if (nblocks < 0 || nblocks > curnblk)
		return -1;				/* bogus request */
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
		else if (priorblocks + RELSEG_SIZE > nblocks)
		{

			/*
			 * This is the last segment we want to keep. Truncate the file
			 * to the right length, and clear chain link that points to
			 * any remaining segments (which we shall zap). NOTE: if
			 * nblocks is exactly a multiple K of RELSEG_SIZE, we will
			 * truncate the K+1st segment to 0 length but keep it. This is
			 * mainly so that the right thing happens if nblocks=0.
			 */
			int			lastsegblocks = nblocks - priorblocks;

			if (FileTruncate(v->mdfd_vfd, lastsegblocks * BLCKSZ) < 0)
				return -1;
			v->mdfd_lstbcnt = lastsegblocks;
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
		return -1;
	v->mdfd_lstbcnt = nblocks;
#endif

	return nblocks;
}

/*
 *	mdcommit() -- Commit a transaction.
 *
 *		All changes to magnetic disk relations must be forced to stable
 *		storage.  This routine makes a pass over the private table of
 *		file descriptors.  Any descriptors to which we have done writes,
 *		but not synced, are synced here.
 *
 *		Returns SM_SUCCESS or SM_FAIL with errno set as appropriate.
 */
int
mdcommit()
{
	int			i;
	MdfdVec    *v;

	for (i = 0; i < CurFd; i++)
	{
		v = &Md_fdvec[i];
		if (v->mdfd_flags & MDFD_FREE)
			continue;
		/* Sync the file entry */
#ifndef LET_OS_MANAGE_FILESIZE
		for (; v != (MdfdVec *) NULL; v = v->mdfd_chain)
#else
		if (v != (MdfdVec *) NULL)
#endif
		{
			if (FileSync(v->mdfd_vfd) < 0)
				return SM_FAIL;
		}
	}

	return SM_SUCCESS;
}

/*
 *	mdabort() -- Abort a transaction.
 *
 *		Changes need not be forced to disk at transaction abort.  We mark
 *		all file descriptors as clean here.  Always returns SM_SUCCESS.
 */
int
mdabort()
{

	/*
	 * We don't actually have to do anything here.  fd.c will discard
	 * fsync-needed bits in its AtEOXact_Files() routine.
	 */
	return SM_SUCCESS;
}

#ifdef XLOG
/*
 *	mdsync() -- Sync storage.
 *
 */
int
mdsync()
{
	sync();
	if (IsUnderPostmaster)
		sleep(2);
	sync();
	return SM_SUCCESS;
}
#endif

/*
 *	_fdvec_alloc () -- grab a free (or new) md file descriptor vector.
 *
 */
static
int
_fdvec_alloc()
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
_mdfd_openseg(Relation reln, int segno, int oflags)
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
		sprintf(fullpath, "%s.%d", path, segno);
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
	v->mdfd_lstbcnt = _mdnblocks(fd, BLCKSZ);
#ifndef LET_OS_MANAGE_FILESIZE
	v->mdfd_chain = (MdfdVec *) NULL;

#ifdef DIAGNOSTIC
	if (v->mdfd_lstbcnt > RELSEG_SIZE)
		elog(FATAL, "segment too big on open!");
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
			elog(ERROR, "cannot open relation %s",
				 RelationGetRelationName(reln));
		reln->rd_fd = fd;
	}
	return fd;
}

/* Find the segment of the relation holding the specified block */

static MdfdVec *
_mdfd_getseg(Relation reln, int blkno)
{
	MdfdVec    *v;
	int			segno;
	int			fd;
	int			i;

	fd = _mdfd_getrelnfd(reln);

#ifndef LET_OS_MANAGE_FILESIZE
	for (v = &Md_fdvec[fd], segno = blkno / RELSEG_SIZE, i = 1;
		 segno > 0;
		 i++, segno--)
	{

		if (v->mdfd_chain == (MdfdVec *) NULL)
		{
			/*
			 * We will create the next segment only if the target block
			 * is within it.  This prevents Sorcerer's Apprentice syndrome
			 * if a bug at higher levels causes us to be handed a ridiculously
			 * large blkno --- otherwise we could create many thousands of
			 * empty segment files before reaching the "target" block.  We
			 * should never need to create more than one new segment per call,
			 * so this restriction seems reasonable.
			 */
			v->mdfd_chain = _mdfd_openseg(reln, i, (segno == 1) ? O_CREAT : 0);

			if (v->mdfd_chain == (MdfdVec *) NULL)
				elog(ERROR, "cannot open segment %d of relation %s (target block %d): %m",
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
_mdfd_blind_getseg(RelFileNode rnode, int blkno)
{
	char	   *path;
	int			fd;

#ifndef LET_OS_MANAGE_FILESIZE
	int			segno;

#endif

	path = relpath(rnode);

#ifndef LET_OS_MANAGE_FILESIZE
	/* append the '.segno', if needed */
	segno = blkno / RELSEG_SIZE;
	if (segno > 0)
	{
		char	   *segpath = (char *) palloc(strlen(path) + 12);

		sprintf(segpath, "%s.%d", path, segno);
		pfree(path);
		path = segpath;
	}
#endif

	/* call fd.c to allow other FDs to be closed if needed */
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY, 0600);
	if (fd < 0)
		elog(DEBUG, "_mdfd_blind_getseg: couldn't open %s: %m", path);

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
