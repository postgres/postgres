/*-------------------------------------------------------------------------
 *
 * smgr.c
 *	  public interface routines to storage manager switch.
 *
 *	  All file system operations in POSTGRES dispatch through these
 *	  routines.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/smgr/smgr.c,v 1.65 2003/09/25 06:58:02 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/ipc.h"
#include "storage/smgr.h"
#include "utils/memutils.h"


static void smgrshutdown(void);

typedef struct f_smgr
{
	int			(*smgr_init) (void);	/* may be NULL */
	int			(*smgr_shutdown) (void);		/* may be NULL */
	int			(*smgr_create) (Relation reln);
	int			(*smgr_unlink) (RelFileNode rnode);
	int			(*smgr_extend) (Relation reln, BlockNumber blocknum,
											char *buffer);
	int			(*smgr_open) (Relation reln);
	int			(*smgr_close) (Relation reln);
	int			(*smgr_read) (Relation reln, BlockNumber blocknum,
										  char *buffer);
	int			(*smgr_write) (Relation reln, BlockNumber blocknum,
										   char *buffer);
	int			(*smgr_blindwrt) (RelFileNode rnode, BlockNumber blkno,
											  char *buffer);
	BlockNumber (*smgr_nblocks) (Relation reln);
	BlockNumber (*smgr_truncate) (Relation reln, BlockNumber nblocks);
	int			(*smgr_commit) (void);	/* may be NULL */
	int			(*smgr_abort) (void);	/* may be NULL */
	int			(*smgr_sync) (void);
} f_smgr;

/*
 *	The weird placement of commas in this init block is to keep the compiler
 *	happy, regardless of what storage managers we have (or don't have).
 */

static f_smgr smgrsw[] = {

	/* magnetic disk */
	{mdinit, NULL, mdcreate, mdunlink, mdextend, mdopen, mdclose,
		mdread, mdwrite, mdblindwrt,
		mdnblocks, mdtruncate, mdcommit, mdabort, mdsync
	},

#ifdef STABLE_MEMORY_STORAGE
	/* main memory */
	{mminit, mmshutdown, mmcreate, mmunlink, mmextend, mmopen, mmclose,
		mmread, mmwrite, mmblindwrt,
	mmnblocks, NULL, mmcommit, mmabort, NULL},
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
 * We keep a list of all relations (represented as RelFileNode values)
 * that have been created or deleted in the current transaction.  When
 * a relation is created, we create the physical file immediately, but
 * remember it so that we can delete the file again if the current
 * transaction is aborted.	Conversely, a deletion request is NOT
 * executed immediately, but is just entered in the list.  When and if
 * the transaction commits, we can delete the physical file.
 *
 * NOTE: the list is kept in TopMemoryContext to be sure it won't disappear
 * unbetimes.  It'd probably be OK to keep it in TopTransactionContext,
 * but I'm being paranoid.
 */

typedef struct PendingRelDelete
{
	RelFileNode relnode;		/* relation that may need to be deleted */
	int16		which;			/* which storage manager? */
	bool		isTemp;			/* is it a temporary relation? */
	bool		atCommit;		/* T=delete at commit; F=delete at abort */
	struct PendingRelDelete *next;		/* linked-list link */
} PendingRelDelete;

static PendingRelDelete *pendingDeletes = NULL; /* head of linked list */


/*
 *	smgrinit(), smgrshutdown() -- Initialize or shut down all storage
 *								  managers.
 *
 */
int
smgrinit(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_init)
		{
			if ((*(smgrsw[i].smgr_init)) () == SM_FAIL)
				elog(FATAL, "smgr initialization failed on %s: %m",
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
				elog(FATAL, "smgr shutdown failed on %s: %m",
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
	PendingRelDelete *pending;

	if ((fd = (*(smgrsw[which].smgr_create)) (reln)) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create relation \"%s\": %m",
						RelationGetRelationName(reln))));

	/* Add the relation to the list of stuff to delete at abort */
	pending = (PendingRelDelete *)
		MemoryContextAlloc(TopMemoryContext, sizeof(PendingRelDelete));
	pending->relnode = reln->rd_node;
	pending->which = which;
	pending->isTemp = reln->rd_istemp;
	pending->atCommit = false;	/* delete if abort */
	pending->next = pendingDeletes;
	pendingDeletes = pending;

	return fd;
}

/*
 *	smgrunlink() -- Unlink a relation.
 *
 *		The relation is removed from the store.  Actually, we just remember
 *		that we want to do this at transaction commit.
 */
int
smgrunlink(int16 which, Relation reln)
{
	PendingRelDelete *pending;

	/* Make sure the file is closed */
	if (reln->rd_fd >= 0)
		smgrclose(which, reln);

	/* Add the relation to the list of stuff to delete at commit */
	pending = (PendingRelDelete *)
		MemoryContextAlloc(TopMemoryContext, sizeof(PendingRelDelete));
	pending->relnode = reln->rd_node;
	pending->which = which;
	pending->isTemp = reln->rd_istemp;
	pending->atCommit = true;	/* delete if commit */
	pending->next = pendingDeletes;
	pendingDeletes = pending;

	/*
	 * NOTE: if the relation was created in this transaction, it will now
	 * be present in the pending-delete list twice, once with atCommit
	 * true and once with atCommit false.  Hence, it will be physically
	 * deleted at end of xact in either case (and the other entry will be
	 * ignored by smgrDoPendingDeletes, so no error will occur).  We could
	 * instead remove the existing list entry and delete the physical file
	 * immediately, but for now I'll keep the logic simple.
	 */

	return SM_SUCCESS;
}

/*
 *	smgrextend() -- Add a new block to a file.
 *
 *		The semantics are basically the same as smgrwrite(): write at the
 *		specified position.  However, we are expecting to extend the
 *		relation (ie, blocknum is the current EOF), and so in case of
 *		failure we clean up by truncating.
 *
 *		Returns SM_SUCCESS on success; aborts the current transaction on
 *		failure.
 */
int
smgrextend(int16 which, Relation reln, BlockNumber blocknum, char *buffer)
{
	int			status;

	status = (*(smgrsw[which].smgr_extend)) (reln, blocknum, buffer);

	if (status == SM_FAIL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not extend relation \"%s\": %m",
						RelationGetRelationName(reln)),
				 errhint("Check free disk space.")));

	return status;
}

