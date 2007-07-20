/*-------------------------------------------------------------------------
 *
 * smgr.c
 *	  public interface routines to storage manager switch.
 *
 *	  All file system operations in POSTGRES dispatch through these
 *	  routines.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/smgr/smgr.c,v 1.101.2.2 2007/07/20 16:29:59 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "access/xlogutils.h"
#include "commands/tablespace.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/ipc.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"


/*
 * This struct of function pointers defines the API between smgr.c and
 * any individual storage manager module.  Note that smgr subfunctions are
 * generally expected to return TRUE on success, FALSE on error.  (For
 * nblocks and truncate we instead say that returning InvalidBlockNumber
 * indicates an error.)
 */
typedef struct f_smgr
{
	bool		(*smgr_init) (void);	/* may be NULL */
	bool		(*smgr_shutdown) (void);		/* may be NULL */
	bool		(*smgr_close) (SMgrRelation reln);
	bool		(*smgr_create) (SMgrRelation reln, bool isRedo);
	bool		(*smgr_unlink) (RelFileNode rnode, bool isRedo);
	bool		(*smgr_extend) (SMgrRelation reln, BlockNumber blocknum,
											char *buffer, bool isTemp);
	bool		(*smgr_read) (SMgrRelation reln, BlockNumber blocknum,
										  char *buffer);
	bool		(*smgr_write) (SMgrRelation reln, BlockNumber blocknum,
										   char *buffer, bool isTemp);
	BlockNumber (*smgr_nblocks) (SMgrRelation reln);
	BlockNumber (*smgr_truncate) (SMgrRelation reln, BlockNumber nblocks,
											  bool isTemp);
	bool		(*smgr_immedsync) (SMgrRelation reln);
	bool		(*smgr_commit) (void);	/* may be NULL */
	bool		(*smgr_abort) (void);	/* may be NULL */
	bool		(*smgr_sync) (void);	/* may be NULL */
} f_smgr;


static const f_smgr smgrsw[] = {
	/* magnetic disk */
	{mdinit, NULL, mdclose, mdcreate, mdunlink, mdextend,
		mdread, mdwrite, mdnblocks, mdtruncate, mdimmedsync,
		NULL, NULL, mdsync
	}
};

static const int NSmgr = lengthof(smgrsw);


/*
 * Each backend has a hashtable that stores all extant SMgrRelation objects.
 */
static HTAB *SMgrRelationHash = NULL;

/*
 * We keep a list of all relations (represented as RelFileNode values)
 * that have been created or deleted in the current transaction.  When
 * a relation is created, we create the physical file immediately, but
 * remember it so that we can delete the file again if the current
 * transaction is aborted.	Conversely, a deletion request is NOT
 * executed immediately, but is just entered in the list.  When and if
 * the transaction commits, we can delete the physical file.
 *
 * To handle subtransactions, every entry is marked with its transaction
 * nesting level.  At subtransaction commit, we reassign the subtransaction's
 * entries to the parent nesting level.  At subtransaction abort, we can
 * immediately execute the abort-time actions for all entries of the current
 * nesting level.
 *
 * NOTE: the list is kept in TopMemoryContext to be sure it won't disappear
 * unbetimes.  It'd probably be OK to keep it in TopTransactionContext,
 * but I'm being paranoid.
 */

typedef struct PendingRelDelete
{
	RelFileNode relnode;		/* relation that may need to be deleted */
	int			which;			/* which storage manager? */
	bool		isTemp;			/* is it a temporary relation? */
	bool		atCommit;		/* T=delete at commit; F=delete at abort */
	int			nestLevel;		/* xact nesting level of request */
	struct PendingRelDelete *next;		/* linked-list link */
} PendingRelDelete;

static PendingRelDelete *pendingDeletes = NULL; /* head of linked list */


/*
 * Declarations for smgr-related XLOG records
 *
 * Note: we log file creation and truncation here, but logging of deletion
 * actions is handled by xact.c, because it is part of transaction commit.
 */

