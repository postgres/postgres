/*-------------------------------------------------------------------------
 *
 * md.c
 *	  This code manages relations that reside on magnetic disk.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/smgr/md.c,v 1.58 1999/11/04 08:01:00 inoue Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "postgres.h"

#include "catalog/catalog.h"
#include "miscadmin.h"
#include "storage/smgr.h"

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
	uint16		mdfd_flags;		/* clean, dirty, free */
	int			mdfd_lstbcnt;	/* most recent block count */
	int			mdfd_nextFree;	/* next free vector */
#ifndef LET_OS_MANAGE_FILESIZE
	struct _MdfdVec *mdfd_chain;/* for large relations */
#endif
} MdfdVec;

static int	Nfds = 100;			/* initial/current size of Md_fdvec array */
static MdfdVec *Md_fdvec = (MdfdVec *) NULL;
static int	Md_Free = -1;		/* head of freelist of unused fdvec entries */
static int	CurFd = 0;			/* first never-used fdvec index */
static MemoryContext MdCxt;		/* context for all my allocations */

#define MDFD_DIRTY		(uint16) 0x01
#define MDFD_FREE		(uint16) 0x02

/* routines declared here */
static int _mdfd_getrelnfd(Relation reln);
static MdfdVec *_mdfd_openseg(Relation reln, int segno, int oflags);
static MdfdVec *_mdfd_getseg(Relation reln, int blkno);
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
	MemoryContext oldcxt;
	int			i;

	MdCxt = (MemoryContext) CreateGlobalMemory("MdSmgr");
	if (MdCxt == (MemoryContext) NULL)
		return SM_FAIL;

	oldcxt = MemoryContextSwitchTo(MdCxt);
	Md_fdvec = (MdfdVec *) palloc(Nfds * sizeof(MdfdVec));
	MemoryContextSwitchTo(oldcxt);

	if (Md_fdvec == (MdfdVec *) NULL)
		return SM_FAIL;

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
	path = relpath(reln->rd_rel->relname.data);
#ifndef __CYGWIN32__
	fd = FileNameOpenFile(path, O_RDWR | O_CREAT | O_EXCL, 0600);
#else
	fd = FileNameOpenFile(path, O_RDWR | O_CREAT | O_EXCL | O_BINARY, 0600);
