/*-------------------------------------------------------------------------
 *
 * smgr.c--
 *    public interface routines to storage manager switch.
 *
 *    All file system operations in POSTGRES dispatch through these
 *    routines.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/storage/smgr/smgr.c,v 1.5 1996/11/27 07:25:52 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"

#include "storage/ipc.h"
#include "storage/block.h"
#include "storage/smgr.h"
#include "utils/rel.h"
#include "utils/palloc.h"

typedef struct f_smgr {
    int		(*smgr_init)();		/* may be NULL */
    int		(*smgr_shutdown)();	/* may be NULL */
    int		(*smgr_create)();
    int		(*smgr_unlink)();
    int		(*smgr_extend)();
    int		(*smgr_open)();
    int		(*smgr_close)();
    int		(*smgr_read)();
    int		(*smgr_write)();
    int		(*smgr_flush)();
    int		(*smgr_blindwrt)();
    int		(*smgr_nblocks)();
    int		(*smgr_truncate)();
    int		(*smgr_commit)();	/* may be NULL */
    int		(*smgr_abort)();	/* may be NULL */
} f_smgr;

/*
 *  The weird placement of commas in this init block is to keep the compiler
 *  happy, regardless of what storage managers we have (or don't have).
 */

static f_smgr smgrsw[] = {

    /* magnetic disk */
    { mdinit, NULL, mdcreate, mdunlink, mdextend, mdopen, mdclose,
      mdread, mdwrite, mdflush, mdblindwrt, mdnblocks, mdtruncate, 
      mdcommit, mdabort },

#ifdef MAIN_MEMORY
    /* main memory */
    { mminit, mmshutdown, mmcreate, mmunlink, mmextend, mmopen, mmclose,
      mmread, mmwrite, mmflush, mmblindwrt, mmnblocks, NULL, 
      mmcommit, mmabort },

#endif /* MAIN_MEMORY */
};

/*
 *  This array records which storage managers are write-once, and which
 *  support overwrite.  A 'true' entry means that the storage manager is
 *  write-once.  In the best of all possible worlds, there would be no
 *  write-once storage managers.
 */

static bool smgrwo[] = {
    false,		/* magnetic disk */
#ifdef MAIN_MEMORY
    false,		/* main memory*/
#endif /* MAIN_MEMORY */
};
static int NSmgr = lengthof(smgrsw);

/*
 *  smgrinit(), smgrshutdown() -- Initialize or shut down all storage
 *				  managers.
 *
 */
int
smgrinit()
{
    int i;

    for (i = 0; i < NSmgr; i++) {
	if (smgrsw[i].smgr_init) {
	    if ((*(smgrsw[i].smgr_init))() == SM_FAIL)
		elog(FATAL, "initialization failed on %s", smgrout(i));
	}
    }

    /* register the shutdown proc */
    on_exitpg(smgrshutdown, 0);

    return (SM_SUCCESS);
}

void
smgrshutdown(int dummy)
{
    int i;

    for (i = 0; i < NSmgr; i++) {
	if (smgrsw[i].smgr_shutdown) {
	    if ((*(smgrsw[i].smgr_shutdown))() == SM_FAIL)
		elog(FATAL, "shutdown failed on %s", smgrout(i));
	}
    }
}

/*
 *  smgrcreate() -- Create a new relation.
 *
 *	This routine takes a reldesc, creates the relation on the appropriate
 *	device, and returns a file descriptor for it.
 */
int
smgrcreate(int16 which, Relation reln)
{
    int fd;

    if ((fd = (*(smgrsw[which].smgr_create))(reln)) < 0)
	elog(WARN, "cannot open %.*s",
	     NAMEDATALEN, &(reln->rd_rel->relname.data[0]));

    return (fd);
}

/*
 *  smgrunlink() -- Unlink a relation.
 *
 *	The relation is removed from the store.
 */
int
smgrunlink(int16 which, Relation reln)
{
    int status;

    if ((status = (*(smgrsw[which].smgr_unlink))(reln)) == SM_FAIL)
	elog(WARN, "cannot unlink %.*s",
	     NAMEDATALEN, &(reln->rd_rel->relname.data[0]));

    return (status);
}

/*
 *  smgrextend() -- Add a new block to a file.
 *
 *	Returns SM_SUCCESS on success; aborts the current transaction on
 *	failure.
 */
int
smgrextend(int16 which, Relation reln, char *buffer)
{
    int status;

    status = (*(smgrsw[which].smgr_extend))(reln, buffer);

    if (status == SM_FAIL)
	elog(WARN, "%.*s: cannot extend",
	     NAMEDATALEN, &(reln->rd_rel->relname.data[0]));

    return (status);
}

/*
 *  smgropen() -- Open a relation using a particular storage manager.
 *
 *	Returns the fd for the open relation on success, aborts the
 *	transaction on failure.
 */
int
smgropen(int16 which, Relation reln)
{
    int fd;

    if ((fd = (*(smgrsw[which].smgr_open))(reln)) < 0)
	elog(WARN, "cannot open %.*s",
	     NAMEDATALEN, &(reln->rd_rel->relname.data[0]));

    return (fd);
}

/*
 *  smgrclose() -- Close a relation.
 *
 *	Returns SM_SUCCESS on success, aborts on failure.
 */
int
smgrclose(int16 which, Relation reln)
{
    if ((*(smgrsw[which].smgr_close))(reln) == SM_FAIL)
	elog(WARN, "cannot close %.*s",
	     NAMEDATALEN, &(reln->rd_rel->relname.data[0]));

    return (SM_SUCCESS);
}