/* XLOG gives us high 4 bits */
#define XLOG_SMGR_CREATE	0x10
#define XLOG_SMGR_TRUNCATE	0x20

typedef struct xl_smgr_create
{
	RelFileNode rnode;
} xl_smgr_create;

typedef struct xl_smgr_truncate
{
	BlockNumber blkno;
	RelFileNode rnode;
} xl_smgr_truncate;


/* local function prototypes */
static void smgrshutdown(int code, Datum arg);
static void smgr_internal_unlink(RelFileNode rnode, int which,
					 bool isTemp, bool isRedo);


/*
 *	smgrinit(), smgrshutdown() -- Initialize or shut down storage
 *								  managers.
 *
 * Note: smgrinit is called during backend startup (normal or standalone
 * case), *not* during postmaster start.  Therefore, any resources created
 * here or destroyed in smgrshutdown are backend-local.
 */
void
smgrinit(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_init)
		{
			if (!(*(smgrsw[i].smgr_init)) ())
				elog(FATAL, "smgr initialization failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}

	/* register the shutdown proc */
	on_proc_exit(smgrshutdown, 0);
}

/*
 * on_proc_exit hook for smgr cleanup during backend shutdown
 */
static void
smgrshutdown(int code, Datum arg)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_shutdown)
		{
			if (!(*(smgrsw[i].smgr_shutdown)) ())
				elog(FATAL, "smgr shutdown failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}
}

/*
 *	smgropen() -- Return an SMgrRelation object, creating it if need be.
 *
 *		This does not attempt to actually open the object.
 */
SMgrRelation
smgropen(RelFileNode rnode)
{
	SMgrRelation reln;
	bool		found;

	if (SMgrRelationHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(RelFileNode);
		ctl.entrysize = sizeof(SMgrRelationData);
		ctl.hash = tag_hash;
		SMgrRelationHash = hash_create("smgr relation table", 400,
									   &ctl, HASH_ELEM | HASH_FUNCTION);
	}

	/* Look up or create an entry */
	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  (void *) &rnode,
									  HASH_ENTER, &found);

	/* Initialize it if not present before */
	if (!found)
	{
		/* hash_search already filled in the lookup key */
		reln->smgr_owner = NULL;
		reln->smgr_which = 0;	/* we only have md.c at present */
		reln->md_fd = NULL;		/* mark it not open */
	}

	return reln;
}

/*
 * smgrsetowner() -- Establish a long-lived reference to an SMgrRelation object
 *
 * There can be only one owner at a time; this is sufficient since currently
 * the only such owners exist in the relcache.
 */
void
smgrsetowner(SMgrRelation *owner, SMgrRelation reln)
{
	/*
	 * First, unhook any old owner.  (Normally there shouldn't be any, but it
	 * seems possible that this can happen during swap_relation_files()
	 * depending on the order of processing.  It's ok to close the old
	 * relcache entry early in that case.)
	 */
	if (reln->smgr_owner)
		*(reln->smgr_owner) = NULL;

	/* Now establish the ownership relationship. */
	reln->smgr_owner = owner;
	*owner = reln;
}

/*
 *	smgrclose() -- Close and delete an SMgrRelation object.
 */
void
smgrclose(SMgrRelation reln)
{
	SMgrRelation *owner;

	if (!(*(smgrsw[reln->smgr_which].smgr_close)) (reln))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close relation %u/%u/%u: %m",
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode)));

	owner = reln->smgr_owner;

	if (hash_search(SMgrRelationHash,
					(void *) &(reln->smgr_rnode),
					HASH_REMOVE, NULL) == NULL)
		elog(ERROR, "SMgrRelation hashtable corrupted");

	/*
	 * Unhook the owner pointer, if any.  We do this last since in the remote
	 * possibility of failure above, the SMgrRelation object will still exist.
	 */
	if (owner)
		*owner = NULL;
}