/*
 *	smgropen() -- Open a relation using a particular storage manager.
 *
 *		Returns the fd for the open relation on success.
 *
 *		On failure, returns -1 if failOK, else aborts the transaction.
 */
int
smgropen(int16 which, Relation reln, bool failOK)
{
	int			fd;

	if (reln->rd_rel->relkind == RELKIND_VIEW)
		return -1;
	if (reln->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		return -1;
	if ((fd = (*(smgrsw[which].smgr_open)) (reln)) < 0)
		if (!failOK)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m",
							RelationGetRelationName(reln))));

	return fd;
}

/*
 *	smgrclose() -- Close a relation.
 *
 *		Returns SM_SUCCESS on success, aborts on failure.
 */
int
smgrclose(int16 which, Relation reln)
{
	if ((*(smgrsw[which].smgr_close)) (reln) == SM_FAIL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close relation \"%s\": %m",
						RelationGetRelationName(reln))));

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
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read block %d of relation \"%s\": %m",
						blocknum, RelationGetRelationName(reln))));

	return status;
}

/*
 *	smgrwrite() -- Write the supplied buffer out.
 *
 *		This is not a synchronous write -- the block is not necessarily
 *		on disk at return, only dumped out to the kernel.
 *
 *		The buffer is written out via the appropriate
 *		storage manager.  This routine returns SM_SUCCESS or aborts
 *		the current transaction.
 */
int
smgrwrite(int16 which, Relation reln, BlockNumber blocknum, char *buffer)
{
	int			status;

	status = (*(smgrsw[which].smgr_write)) (reln, blocknum, buffer);

	if (status == SM_FAIL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write block %d of relation \"%s\": %m",
						blocknum, RelationGetRelationName(reln))));

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
 *		buffer belongs.  Every storage manager must be able to write
 *		this page out to stable storage in this circumstance.
 */
