/*-------------------------------------------------------------------------
 *
 * smgr.c
 *	  public interface routines to storage manager switch.
 *
 *	  All file system operations in POSTGRES dispatch through these
 *	  routines.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/smgr/smgr.c,v 1.40 2000/10/16 14:52:12 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/smgr.h"

static void smgrshutdown(void);

typedef struct f_smgr
{
	int			(*smgr_init) (void);	/* may be NULL */
	int			(*smgr_shutdown) (void);		/* may be NULL */
	int			(*smgr_create) (Relation reln);
	int			(*smgr_unlink) (Relation reln);
	int			(*smgr_extend) (Relation reln, char *buffer);
	int			(*smgr_open) (Relation reln);
	int			(*smgr_close) (Relation reln);
	int			(*smgr_read) (Relation reln, BlockNumber blocknum,
										  char *buffer);
	int			(*smgr_write) (Relation reln, BlockNumber blocknum,
										   char *buffer);
	int			(*smgr_flush) (Relation reln, BlockNumber blocknum,
										   char *buffer);
#ifdef OLD_FILE_NAMING
	int			(*smgr_blindwrt) (char *dbname, char *relname,
											  Oid dbid, Oid relid,
										 BlockNumber blkno, char *buffer,
											  bool dofsync);
#else
	int			(*smgr_blindwrt) (RelFileNode rnode, BlockNumber blkno, 
										char *buffer, bool dofsync);
#endif
	int			(*smgr_markdirty) (Relation reln, BlockNumber blkno);
#ifdef OLD_FILE_NAMING
	int			(*smgr_blindmarkdirty) (char *dbname, char *relname,
													Oid dbid, Oid relid,
													BlockNumber blkno);
#else
	int			(*smgr_blindmarkdirty) (RelFileNode, BlockNumber blkno);
#endif
	int			(*smgr_nblocks) (Relation reln);
	int			(*smgr_truncate) (Relation reln, int nblocks);
	int			(*smgr_commit) (void);	/* may be NULL */
	int			(*smgr_abort) (void);	/* may be NULL */
} f_smgr;

/*
 *	The weird placement of commas in this init block is to keep the compiler
 *	happy, regardless of what storage managers we have (or don't have).
 */

static f_smgr smgrsw[] = {

	/* magnetic disk */
	{mdinit, NULL, mdcreate, mdunlink, mdextend, mdopen, mdclose,
		mdread, mdwrite, mdflush, mdblindwrt, mdmarkdirty, mdblindmarkdirty,
	mdnblocks, mdtruncate, mdcommit, mdabort},

#ifdef STABLE_MEMORY_STORAGE
	/* main memory */
	{mminit, mmshutdown, mmcreate, mmunlink, mmextend, mmopen, mmclose,
		mmread, mmwrite, mmflush, mmblindwrt, mmmarkdirty, mmblindmarkdirty,
	mmnblocks, NULL, mmcommit, mmabort},

#endif
};

/*
 *	This array records which storage managers are write-once, and which
 *	support overwrite.	A 'true' entry means that the storage manager is
 *	write-once.  In the best of all possible worlds, there would be no
 *	write-once storage managers.
 */

#ifdef NOT_USED
static bool smgrwo[] = {
	false,						/* magnetic disk */
#ifdef STABLE_MEMORY_STORAGE
	false,						/* main memory */
#endif
};

#endif

static int	NSmgr = lengthof(smgrsw);

/*
 *	smgrinit(), smgrshutdown() -- Initialize or shut down all storage
 *								  managers.
 *
 */