/*
 *	smgrcloseall() -- Close all existing SMgrRelation objects.
 */
void
smgrcloseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
		smgrclose(reln);
}

/*
 *	smgrclosenode() -- Close SMgrRelation object for given RelFileNode,
 *					   if one exists.
 *
 * This has the same effects as smgrclose(smgropen(rnode)), but it avoids
 * uselessly creating a hashtable entry only to drop it again when no
 * such entry exists already.
 */
void
smgrclosenode(RelFileNode rnode)
{
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  (void *) &rnode,
									  HASH_FIND, NULL);
	if (reln != NULL)
		smgrclose(reln);
}

/*
 *	smgrcreate() -- Create a new relation.
 *
 *		Given an already-created (but presumably unused) SMgrRelation,
 *		cause the underlying disk file or other storage to be created.
 *
 *		If isRedo is true, it is okay for the underlying file to exist
 *		already because we are in a WAL replay sequence.  In this case
 *		we should make no PendingRelDelete entry; the WAL sequence will
 *		tell whether to drop the file.
 */
void
smgrcreate(SMgrRelation reln, bool isTemp, bool isRedo)
{
	XLogRecPtr	lsn;
	XLogRecData rdata;
	xl_smgr_create xlrec;
	PendingRelDelete *pending;

	/*
	 * We may be using the target table space for the first time in this
	 * database, so create a per-database subdirectory if needed.
	 *
	 * XXX this is a fairly ugly violation of module layering, but this seems
	 * to be the best place to put the check.  Maybe TablespaceCreateDbspace
	 * should be here and not in commands/tablespace.c?  But that would imply
	 * importing a lot of stuff that smgr.c oughtn't know, either.
	 */
	TablespaceCreateDbspace(reln->smgr_rnode.spcNode,
							reln->smgr_rnode.dbNode,
							isRedo);

	if (!(*(smgrsw[reln->smgr_which].smgr_create)) (reln, isRedo))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create relation %u/%u/%u: %m",
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode)));

	if (isRedo)
		return;

	/*
	 * Make a non-transactional XLOG entry showing the file creation. It's
	 * non-transactional because we should replay it whether the transaction
	 * commits or not; if not, the file will be dropped at abort time.
	 */
	xlrec.rnode = reln->smgr_rnode;

	rdata.data = (char *) &xlrec;
	rdata.len = sizeof(xlrec);
	rdata.buffer = InvalidBuffer;
	rdata.next = NULL;

	lsn = XLogInsert(RM_SMGR_ID, XLOG_SMGR_CREATE | XLOG_NO_TRAN, &rdata);

	/* Add the relation to the list of stuff to delete at abort */
	pending = (PendingRelDelete *)
		MemoryContextAlloc(TopMemoryContext, sizeof(PendingRelDelete));
	pending->relnode = reln->smgr_rnode;
	pending->which = reln->smgr_which;
	pending->isTemp = isTemp;
	pending->atCommit = false;	/* delete if abort */
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->next = pendingDeletes;
	pendingDeletes = pending;
}

/*
 *	smgrscheduleunlink() -- Schedule unlinking a relation at xact commit.
 *
 *		The relation is marked to be removed from the store if we
 *		successfully commit the current transaction.
 *
 * This also implies smgrclose() on the SMgrRelation object.
 */
void
smgrscheduleunlink(SMgrRelation reln, bool isTemp)
{
	PendingRelDelete *pending;

	/* Add the relation to the list of stuff to delete at commit */
	pending = (PendingRelDelete *)
		MemoryContextAlloc(TopMemoryContext, sizeof(PendingRelDelete));
	pending->relnode = reln->smgr_rnode;
	pending->which = reln->smgr_which;
	pending->isTemp = isTemp;
	pending->atCommit = true;	/* delete if commit */
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->next = pendingDeletes;
	pendingDeletes = pending;

	/*
	 * NOTE: if the relation was created in this transaction, it will now be
	 * present in the pending-delete list twice, once with atCommit true and
	 * once with atCommit false.  Hence, it will be physically deleted at end
	 * of xact in either case (and the other entry will be ignored by
	 * smgrDoPendingDeletes, so no error will occur).  We could instead remove
	 * the existing list entry and delete the physical file immediately, but
	 * for now I'll keep the logic simple.
	 */

	/* Now close the file and throw away the hashtable entry */
	smgrclose(reln);
}