int
smgrblindwrt(int16 which,
			 RelFileNode rnode,
			 BlockNumber blkno,
			 char *buffer)
{
	int			status;

	status = (*(smgrsw[which].smgr_blindwrt)) (rnode, blkno, buffer);

	if (status == SM_FAIL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write block %d of %u/%u blind: %m",
						blkno, rnode.tblNode, rnode.relNode)));

	return status;
}

/*
 *	smgrnblocks() -- Calculate the number of POSTGRES blocks in the
 *					 supplied relation.
 *
 *		Returns the number of blocks on success, aborts the current
 *		transaction on failure.
 */
BlockNumber
smgrnblocks(int16 which, Relation reln)
{
	BlockNumber nblocks;

	nblocks = (*(smgrsw[which].smgr_nblocks)) (reln);

	/*
	 * NOTE: if a relation ever did grow to 2^32-1 blocks, this code would
	 * fail --- but that's a good thing, because it would stop us from
	 * extending the rel another block and having a block whose number
	 * actually is InvalidBlockNumber.
	 */
	if (nblocks == InvalidBlockNumber)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not count blocks of relation \"%s\": %m",
						RelationGetRelationName(reln))));

	return nblocks;
}

/*
 *	smgrtruncate() -- Truncate supplied relation to a specified number
 *						of blocks
 *
 *		Returns the number of blocks on success, aborts the current
 *		transaction on failure.
 */
BlockNumber
smgrtruncate(int16 which, Relation reln, BlockNumber nblocks)
{
	BlockNumber newblks;

	newblks = nblocks;
	if (smgrsw[which].smgr_truncate)
	{
		/*
		 * Tell the free space map to forget anything it may have stored
		 * for the about-to-be-deleted blocks.	We want to be sure it
		 * won't return bogus block numbers later on.
		 */
		FreeSpaceMapTruncateRel(&reln->rd_node, nblocks);

		newblks = (*(smgrsw[which].smgr_truncate)) (reln, nblocks);
		if (newblks == InvalidBlockNumber)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not truncate relation \"%s\" to %u blocks: %m",
							RelationGetRelationName(reln), nblocks)));
	}

	return newblks;
}

/*
 * smgrDoPendingDeletes() -- take care of relation deletes at end of xact.
 */
int
smgrDoPendingDeletes(bool isCommit)
{
	while (pendingDeletes != NULL)
	{
		PendingRelDelete *pending = pendingDeletes;

		pendingDeletes = pending->next;
		if (pending->atCommit == isCommit)
		{
			/*
			 * Get rid of any leftover buffers for the rel (shouldn't be
			 * any in the commit case, but there can be in the abort
			 * case).
			 */
			DropRelFileNodeBuffers(pending->relnode, pending->isTemp);

			/*
			 * Tell the free space map to forget this relation.  It won't
			 * be accessed any more anyway, but we may as well recycle the
			 * map space quickly.
			 */
			FreeSpaceMapForgetRel(&pending->relnode);

			/*
			 * And delete the physical files.
			 *
			 * Note: we treat deletion failure as a WARNING, not an error,
			 * because we've already decided to commit or abort the
			 * current xact.
			 */
			if ((*(smgrsw[pending->which].smgr_unlink)) (pending->relnode) == SM_FAIL)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not unlink %u/%u: %m",
								pending->relnode.tblNode,
								pending->relnode.relNode)));
		}
		pfree(pending);
	}

	return SM_SUCCESS;
}

/*
 *	smgrcommit() -- Prepare to commit changes made during the current
 *					transaction.
 *
 * This is called before we actually commit.
 */
int
smgrcommit(void)
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

/*
 *	smgrabort() -- Abort changes made during the current transaction.
 */
int
smgrabort(void)
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

/*
 * Sync files to disk at checkpoint time.
 */
int
smgrsync(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_sync)
		{
			if ((*(smgrsw[i].smgr_sync)) () == SM_FAIL)
				elog(PANIC, "storage sync failed on %s: %m",
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
		elog(ERROR, "invalid storage manager id: %d", smgrno);

	return smgrwo[smgrno];
}
#endif

void
smgr_redo(XLogRecPtr lsn, XLogRecord *record)
{
}

void
smgr_undo(XLogRecPtr lsn, XLogRecord *record)
{
}

void
smgr_desc(char *buf, uint8 xl_info, char *rec)
{
}