int
smgrinit()
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_init)
		{
			if ((*(smgrsw[i].smgr_init)) () == SM_FAIL)
				elog(FATAL, "initialization failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}

	/* register the shutdown proc */
	on_proc_exit(smgrshutdown, 0);

	return SM_SUCCESS;
}

static void
smgrshutdown(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_shutdown)
		{
			if ((*(smgrsw[i].smgr_shutdown)) () == SM_FAIL)
				elog(FATAL, "shutdown failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}
}

/*
 *	smgrcreate() -- Create a new relation.
 *
 *		This routine takes a reldesc, creates the relation on the appropriate
 *		device, and returns a file descriptor for it.
 */
int
smgrcreate(int16 which, Relation reln)
{
	int			fd;

	if ((fd = (*(smgrsw[which].smgr_create)) (reln)) < 0)
		elog(ERROR, "cannot create %s: %m", RelationGetRelationName(reln));

	return fd;
}

/*
 *	smgrunlink() -- Unlink a relation.
 *
 *		The relation is removed from the store.
 */
int
smgrunlink(int16 which, Relation reln)
{
	int			status;

	if ((status = (*(smgrsw[which].smgr_unlink)) (reln)) == SM_FAIL)
		elog(ERROR, "cannot unlink %s: %m", RelationGetRelationName(reln));

	return status;
}

/*
 *	smgrextend() -- Add a new block to a file.
 *
 *		Returns SM_SUCCESS on success; aborts the current transaction on
 *		failure.
 */
int
smgrextend(int16 which, Relation reln, char *buffer)
{
	int			status;

	status = (*(smgrsw[which].smgr_extend)) (reln, buffer);

	if (status == SM_FAIL)
		elog(ERROR, "cannot extend %s: %m.\n\tCheck free disk space.",
			 RelationGetRelationName(reln));

	return status;
}

/*
 *	smgropen() -- Open a relation using a particular storage manager.
 *
 *		Returns the fd for the open relation on success, aborts the
 *		transaction on failure.
 */
int
smgropen(int16 which, Relation reln)
{
	int			fd;

	if ((fd = (*(smgrsw[which].smgr_open)) (reln)) < 0 &&
		!reln->rd_unlinked)
		elog(ERROR, "cannot open %s: %m", RelationGetRelationName(reln));

	return fd;
}

/*
 *	smgrclose() -- Close a relation.
 *
 *		NOTE: underlying manager should allow case where relation is
 *		already closed.  Indeed relation may have been unlinked!
 *		This is currently called only from RelationFlushRelation() when
 *		the relation cache entry is about to be dropped; could be doing
 *		simple relation cache clear, or finishing up DROP TABLE.
 *
 *		Returns SM_SUCCESS on success, aborts on failure.
 */
int
smgrclose(int16 which, Relation reln)
{
	if ((*(smgrsw[which].smgr_close)) (reln) == SM_FAIL)
		elog(ERROR, "cannot close %s: %m", RelationGetRelationName(reln));

	return SM_SUCCESS;
}

/*
 *	smgrread() -- read a particular block from a relation into the supplied
 *				  buffer.
 *
 *		This routine is called from the buffer manager in order to
 *		instantiate pages in the shared buffer cache.  All storage managers
 *		return pages in the format that POSTGRES expects.  This routine
 *		dispatches the read.  On success, it returns SM_SUCCESS.  On failure,
 *		the current transaction is aborted.
 */
int
smgrread(int16 which, Relation reln, BlockNumber blocknum, char *buffer)
{
	int			status;

	status = (*(smgrsw[which].smgr_read)) (reln, blocknum, buffer);

	if (status == SM_FAIL)
		elog(ERROR, "cannot read block %d of %s: %m",
			 blocknum, RelationGetRelationName(reln));

	return status;
}

/*
 *	smgrwrite() -- Write the supplied buffer out.
 *
 *		This is not a synchronous write -- the interface for that is
 *		smgrflush().  The buffer is written out via the appropriate
 *		storage manager.  This routine returns SM_SUCCESS or aborts
 *		the current transaction.
 */
int
smgrwrite(int16 which, Relation reln, BlockNumber blocknum, char *buffer)
{
	int			status;

	status = (*(smgrsw[which].smgr_write)) (reln, blocknum, buffer);

	if (status == SM_FAIL)
		elog(ERROR, "cannot write block %d of %s: %m",
			 blocknum, RelationGetRelationName(reln));

	return status;
}

/*
 *	smgrflush() -- A synchronous smgrwrite().
 */
int
smgrflush(int16 which, Relation reln, BlockNumber blocknum, char *buffer)
{
	int			status;

	status = (*(smgrsw[which].smgr_flush)) (reln, blocknum, buffer);

	if (status == SM_FAIL)
		elog(ERROR, "cannot flush block %d of %s to stable store: %m",
			 blocknum, RelationGetRelationName(reln));

	return status;
}

/*
 *	smgrblindwrt() -- Write a page out blind.
 *
 *		In some cases, we may find a page in the buffer cache that we
 *		can't make a reldesc for.  This happens, for example, when we
 *		want to reuse a dirty page that was written by a transaction
 *		that has not yet committed, which created a new relation.  In
 *		this case, the buffer manager will call smgrblindwrt() with
 *		the name and OID of the database and the relation to which the
 *		buffer belongs.  Every storage manager must be able to force
 *		this page down to stable storage in this circumstance.	The
 *		write should be synchronous if dofsync is true.
 */
#ifdef OLD_FILE_NAMING
int
smgrblindwrt(int16 which,
			 char *dbname,
			 char *relname,
			 Oid dbid,
			 Oid relid,
			 BlockNumber blkno,
			 char *buffer,
			 bool dofsync)
{
	char	   *dbstr;
	char	   *relstr;
	int			status;

	/* strdup here is probably redundant */
	dbstr = pstrdup(dbname);
	relstr = pstrdup(relname);

	status = (*(smgrsw[which].smgr_blindwrt)) (dbstr, relstr, dbid, relid,
											   blkno, buffer, dofsync);

	if (status == SM_FAIL)
		elog(ERROR, "cannot write block %d of %s [%s] blind: %m",
			 blkno, relstr, dbstr);

	pfree(dbstr);
	pfree(relstr);

	return status;
}

#else

int
smgrblindwrt(int16 which,
			 RelFileNode rnode,
			 BlockNumber blkno,
			 char *buffer,
			 bool dofsync)
{
	int			status;

	status = (*(smgrsw[which].smgr_blindwrt)) (rnode, blkno, buffer, dofsync);

	if (status == SM_FAIL)
		elog(ERROR, "cannot write block %d of %u/%u blind: %m",
			 blkno, rnode.tblNode, rnode.relNode);

	return status;
}
#endif

/*
 *	smgrmarkdirty() -- Mark a page dirty (needs fsync).
 *
 *		Mark the specified page as needing to be fsync'd before commit.
 *		Ordinarily, the storage manager will do this implicitly during
 *		smgrwrite().  However, the buffer manager may discover that some
 *		other backend has written a buffer that we dirtied in the current
 *		transaction.  In that case, we still need to fsync the file to be
 *		sure the page is down to disk before we commit.
 */
int
smgrmarkdirty(int16 which,
			  Relation reln,
			  BlockNumber blkno)
{
	int			status;

	status = (*(smgrsw[which].smgr_markdirty)) (reln, blkno);

	if (status == SM_FAIL)
		elog(ERROR, "cannot mark block %d of %s: %m",
			 blkno, RelationGetRelationName(reln));

	return status;
}

/*
 *	smgrblindmarkdirty() -- Mark a page dirty, "blind".
 *
 *		Just like smgrmarkdirty, except we don't have a reldesc.
 */
#ifdef OLD_FILE_NAMING
int
smgrblindmarkdirty(int16 which,
				   char *dbname,
				   char *relname,
				   Oid dbid,
				   Oid relid,
				   BlockNumber blkno)
{
	char	   *dbstr;
	char	   *relstr;
	int			status;

	/* strdup here is probably redundant */
	dbstr = pstrdup(dbname);
	relstr = pstrdup(relname);

	status = (*(smgrsw[which].smgr_blindmarkdirty)) (dbstr, relstr,
													 dbid, relid,
													 blkno);

	if (status == SM_FAIL)
		elog(ERROR, "cannot mark block %d of %s [%s] blind: %m",
			 blkno, relstr, dbstr);

	pfree(dbstr);
	pfree(relstr);

	return status;
}

#else

int
smgrblindmarkdirty(int16 which,
				   RelFileNode rnode,
				   BlockNumber blkno)
{
	int			status;

	status = (*(smgrsw[which].smgr_blindmarkdirty)) (rnode, blkno);

	if (status == SM_FAIL)
		elog(ERROR, "cannot mark block %d of %u/%u blind: %m",
			 blkno, rnode.tblNode, rnode.relNode);

	return status;
}
#endif

/*
 *	smgrnblocks() -- Calculate the number of POSTGRES blocks in the
 *					 supplied relation.
 *
 *		Returns the number of blocks on success, aborts the current
 *		transaction on failure.
 */
int
smgrnblocks(int16 which, Relation reln)
{
	int			nblocks;

	if ((nblocks = (*(smgrsw[which].smgr_nblocks)) (reln)) < 0)
		elog(ERROR, "cannot count blocks for %s: %m",
			 RelationGetRelationName(reln));

	return nblocks;
}

/*
 *	smgrtruncate() -- Truncate supplied relation to a specified number
 *						of blocks
 *
 *		Returns the number of blocks on success, aborts the current
 *		transaction on failure.
 */
int
smgrtruncate(int16 which, Relation reln, int nblocks)
{
	int			newblks;

	newblks = nblocks;
	if (smgrsw[which].smgr_truncate)
	{
		if ((newblks = (*(smgrsw[which].smgr_truncate)) (reln, nblocks)) < 0)
			elog(ERROR, "cannot truncate %s to %d blocks: %m",
				 RelationGetRelationName(reln), nblocks);
	}

	return newblks;
}

/*
 *	smgrcommit(), smgrabort() -- Commit or abort changes made during the
 *								 current transaction.
 */
int
smgrcommit()
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_commit)
		{
			if ((*(smgrsw[i].smgr_commit)) () == SM_FAIL)
				elog(FATAL, "transaction commit failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}

	return SM_SUCCESS;
}

int
smgrabort()
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_abort)
		{
			if ((*(smgrsw[i].smgr_abort)) () == SM_FAIL)
				elog(FATAL, "transaction abort failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}

	return SM_SUCCESS;
}

#ifdef NOT_USED
bool
smgriswo(int16 smgrno)
{
	if (smgrno < 0 || smgrno >= NSmgr)
		elog(ERROR, "illegal storage manager number %d", smgrno);

	return smgrwo[smgrno];
}

#endif