/*
 *	smgrdounlink() -- Immediately unlink a relation.
 *
 *		The relation is removed from the store.  This should not be used
 *		during transactional operations, since it can't be undone.
 *
 *		If isRedo is true, it is okay for the underlying file to be gone
 *		already.
 *
 * This also implies smgrclose() on the SMgrRelation object.
 */
void
smgrdounlink(SMgrRelation reln, bool isTemp, bool isRedo)
{
	RelFileNode rnode = reln->smgr_rnode;
	int			which = reln->smgr_which;

	/* Close the file and throw away the hashtable entry */
	smgrclose(reln);

	smgr_internal_unlink(rnode, which, isTemp, isRedo);
}

/*
 * Shared subroutine that actually does the unlink ...
 */
static void
smgr_internal_unlink(RelFileNode rnode, int which, bool isTemp, bool isRedo)
{
	/*
	 * Get rid of any remaining buffers for the relation.  bufmgr will just
	 * drop them without bothering to write the contents.
	 */
	DropRelFileNodeBuffers(rnode, isTemp, 0);

	/*
	 * Tell the free space map to forget this relation.  It won't be accessed
	 * any more anyway, but we may as well recycle the map space quickly.
	 */
	FreeSpaceMapForgetRel(&rnode);

	/*
	 * It'd be nice to tell the stats collector to forget it immediately, too.
	 * But we can't because we don't know the OID (and in cases involving
	 * relfilenode swaps, it's not always clear which table OID to forget,
	 * anyway).
	 */

	/*
	 * And delete the physical files.
	 *
	 * Note: we treat deletion failure as a WARNING, not an error, because
	 * we've already decided to commit or abort the current xact.
	 */
	if (!(*(smgrsw[which].smgr_unlink)) (rnode, isRedo))
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove relation %u/%u/%u: %m",
						rnode.spcNode,
						rnode.dbNode,
						rnode.relNode)));
}

/*
 *	smgrextend() -- Add a new block to a file.
 *
 *		The semantics are basically the same as smgrwrite(): write at the
 *		specified position.  However, we are expecting to extend the
 *		relation (ie, blocknum is the current EOF), and so in case of
 *		failure we clean up by truncating.
 */
void
smgrextend(SMgrRelation reln, BlockNumber blocknum, char *buffer, bool isTemp)
{
	if (!(*(smgrsw[reln->smgr_which].smgr_extend)) (reln, blocknum, buffer,
													isTemp))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not extend relation %u/%u/%u: %m",
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode),
				 errhint("Check free disk space.")));
}

/*
 *	smgrread() -- read a particular block from a relation into the supplied
 *				  buffer.
 *
 *		This routine is called from the buffer manager in order to
 *		instantiate pages in the shared buffer cache.  All storage managers
 *		return pages in the format that POSTGRES expects.
 */
void
smgrread(SMgrRelation reln, BlockNumber blocknum, char *buffer)
{
	if (!(*(smgrsw[reln->smgr_which].smgr_read)) (reln, blocknum, buffer))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read block %u of relation %u/%u/%u: %m",
						blocknum,
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode)));
}

/*
 *	smgrwrite() -- Write the supplied buffer out.
 *
 *		This is not a synchronous write -- the block is not necessarily
 *		on disk at return, only dumped out to the kernel.  However,
 *		provisions will be made to fsync the write before the next checkpoint.
 *
 *		isTemp indicates that the relation is a temp table (ie, is managed
 *		by the local-buffer manager).  In this case no provisions need be
 *		made to fsync the write before checkpointing.
 */