/*
 *  smgrread() -- read a particular block from a relation into the supplied
 *		  buffer.
 *
 *	This routine is called from the buffer manager in order to
 *	instantiate pages in the shared buffer cache.  All storage managers
 *	return pages in the format that POSTGRES expects.  This routine
 *	dispatches the read.  On success, it returns SM_SUCCESS.  On failure,
 *	the current transaction is aborted.
 */
int
smgrread(int16 which, Relation reln, BlockNumber blocknum, char *buffer)
{
    int status;

    status = (*(smgrsw[which].smgr_read))(reln, blocknum, buffer);

    if (status == SM_FAIL)
	elog(WARN, "cannot read block %d of %.*s",
	     blocknum, NAMEDATALEN, &(reln->rd_rel->relname.data[0]));

    return (status);
}

/*
 *  smgrwrite() -- Write the supplied buffer out.
 *
 *	This is not a synchronous write -- the interface for that is
 *	smgrflush().  The buffer is written out via the appropriate
 *	storage manager.  This routine returns SM_SUCCESS or aborts
 *	the current transaction.
 */
int
smgrwrite(int16 which, Relation reln, BlockNumber blocknum, char *buffer)
{
    int status;

    status = (*(smgrsw[which].smgr_write))(reln, blocknum, buffer);

    if (status == SM_FAIL)
	elog(WARN, "cannot write block %d of %.*s",
	     blocknum, NAMEDATALEN, &(reln->rd_rel->relname.data[0]));

    return (status);
}

/*
 *  smgrflush() -- A synchronous smgrwrite().
 */
int
smgrflush(int16 which, Relation reln, BlockNumber blocknum, char *buffer)
{
    int status;

    status = (*(smgrsw[which].smgr_flush))(reln, blocknum, buffer);

    if (status == SM_FAIL)
	elog(WARN, "cannot flush block %d of %.*s to stable store",
	     blocknum, NAMEDATALEN, &(reln->rd_rel->relname.data[0]));

    return (status);
}

/*
 *  smgrblindwrt() -- Write a page out blind.
 *
 *	In some cases, we may find a page in the buffer cache that we
 *	can't make a reldesc for.  This happens, for example, when we
 *	want to reuse a dirty page that was written by a transaction
 *	that has not yet committed, which created a new relation.  In
 *	this case, the buffer manager will call smgrblindwrt() with
 *	the name and OID of the database and the relation to which the
 *	buffer belongs.  Every storage manager must be able to force
 *	this page down to stable storage in this circumstance.
 */
int
smgrblindwrt(int16 which,
	     char *dbname,
	     char *relname,
	     Oid dbid,
	     Oid relid,
	     BlockNumber blkno,
	     char *buffer)
{
    char *dbstr;
    char *relstr;
    int status;

    dbstr = pstrdup(dbname);
    relstr = pstrdup(relname);

    status = (*(smgrsw[which].smgr_blindwrt))(dbstr, relstr, dbid, relid,
					      blkno, buffer);

    if (status == SM_FAIL)
	elog(WARN, "cannot write block %d of %s [%s] blind",
	     blkno, relstr, dbstr);

    pfree(dbstr);
    pfree(relstr);

    return (status);
}

/*
 *  smgrnblocks() -- Calculate the number of POSTGRES blocks in the
 *		     supplied relation.
 *
 *	Returns the number of blocks on success, aborts the current
 *	transaction on failure.
 */
int
smgrnblocks(int16 which, Relation reln)
{
    int nblocks;

    if ((nblocks = (*(smgrsw[which].smgr_nblocks))(reln)) < 0)
	elog(WARN, "cannot count blocks for %.*s",
	     NAMEDATALEN, &(reln->rd_rel->relname.data[0]));

    return (nblocks);
}

/*
 *  smgrtruncate() -- Truncate supplied relation to a specified number
 *			of blocks
 *
 *	Returns the number of blocks on success, aborts the current
 *	transaction on failure.
 */
int
smgrtruncate(int16 which, Relation reln, int nblocks)
{
    int newblks;
    
    newblks = nblocks;
    if (smgrsw[which].smgr_truncate)
    {
    	if ((newblks = (*(smgrsw[which].smgr_truncate))(reln, nblocks)) < 0)
	    elog(WARN, "cannot truncate %.*s to %d blocks",
		NAMEDATALEN, &(reln->rd_rel->relname.data[0]), nblocks);
    }

    return (newblks);
}

/*
 *  smgrcommit(), smgrabort() -- Commit or abort changes made during the
 *				 current transaction.
 */
int
smgrcommit()
{
    int i;

    for (i = 0; i < NSmgr; i++) {
	if (smgrsw[i].smgr_commit) {
	    if ((*(smgrsw[i].smgr_commit))() == SM_FAIL)
		elog(FATAL, "transaction commit failed on %s", smgrout(i));
	}
    }

    return (SM_SUCCESS);
}

int
smgrabort()
{
    int i;

    for (i = 0; i < NSmgr; i++) {
	if (smgrsw[i].smgr_abort) {
	    if ((*(smgrsw[i].smgr_abort))() == SM_FAIL)
		elog(FATAL, "transaction abort failed on %s", smgrout(i));
	}
    }

    return (SM_SUCCESS);
}

bool
smgriswo(int16 smgrno)
{
    if (smgrno < 0 || smgrno >= NSmgr)
	elog(WARN, "illegal storage manager number %d", smgrno);

    return (smgrwo[smgrno]);
}