#endif

	/*
	 * During bootstrap processing, we skip that check, because pg_time,
	 * pg_variable, and pg_log get created before their .bki file entries
	 * are processed.
	 *
	 * For cataloged relations,pg_class is guaranteed to have an unique
	 * record with the same relname by the unique index.
	 * So we are able to reuse existent files for new catloged relations.
	 * Currently we reuse them in the following cases.
	 * 1. they are empty.
	 * 2. they are used for Index relations and their size == BLCKSZ * 2.
	 */

	if (fd < 0)
	{
		if (!IsBootstrapProcessingMode() &&
			reln->rd_rel->relkind == RELKIND_UNCATALOGED)
			return -1;

#ifndef __CYGWIN32__
		fd = FileNameOpenFile(path, O_RDWR, 0600);
#else
		fd = FileNameOpenFile(path, O_RDWR | O_BINARY, 0600);
#endif
		if (fd < 0)
			return -1;
		if (!IsBootstrapProcessingMode())
		{
			bool	reuse = false;	
			int	len = FileSeek(fd, 0L, SEEK_END);

			if (len == 0)
				reuse = true;
			else if (reln->rd_rel->relkind == RELKIND_INDEX &&
				 len == BLCKSZ * 2)
				reuse = true;
			if (!reuse)
			{
				FileClose(fd);
				return -1;
			}
		}
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
	MemoryContext oldcxt;

	/* If the relation is already unlinked,we have nothing to do
	 * any more.
	 */
	if (reln->rd_unlinked && reln->rd_fd < 0)
		return SM_SUCCESS;
	/*
	 * Force all segments of the relation to be opened, so that we
	 * won't miss deleting any of them.
	 */
	nblocks = mdnblocks(reln);

	/*
	 * Clean out the mdfd vector, letting fd.c unlink the physical files.
	 *
	 * NOTE: We truncate the file(s) before deleting 'em, because if other
	 * backends are holding the files open, the unlink will fail on some
	 * platforms (think Microsoft).  Better a zero-size file gets left around
	 * than a big file.  Those other backends will be forced to close the
	 * relation by cache invalidation, but that probably hasn't happened yet.
	 */
	fd = RelationGetFile(reln);
	if (fd < 0)					/* should not happen */
		elog(ERROR, "mdunlink: mdnblocks didn't open relation");

	Md_fdvec[fd].mdfd_flags = (uint16) 0;

	oldcxt = MemoryContextSwitchTo(MdCxt);
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
	MemoryContextSwitchTo(oldcxt);

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
	long		pos, nbytes;
	int			nblocks;
	MdfdVec    *v;

	nblocks = mdnblocks(reln);
	v = _mdfd_getseg(reln, nblocks);

	if ((pos = FileSeek(v->mdfd_vfd, 0L, SEEK_END)) < 0)
		return SM_FAIL;

	if (pos % BLCKSZ != 0) /* the last block is incomplete */
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

	/* remember that we did a write, so we can sync at xact commit */
	v->mdfd_flags |= MDFD_DIRTY;

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
	path = relpath(reln->rd_rel->relname.data);

#ifndef __CYGWIN32__
	fd = FileNameOpenFile(path, O_RDWR, 0600);
#else
	fd = FileNameOpenFile(path, O_RDWR | O_BINARY, 0600);
#endif

	if (fd < 0)
	{
		/* in bootstrap mode, accept mdopen as substitute for mdcreate */
		if (IsBootstrapProcessingMode())
		{
#ifndef __CYGWIN32__
			fd = FileNameOpenFile(path, O_RDWR | O_CREAT | O_EXCL, 0600);
#else
			fd = FileNameOpenFile(path, O_RDWR | O_CREAT | O_EXCL | O_BINARY, 0600);
#endif
		}
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
	MdfdVec    *v;
	MemoryContext oldcxt;

	fd = RelationGetFile(reln);
	if (fd < 0)
		return SM_SUCCESS;		/* already closed, so no work */

	oldcxt = MemoryContextSwitchTo(MdCxt);
#ifndef LET_OS_MANAGE_FILESIZE
	for (v = &Md_fdvec[fd]; v != (MdfdVec *) NULL;)
	{
		MdfdVec    *ov = v;

		/* if not closed already */
		if (v->mdfd_vfd >= 0)
		{

			/*
			 * We sync the file descriptor so that we don't need to reopen
			 * it at transaction commit to force changes to disk.
			 */

			FileSync(v->mdfd_vfd);
			FileClose(v->mdfd_vfd);

			/* mark this file descriptor as clean in our private table */
			v->mdfd_flags &= ~MDFD_DIRTY;
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
			 * it at transaction commit to force changes to disk.
			 */

			FileSync(v->mdfd_vfd);
			FileClose(v->mdfd_vfd);

			/* mark this file descriptor as clean in our private table */
			v->mdfd_flags &= ~MDFD_DIRTY;
		}
	}
#endif
	MemoryContextSwitchTo(oldcxt);

	_fdvec_free(fd);

	/* be sure to mark relation closed */
	reln->rd_fd = -1;

	return SM_SUCCESS;
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

	v->mdfd_flags |= MDFD_DIRTY;

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

	/*
	 * By here, the block is written and changes have been forced to
	 * stable storage.	Mark the descriptor as clean until the next write,
	 * so we don't sync it again unnecessarily at transaction commit.
	 */

	v->mdfd_flags &= ~MDFD_DIRTY;

	return status;
}

/*
 *	mdblindwrt() -- Write a block to disk blind.
 *
 *		We have to be able to do this using only the name and OID of
 *		the database and relation in which the block belongs.  This
 *		is a synchronous write.
 */
int
mdblindwrt(char *dbstr,
		   char *relstr,
		   Oid dbid,
		   Oid relid,
		   BlockNumber blkno,
		   char *buffer)
{
	int			fd;
	int			segno;
	long		seekpos;
	int			status;
	char	   *path;

#ifndef LET_OS_MANAGE_FILESIZE
	int			nchars;

	/* be sure we have enough space for the '.segno', if any */
	segno = blkno / RELSEG_SIZE;
	if (segno > 0)
		nchars = 10;
	else
		nchars = 0;

	/* construct the path to the file and open it */
	/* system table? then put in system area... */
	if (dbid == (Oid) 0)
	{
		path = (char *) palloc(strlen(DataDir) + sizeof(NameData) + 2 + nchars);
		if (segno == 0)
			sprintf(path, "%s/%s", DataDir, relstr);
		else
			sprintf(path, "%s/%s.%d", DataDir, relstr, segno);
	}
	/* user table? then put in user database area... */
	else if (dbid == MyDatabaseId)
	{
		extern char *DatabasePath;

		path = (char *) palloc(strlen(DatabasePath) + 2 * sizeof(NameData) + 2 + nchars);
		if (segno == 0)
			sprintf(path, "%s%c%s", DatabasePath, SEP_CHAR, relstr);
		else
			sprintf(path, "%s%c%s.%d", DatabasePath, SEP_CHAR, relstr, segno);
	}
	else
/* this is work arround only !!! */
	{
		char		dbpath[MAXPGPATH];
		Oid			id;
		char	   *tmpPath;

		GetRawDatabaseInfo(dbstr, &id, dbpath);

		if (id != dbid)
			elog(FATAL, "mdblindwrt: oid of db %s is not %u", dbstr, dbid);
		tmpPath = ExpandDatabasePath(dbpath);
		if (tmpPath == NULL)
			elog(FATAL, "mdblindwrt: can't expand path for db %s", dbstr);
		path = (char *) palloc(strlen(tmpPath) + 2 * sizeof(NameData) + 2 + nchars);
		if (segno == 0)
			sprintf(path, "%s%c%s", tmpPath, SEP_CHAR, relstr);
		else
			sprintf(path, "%s%c%s.%d", tmpPath, SEP_CHAR, relstr, segno);
		pfree(tmpPath);
	}
#else
	/* construct the path to the file and open it */
	/* system table? then put in system area... */
	if (dbid == (Oid) 0)
	{
		path = (char *) palloc(strlen(DataDir) + sizeof(NameData) + 2);
		sprintf(path, "%s/%s", DataDir, relstr);
	}
	/* user table? then put in user database area... */
	else if (dbid == MyDatabaseId)
	{
		extern char *DatabasePath;

		path = (char *) palloc(strlen(DatabasePath) + 2 * sizeof(NameData) + 2);
		sprintf(path, "%s%c%s", DatabasePath, SEP_CHAR, relstr);
	}
	else
/* this is work arround only !!! */
	{
		char		dbpath[MAXPGPATH];
		Oid			id;
		char	   *tmpPath;

		GetRawDatabaseInfo(dbstr, &id, dbpath);

		if (id != dbid)
			elog(FATAL, "mdblindwrt: oid of db %s is not %u", dbstr, dbid);
		tmpPath = ExpandDatabasePath(dbpath);
		if (tmpPath == NULL)
			elog(FATAL, "mdblindwrt: can't expand path for db %s", dbstr);
		path = (char *) palloc(strlen(tmpPath) + 2 * sizeof(NameData) + 2);
		sprintf(path, "%s%c%s", tmpPath, SEP_CHAR, relstr);
		pfree(tmpPath);
	}
#endif

#ifndef __CYGWIN32__
	if ((fd = open(path, O_RDWR, 0600)) < 0)
#else
	if ((fd = open(path, O_RDWR | O_BINARY, 0600)) < 0)
#endif
		return SM_FAIL;

	/* seek to the right spot */
#ifndef LET_OS_MANAGE_FILESIZE
	seekpos = (long) (BLCKSZ * (blkno % RELSEG_SIZE));
#else
	seekpos = (long) (BLCKSZ * (blkno));
#endif

	if (lseek(fd, seekpos, SEEK_SET) != seekpos)
	{
		close(fd);
		return SM_FAIL;
	}

	status = SM_SUCCESS;

	/* write and sync the block */
	if (write(fd, buffer, BLCKSZ) != BLCKSZ || (pg_fsync(fd) < 0))
		status = SM_FAIL;

	if (close(fd) < 0)
		status = SM_FAIL;

	pfree(path);

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
				v->mdfd_chain = _mdfd_openseg(reln, segno, O_CREAT);
				if (v->mdfd_chain == (MdfdVec *) NULL)
					elog(ERROR, "cannot count blocks for %s -- open failed",
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
	MemoryContext oldcxt;
	int			priorblocks;
#endif

	/* NOTE: mdnblocks makes sure we have opened all existing segments,
	 * so that truncate/delete loop will get them all!
	 */
	curnblk = mdnblocks(reln);
	if (nblocks < 0 || nblocks > curnblk)
		return -1;				/* bogus request */
	if (nblocks == curnblk)
		return nblocks;			/* no work */

	fd = _mdfd_getrelnfd(reln);
	v = &Md_fdvec[fd];

#ifndef LET_OS_MANAGE_FILESIZE
	oldcxt = MemoryContextSwitchTo(MdCxt);
	priorblocks = 0;
	while (v != (MdfdVec *) NULL)
	{
		MdfdVec    *ov = v;

		if (priorblocks > nblocks)
		{
			/* This segment is no longer wanted at all (and has already been
			 * unlinked from the mdfd_chain).
			 * We truncate the file before deleting it because if other
			 * backends are holding the file open, the unlink will fail on
			 * some platforms.  Better a zero-size file gets left around than
			 * a big file...
			 */
			FileTruncate(v->mdfd_vfd, 0);
			FileUnlink(v->mdfd_vfd);
			v = v->mdfd_chain;
			Assert(ov != &Md_fdvec[fd]); /* we never drop the 1st segment */
			pfree(ov);
		}
		else if (priorblocks + RELSEG_SIZE > nblocks)
		{
			/* This is the last segment we want to keep.
			 * Truncate the file to the right length, and clear chain link
			 * that points to any remaining segments (which we shall zap).
			 * NOTE: if nblocks is exactly a multiple K of RELSEG_SIZE,
			 * we will truncate the K+1st segment to 0 length but keep it.
			 * This is mainly so that the right thing happens if nblocks=0.
			 */
			int lastsegblocks = nblocks - priorblocks;
			if (FileTruncate(v->mdfd_vfd, lastsegblocks * BLCKSZ) < 0)
				return -1;
			v->mdfd_lstbcnt = lastsegblocks;
			v = v->mdfd_chain;
			ov->mdfd_chain = (MdfdVec *) NULL;
		}
		else
		{
			/* We still need this segment and 0 or more blocks beyond it,
			 * so nothing to do here.
			 */
			v = v->mdfd_chain;
		}
		priorblocks += RELSEG_SIZE;
	}
	MemoryContextSwitchTo(oldcxt);
#else
	if (FileTruncate(v->mdfd_vfd, nblocks * BLCKSZ) < 0)
		return -1;
	v->mdfd_lstbcnt = nblocks;
#endif

	return nblocks;

}	/* mdtruncate */

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
#ifdef XLOG
	sync();
	sleep(1);
	sync();
	return SM_SUCCESS;
#else
	int			i;
	MdfdVec    *v;

	for (i = 0; i < CurFd; i++)
	{
#ifndef LET_OS_MANAGE_FILESIZE
		for (v = &Md_fdvec[i]; v != (MdfdVec *) NULL; v = v->mdfd_chain)
#else
		v = &Md_fdvec[i];
		if (v != (MdfdVec *) NULL)
#endif
		{
			if (v->mdfd_flags & MDFD_DIRTY)
			{
				if (FileSync(v->mdfd_vfd) < 0)
					return SM_FAIL;

				v->mdfd_flags &= ~MDFD_DIRTY;
			}
		}
	}

	return SM_SUCCESS;
#endif	/* XLOG */
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
	int			i;
	MdfdVec    *v;

	for (i = 0; i < CurFd; i++)
	{
#ifndef LET_OS_MANAGE_FILESIZE
		for (v = &Md_fdvec[i]; v != (MdfdVec *) NULL; v = v->mdfd_chain)
			v->mdfd_flags &= ~MDFD_DIRTY;
#else
		v = &Md_fdvec[i];
		v->mdfd_flags &= ~MDFD_DIRTY;
#endif
	}

	return SM_SUCCESS;
}

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
	MemoryContext oldcxt;

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

	oldcxt = MemoryContextSwitchTo(MdCxt);

	nvec = (MdfdVec *) palloc(Nfds * sizeof(MdfdVec));
	MemSet(nvec, 0, Nfds * sizeof(MdfdVec));
	memmove(nvec, (char *) Md_fdvec, CurFd * sizeof(MdfdVec));
	pfree(Md_fdvec);

	MemoryContextSwitchTo(oldcxt);

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
	MemoryContext oldcxt;
	MdfdVec    *v;
	int			fd;
	bool		dofree;
	char	   *path,
			   *fullpath;

	/* be sure we have enough space for the '.segno', if any */
	path = relpath(RelationGetRelationName(reln)->data);

	dofree = false;
	if (segno > 0)
	{
		dofree = true;
		fullpath = (char *) palloc(strlen(path) + 12);
		sprintf(fullpath, "%s.%d", path, segno);
	}
	else
		fullpath = path;

	/* open the file */
#ifndef __CYGWIN32__
	fd = FileNameOpenFile(fullpath, O_RDWR | oflags, 0600);
#else
	fd = FileNameOpenFile(fullpath, O_RDWR | O_BINARY | oflags, 0600);
#endif

	if (dofree)
		pfree(fullpath);

	if (fd < 0)
		return (MdfdVec *) NULL;

	/* allocate an mdfdvec entry for it */
	oldcxt = MemoryContextSwitchTo(MdCxt);
	v = (MdfdVec *) palloc(sizeof(MdfdVec));
	MemoryContextSwitchTo(oldcxt);

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
			v->mdfd_chain = _mdfd_openseg(reln, i, O_CREAT);

			if (v->mdfd_chain == (MdfdVec *) NULL)
				elog(ERROR, "cannot open segment %d of relation %s",
					 i, RelationGetRelationName(reln));
		}
		v = v->mdfd_chain;
	}
#else
	v = &Md_fdvec[fd];
#endif

	return v;
}

static BlockNumber
_mdnblocks(File file, Size blcksz)
{
	long		len;

	len = FileSeek(file, 0L, SEEK_END);
	if (len < 0) return 0;	/* on failure, assume file is empty */
	return (BlockNumber) (len / blcksz);
}