void
smgrwrite(SMgrRelation reln, BlockNumber blocknum, char *buffer, bool isTemp)
{
	if (!(*(smgrsw[reln->smgr_which].smgr_write)) (reln, blocknum, buffer,
												   isTemp))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write block %u of relation %u/%u/%u: %m",
						blocknum,
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode)));
}

/*
 *	smgrnblocks() -- Calculate the number of blocks in the
 *					 supplied relation.
 *
 *		Returns the number of blocks on success, aborts the current
 *		transaction on failure.
 */
BlockNumber
smgrnblocks(SMgrRelation reln)
{
	BlockNumber nblocks;

	nblocks = (*(smgrsw[reln->smgr_which].smgr_nblocks)) (reln);

	/*
	 * NOTE: if a relation ever did grow to 2^32-1 blocks, this code would
	 * fail --- but that's a good thing, because it would stop us from
	 * extending the rel another block and having a block whose number
	 * actually is InvalidBlockNumber.
	 */
	if (nblocks == InvalidBlockNumber)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not count blocks of relation %u/%u/%u: %m",
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode)));

	return nblocks;
}

/*
 *	smgrtruncate() -- Truncate supplied relation to the specified number
 *					  of blocks
 *
 *		Returns the number of blocks on success, aborts the current
 *		transaction on failure.
 */
BlockNumber
smgrtruncate(SMgrRelation reln, BlockNumber nblocks, bool isTemp)
{
	BlockNumber newblks;

	/*
	 * Get rid of any buffers for the about-to-be-deleted blocks. bufmgr will
	 * just drop them without bothering to write the contents.
	 */
	DropRelFileNodeBuffers(reln->smgr_rnode, isTemp, nblocks);

	/*
	 * Tell the free space map to forget anything it may have stored for the
	 * about-to-be-deleted blocks.	We want to be sure it won't return bogus
	 * block numbers later on.
	 */
	FreeSpaceMapTruncateRel(&reln->smgr_rnode, nblocks);

	/* Do the truncation */
	newblks = (*(smgrsw[reln->smgr_which].smgr_truncate)) (reln, nblocks,
														   isTemp);
	if (newblks == InvalidBlockNumber)
		ereport(ERROR,
				(errcode_for_file_access(),
			  errmsg("could not truncate relation %u/%u/%u to %u blocks: %m",
					 reln->smgr_rnode.spcNode,
					 reln->smgr_rnode.dbNode,
					 reln->smgr_rnode.relNode,
					 nblocks)));

	if (!isTemp)
	{
		/*
		 * Make a non-transactional XLOG entry showing the file truncation.
		 * It's non-transactional because we should replay it whether the
		 * transaction commits or not; the underlying file change is certainly
		 * not reversible.
		 */
		XLogRecPtr	lsn;
		XLogRecData rdata;
		xl_smgr_truncate xlrec;

		xlrec.blkno = newblks;
		xlrec.rnode = reln->smgr_rnode;

		rdata.data = (char *) &xlrec;
		rdata.len = sizeof(xlrec);
		rdata.buffer = InvalidBuffer;
		rdata.next = NULL;

		lsn = XLogInsert(RM_SMGR_ID, XLOG_SMGR_TRUNCATE | XLOG_NO_TRAN,
						 &rdata);
	}

	return newblks;
}

/*
 *	smgrimmedsync() -- Force the specified relation to stable storage.
 *
 *		Synchronously force all previous writes to the specified relation
 *		down to disk.
 *
 *		This is useful for building completely new relations (eg, new
 *		indexes).  Instead of incrementally WAL-logging the index build
 *		steps, we can just write completed index pages to disk with smgrwrite
 *		or smgrextend, and then fsync the completed index file before
 *		committing the transaction.  (This is sufficient for purposes of
 *		crash recovery, since it effectively duplicates forcing a checkpoint
 *		for the completed index.  But it is *not* sufficient if one wishes
 *		to use the WAL log for PITR or replication purposes: in that case
 *		we have to make WAL entries as well.)
 *
 *		The preceding writes should specify isTemp = true to avoid
 *		duplicative fsyncs.
 *
 *		Note that you need to do FlushRelationBuffers() first if there is
 *		any possibility that there are dirty buffers for the relation;
 *		otherwise the sync is not very meaningful.
 */
void
smgrimmedsync(SMgrRelation reln)
{
	if (!(*(smgrsw[reln->smgr_which].smgr_immedsync)) (reln))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not sync relation %u/%u/%u: %m",
						reln->smgr_rnode.spcNode,
						reln->smgr_rnode.dbNode,
						reln->smgr_rnode.relNode)));
}


/*
 *	PostPrepare_smgr -- Clean up after a successful PREPARE
 *
 * What we have to do here is throw away the in-memory state about pending
 * relation deletes.  It's all been recorded in the 2PC state file and
 * it's no longer smgr's job to worry about it.
 */
void
PostPrepare_smgr(void)
{
	PendingRelDelete *pending;
	PendingRelDelete *next;

	for (pending = pendingDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		pendingDeletes = next;
		/* must explicitly free the list entry */
		pfree(pending);
	}
}


/*
 *	smgrDoPendingDeletes() -- Take care of relation deletes at end of xact.
 *
 * This also runs when aborting a subxact; we want to clean up a failed
 * subxact immediately.
 */
void
smgrDoPendingDeletes(bool isCommit)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingRelDelete *pending;
	PendingRelDelete *prev;
	PendingRelDelete *next;

	prev = NULL;
	for (pending = pendingDeletes; pending != NULL; pending = next)
	{
		next = pending->next;
		if (pending->nestLevel < nestLevel)
		{
			/* outer-level entries should not be processed yet */
			prev = pending;
		}
		else
		{
			/* unlink list entry first, so we don't retry on failure */
			if (prev)
				prev->next = next;
			else
				pendingDeletes = next;
			/* do deletion if called for */
			if (pending->atCommit == isCommit)
				smgr_internal_unlink(pending->relnode,
									 pending->which,
									 pending->isTemp,
									 false);
			/* must explicitly free the list entry */
			pfree(pending);
			/* prev does not change */
		}
	}
}

/*
 * smgrGetPendingDeletes() -- Get a list of relations to be deleted.
 *
 * The return value is the number of relations scheduled for termination.
 * *ptr is set to point to a freshly-palloc'd array of RelFileNodes.
 * If there are no relations to be deleted, *ptr is set to NULL.
 *
 * Note that the list does not include anything scheduled for termination
 * by upper-level transactions.
 */
int
smgrGetPendingDeletes(bool forCommit, RelFileNode **ptr)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	int			nrels;
	RelFileNode *rptr;
	PendingRelDelete *pending;

	nrels = 0;
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel && pending->atCommit == forCommit)
			nrels++;
	}
	if (nrels == 0)
	{
		*ptr = NULL;
		return 0;
	}
	rptr = (RelFileNode *) palloc(nrels * sizeof(RelFileNode));
	*ptr = rptr;
	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel && pending->atCommit == forCommit)
			*rptr++ = pending->relnode;
	}
	return nrels;
}

/*
 * AtSubCommit_smgr() --- Take care of subtransaction commit.
 *
 * Reassign all items in the pending-deletes list to the parent transaction.
 */
void
AtSubCommit_smgr(void)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingRelDelete *pending;

	for (pending = pendingDeletes; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel)
			pending->nestLevel = nestLevel - 1;
	}
}

/*
 * AtSubAbort_smgr() --- Take care of subtransaction abort.
 *
 * Delete created relations and forget about deleted relations.
 * We can execute these operations immediately because we know this
 * subtransaction will not commit.
 */
void
AtSubAbort_smgr(void)
{
	smgrDoPendingDeletes(false);
}

/*
 *	smgrcommit() -- Prepare to commit changes made during the current
 *					transaction.
 *
 *		This is called before we actually commit.
 */
void
smgrcommit(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_commit)
		{
			if (!(*(smgrsw[i].smgr_commit)) ())
				elog(ERROR, "transaction commit failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}
}

/*
 *	smgrabort() -- Clean up after transaction abort.
 */
void
smgrabort(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_abort)
		{
			if (!(*(smgrsw[i].smgr_abort)) ())
				elog(ERROR, "transaction abort failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}
}

/*
 *	smgrsync() -- Sync files to disk at checkpoint time.
 */
void
smgrsync(void)
{
	int			i;

	for (i = 0; i < NSmgr; i++)
	{
		if (smgrsw[i].smgr_sync)
		{
			if (!(*(smgrsw[i].smgr_sync)) ())
				elog(ERROR, "storage sync failed on %s: %m",
					 DatumGetCString(DirectFunctionCall1(smgrout,
														 Int16GetDatum(i))));
		}
	}
}


void
smgr_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_SMGR_CREATE)
	{
		xl_smgr_create *xlrec = (xl_smgr_create *) XLogRecGetData(record);
		SMgrRelation reln;

		reln = smgropen(xlrec->rnode);
		smgrcreate(reln, false, true);
	}
	else if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec = (xl_smgr_truncate *) XLogRecGetData(record);
		SMgrRelation reln;
		BlockNumber newblks;

		reln = smgropen(xlrec->rnode);

		/*
		 * Forcibly create relation if it doesn't exist (which suggests that
		 * it was dropped somewhere later in the WAL sequence).  As in
		 * XLogOpenRelation, we prefer to recreate the rel and replay the
		 * log as best we can until the drop is seen.
		 */
		smgrcreate(reln, false, true);

		/* Can't use smgrtruncate because it would try to xlog */

		/*
		 * First, force bufmgr to drop any buffers it has for the to-be-
		 * truncated blocks.  We must do this, else subsequent XLogReadBuffer
		 * operations will not re-extend the file properly.
		 */
		DropRelFileNodeBuffers(xlrec->rnode, false, xlrec->blkno);

		/*
		 * Tell the free space map to forget anything it may have stored for
		 * the about-to-be-deleted blocks.	We want to be sure it won't return
		 * bogus block numbers later on.
		 */
		FreeSpaceMapTruncateRel(&reln->smgr_rnode, xlrec->blkno);

		/* Do the truncation */
		newblks = (*(smgrsw[reln->smgr_which].smgr_truncate)) (reln,
															   xlrec->blkno,
															   false);
		if (newblks == InvalidBlockNumber)
			ereport(WARNING,
					(errcode_for_file_access(),
			  errmsg("could not truncate relation %u/%u/%u to %u blocks: %m",
					 reln->smgr_rnode.spcNode,
					 reln->smgr_rnode.dbNode,
					 reln->smgr_rnode.relNode,
					 xlrec->blkno)));

		/* Also tell xlogutils.c about it */
		XLogTruncateRelation(xlrec->rnode, xlrec->blkno);
	}
	else
		elog(PANIC, "smgr_redo: unknown op code %u", info);
}

void
smgr_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_SMGR_CREATE)
	{
		xl_smgr_create *xlrec = (xl_smgr_create *) rec;

		appendStringInfo(buf, "file create: %u/%u/%u",
						 xlrec->rnode.spcNode, xlrec->rnode.dbNode,
						 xlrec->rnode.relNode);
	}
	else if (info == XLOG_SMGR_TRUNCATE)
	{
		xl_smgr_truncate *xlrec = (xl_smgr_truncate *) rec;

		appendStringInfo(buf, "file truncate: %u/%u/%u to %u blocks",
						 xlrec->rnode.spcNode, xlrec->rnode.dbNode,
						 xlrec->rnode.relNode, xlrec->blkno);
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}
