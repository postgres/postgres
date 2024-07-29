/*-------------------------------------------------------------------------
 *
 * smgr.c
 *	  public interface routines to storage manager switch.
 *
 * All file system operations on relations dispatch through these routines.
 * An SMgrRelation represents physical on-disk relation files that are open
 * for reading and writing.
 *
 * When a relation is first accessed through the relation cache, the
 * corresponding SMgrRelation entry is opened by calling smgropen(), and the
 * reference is stored in the relation cache entry.
 *
 * Accesses that don't go through the relation cache open the SMgrRelation
 * directly.  That includes flushing buffers from the buffer cache, as well as
 * all accesses in auxiliary processes like the checkpointer or the WAL redo
 * in the startup process.
 *
 * Operations like CREATE, DROP, ALTER TABLE also hold SMgrRelation references
 * independent of the relation cache.  They need to prepare the physical files
 * before updating the relation cache.
 *
 * There is a hash table that holds all the SMgrRelation entries in the
 * backend.  If you call smgropen() twice for the same rel locator, you get a
 * reference to the same SMgrRelation. The reference is valid until the end of
 * transaction.  This makes repeated access to the same relation efficient,
 * and allows caching things like the relation size in the SMgrRelation entry.
 *
 * At end of transaction, all SMgrRelation entries that haven't been pinned
 * are removed.  An SMgrRelation can hold kernel file system descriptors for
 * the underlying files, and we'd like to close those reasonably soon if the
 * file gets deleted.  The SMgrRelations references held by the relcache are
 * pinned to prevent them from being closed.
 *
 * There is another mechanism to close file descriptors early:
 * PROCSIGNAL_BARRIER_SMGRRELEASE.  It is a request to immediately close all
 * file descriptors.  Upon receiving that signal, the backend closes all file
 * descriptors held open by SMgrRelations, but because it can happen in the
 * middle of a transaction, we cannot destroy the SMgrRelation objects
 * themselves, as there could pointers to them in active use.  See
 * smgrrelease() and smgrreleaseall().
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/smgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlogutils.h"
#include "lib/ilist.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/inval.h"


/*
 * This struct of function pointers defines the API between smgr.c and
 * any individual storage manager module.  Note that smgr subfunctions are
 * generally expected to report problems via elog(ERROR).  An exception is
 * that smgr_unlink should use elog(WARNING), rather than erroring out,
 * because we normally unlink relations during post-commit/abort cleanup,
 * and so it's too late to raise an error.  Also, various conditions that
 * would normally be errors should be allowed during bootstrap and/or WAL
 * recovery --- see comments in md.c for details.
 */
typedef struct f_smgr
{
	void		(*smgr_init) (void);	/* may be NULL */
	void		(*smgr_shutdown) (void);	/* may be NULL */
	void		(*smgr_open) (SMgrRelation reln);
	void		(*smgr_close) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_create) (SMgrRelation reln, ForkNumber forknum,
								bool isRedo);
	bool		(*smgr_exists) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_unlink) (RelFileLocatorBackend rlocator, ForkNumber forknum,
								bool isRedo);
	void		(*smgr_extend) (SMgrRelation reln, ForkNumber forknum,
								BlockNumber blocknum, const void *buffer, bool skipFsync);
	void		(*smgr_zeroextend) (SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum, int nblocks, bool skipFsync);
	bool		(*smgr_prefetch) (SMgrRelation reln, ForkNumber forknum,
								  BlockNumber blocknum, int nblocks);
	void		(*smgr_readv) (SMgrRelation reln, ForkNumber forknum,
							   BlockNumber blocknum,
							   void **buffers, BlockNumber nblocks);
	void		(*smgr_writev) (SMgrRelation reln, ForkNumber forknum,
								BlockNumber blocknum,
								const void **buffers, BlockNumber nblocks,
								bool skipFsync);
	void		(*smgr_writeback) (SMgrRelation reln, ForkNumber forknum,
								   BlockNumber blocknum, BlockNumber nblocks);
	BlockNumber (*smgr_nblocks) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_truncate) (SMgrRelation reln, ForkNumber forknum,
								  BlockNumber nblocks);
	void		(*smgr_immedsync) (SMgrRelation reln, ForkNumber forknum);
	void		(*smgr_registersync) (SMgrRelation reln, ForkNumber forknum);
} f_smgr;

static const f_smgr smgrsw[] = {
	/* magnetic disk */
	{
		.smgr_init = mdinit,
		.smgr_shutdown = NULL,
		.smgr_open = mdopen,
		.smgr_close = mdclose,
		.smgr_create = mdcreate,
		.smgr_exists = mdexists,
		.smgr_unlink = mdunlink,
		.smgr_extend = mdextend,
		.smgr_zeroextend = mdzeroextend,
		.smgr_prefetch = mdprefetch,
		.smgr_readv = mdreadv,
		.smgr_writev = mdwritev,
		.smgr_writeback = mdwriteback,
		.smgr_nblocks = mdnblocks,
		.smgr_truncate = mdtruncate,
		.smgr_immedsync = mdimmedsync,
		.smgr_registersync = mdregistersync,
	}
};

static const int NSmgr = lengthof(smgrsw);

/*
 * Each backend has a hashtable that stores all extant SMgrRelation objects.
 * In addition, "unpinned" SMgrRelation objects are chained together in a list.
 */
static HTAB *SMgrRelationHash = NULL;

static dlist_head unpinned_relns;

/* local function prototypes */
static void smgrshutdown(int code, Datum arg);
static void smgrdestroy(SMgrRelation reln);


/*
 * smgrinit(), smgrshutdown() -- Initialize or shut down storage
 *								 managers.
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
			smgrsw[i].smgr_init();
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
			smgrsw[i].smgr_shutdown();
	}
}

/*
 * smgropen() -- Return an SMgrRelation object, creating it if need be.
 *
 * In versions of PostgreSQL prior to 17, this function returned an object
 * with no defined lifetime.  Now, however, the object remains valid for the
 * lifetime of the transaction, up to the point where AtEOXact_SMgr() is
 * called, making it much easier for callers to know for how long they can
 * hold on to a pointer to the returned object.  If this function is called
 * outside of a transaction, the object remains valid until smgrdestroy() or
 * smgrdestroyall() is called.  Background processes that use smgr but not
 * transactions typically do this once per checkpoint cycle.
 *
 * This does not attempt to actually open the underlying files.
 */
SMgrRelation
smgropen(RelFileLocator rlocator, ProcNumber backend)
{
	RelFileLocatorBackend brlocator;
	SMgrRelation reln;
	bool		found;

	Assert(RelFileNumberIsValid(rlocator.relNumber));

	if (SMgrRelationHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileLocatorBackend);
		ctl.entrysize = sizeof(SMgrRelationData);
		SMgrRelationHash = hash_create("smgr relation table", 400,
									   &ctl, HASH_ELEM | HASH_BLOBS);
		dlist_init(&unpinned_relns);
	}

	/* Look up or create an entry */
	brlocator.locator = rlocator;
	brlocator.backend = backend;
	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  &brlocator,
									  HASH_ENTER, &found);

	/* Initialize it if not present before */
	if (!found)
	{
		/* hash_search already filled in the lookup key */
		reln->smgr_targblock = InvalidBlockNumber;
		for (int i = 0; i <= MAX_FORKNUM; ++i)
			reln->smgr_cached_nblocks[i] = InvalidBlockNumber;
		reln->smgr_which = 0;	/* we only have md.c at present */

		/* implementation-specific initialization */
		smgrsw[reln->smgr_which].smgr_open(reln);

		/* it is not pinned yet */
		reln->pincount = 0;
		dlist_push_tail(&unpinned_relns, &reln->node);
	}

	return reln;
}

/*
 * smgrpin() -- Prevent an SMgrRelation object from being destroyed at end of
 *				transaction
 */
void
smgrpin(SMgrRelation reln)
{
	if (reln->pincount == 0)
		dlist_delete(&reln->node);
	reln->pincount++;
}

/*
 * smgrunpin() -- Allow an SMgrRelation object to be destroyed at end of
 *				  transaction
 *
 * The object remains valid, but if there are no other pins on it, it is moved
 * to the unpinned list where it will be destroyed by AtEOXact_SMgr().
 */
void
smgrunpin(SMgrRelation reln)
{
	Assert(reln->pincount > 0);
	reln->pincount--;
	if (reln->pincount == 0)
		dlist_push_tail(&unpinned_relns, &reln->node);
}

/*
 * smgrdestroy() -- Delete an SMgrRelation object.
 */
static void
smgrdestroy(SMgrRelation reln)
{
	ForkNumber	forknum;

	Assert(reln->pincount == 0);

	for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		smgrsw[reln->smgr_which].smgr_close(reln, forknum);

	dlist_delete(&reln->node);

	if (hash_search(SMgrRelationHash,
					&(reln->smgr_rlocator),
					HASH_REMOVE, NULL) == NULL)
		elog(ERROR, "SMgrRelation hashtable corrupted");
}

/*
 * smgrrelease() -- Release all resources used by this object.
 *
 * The object remains valid.
 */
void
smgrrelease(SMgrRelation reln)
{
	for (ForkNumber forknum = 0; forknum <= MAX_FORKNUM; forknum++)
	{
		smgrsw[reln->smgr_which].smgr_close(reln, forknum);
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
	}
	reln->smgr_targblock = InvalidBlockNumber;
}

/*
 * smgrclose() -- Close an SMgrRelation object.
 *
 * The SMgrRelation reference should not be used after this call.  However,
 * because we don't keep track of the references returned by smgropen(), we
 * don't know if there are other references still pointing to the same object,
 * so we cannot remove the SMgrRelation object yet.  Therefore, this is just a
 * synonym for smgrrelease() at the moment.
 */
void
smgrclose(SMgrRelation reln)
{
	smgrrelease(reln);
}

/*
 * smgrdestroyall() -- Release resources used by all unpinned objects.
 *
 * It must be known that there are no pointers to SMgrRelations, other than
 * those pinned with smgrpin().
 */
void
smgrdestroyall(void)
{
	dlist_mutable_iter iter;

	/*
	 * Zap all unpinned SMgrRelations.  We rely on smgrdestroy() to remove
	 * each one from the list.
	 */
	dlist_foreach_modify(iter, &unpinned_relns)
	{
		SMgrRelation rel = dlist_container(SMgrRelationData, node,
										   iter.cur);

		smgrdestroy(rel);
	}
}

/*
 * smgrreleaseall() -- Release resources used by all objects.
 */
void
smgrreleaseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
	{
		smgrrelease(reln);
	}
}

/*
 * smgrreleaserellocator() -- Release resources for given RelFileLocator, if
 *							  it's open.
 *
 * This has the same effects as smgrrelease(smgropen(rlocator)), but avoids
 * uselessly creating a hashtable entry only to drop it again when no
 * such entry exists already.
 */
void
smgrreleaserellocator(RelFileLocatorBackend rlocator)
{
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  &rlocator,
									  HASH_FIND, NULL);
	if (reln != NULL)
		smgrrelease(reln);
}

/*
 * smgrexists() -- Does the underlying file for a fork exist?
 */
bool
smgrexists(SMgrRelation reln, ForkNumber forknum)
{
	return smgrsw[reln->smgr_which].smgr_exists(reln, forknum);
}

/*
 * smgrcreate() -- Create a new relation.
 *
 * Given an already-created (but presumably unused) SMgrRelation,
 * cause the underlying disk file or other storage for the fork
 * to be created.
 */
void
smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	smgrsw[reln->smgr_which].smgr_create(reln, forknum, isRedo);
}

/*
 * smgrdosyncall() -- Immediately sync all forks of all given relations
 *
 * All forks of all given relations are synced out to the store.
 *
 * This is equivalent to FlushRelationBuffers() for each smgr relation,
 * then calling smgrimmedsync() for all forks of each relation, but it's
 * significantly quicker so should be preferred when possible.
 */
void
smgrdosyncall(SMgrRelation *rels, int nrels)
{
	int			i = 0;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	FlushRelationsAllBuffers(rels, nrels);

	/*
	 * Sync the physical file(s).
	 */
	for (i = 0; i < nrels; i++)
	{
		int			which = rels[i]->smgr_which;

		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		{
			if (smgrsw[which].smgr_exists(rels[i], forknum))
				smgrsw[which].smgr_immedsync(rels[i], forknum);
		}
	}
}

/*
 * smgrdounlinkall() -- Immediately unlink all forks of all given relations
 *
 * All forks of all given relations are removed from the store.  This
 * should not be used during transactional operations, since it can't be
 * undone.
 *
 * If isRedo is true, it is okay for the underlying file(s) to be gone
 * already.
 */
void
smgrdounlinkall(SMgrRelation *rels, int nrels, bool isRedo)
{
	int			i = 0;
	RelFileLocatorBackend *rlocators;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	/*
	 * Get rid of any remaining buffers for the relations.  bufmgr will just
	 * drop them without bothering to write the contents.
	 */
	DropRelationsAllBuffers(rels, nrels);

	/*
	 * create an array which contains all relations to be dropped, and close
	 * each relation's forks at the smgr level while at it
	 */
	rlocators = palloc(sizeof(RelFileLocatorBackend) * nrels);
	for (i = 0; i < nrels; i++)
	{
		RelFileLocatorBackend rlocator = rels[i]->smgr_rlocator;
		int			which = rels[i]->smgr_which;

		rlocators[i] = rlocator;

		/* Close the forks at smgr level */
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			smgrsw[which].smgr_close(rels[i], forknum);
	}

	/*
	 * Send a shared-inval message to force other backends to close any
	 * dangling smgr references they may have for these rels.  We should do
	 * this before starting the actual unlinking, in case we fail partway
	 * through that step.  Note that the sinval messages will eventually come
	 * back to this backend, too, and thereby provide a backstop that we
	 * closed our own smgr rel.
	 */
	for (i = 0; i < nrels; i++)
		CacheInvalidateSmgr(rlocators[i]);

	/*
	 * Delete the physical file(s).
	 *
	 * Note: smgr_unlink must treat deletion failure as a WARNING, not an
	 * ERROR, because we've already decided to commit or abort the current
	 * xact.
	 */

	for (i = 0; i < nrels; i++)
	{
		int			which = rels[i]->smgr_which;

		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			smgrsw[which].smgr_unlink(rlocators[i], forknum, isRedo);
	}

	pfree(rlocators);
}


/*
 * smgrextend() -- Add a new block to a file.
 *
 * The semantics are nearly the same as smgrwrite(): write at the
 * specified position.  However, this is to be used for the case of
 * extending a relation (i.e., blocknum is at or beyond the current
 * EOF).  Note that we assume writing a block beyond current EOF
 * causes intervening file space to become filled with zeroes.
 */
void
smgrextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   const void *buffer, bool skipFsync)
{
	smgrsw[reln->smgr_which].smgr_extend(reln, forknum, blocknum,
										 buffer, skipFsync);

	/*
	 * Normally we expect this to increase nblocks by one, but if the cached
	 * value isn't as expected, just invalidate it so the next call asks the
	 * kernel.
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + 1;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
}

/*
 * smgrzeroextend() -- Add new zeroed out blocks to a file.
 *
 * Similar to smgrextend(), except the relation can be extended by
 * multiple blocks at once and the added blocks will be filled with
 * zeroes.
 */
void
smgrzeroextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   int nblocks, bool skipFsync)
{
	smgrsw[reln->smgr_which].smgr_zeroextend(reln, forknum, blocknum,
											 nblocks, skipFsync);

	/*
	 * Normally we expect this to increase the fork size by nblocks, but if
	 * the cached value isn't as expected, just invalidate it so the next call
	 * asks the kernel.
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + nblocks;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
}

/*
 * smgrprefetch() -- Initiate asynchronous read of the specified block of a relation.
 *
 * In recovery only, this can return false to indicate that a file
 * doesn't exist (presumably it has been dropped by a later WAL
 * record).
 */
bool
smgrprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			 int nblocks)
{
	return smgrsw[reln->smgr_which].smgr_prefetch(reln, forknum, blocknum, nblocks);
}

/*
 * smgrreadv() -- read a particular block range from a relation into the
 *				 supplied buffers.
 *
 * This routine is called from the buffer manager in order to
 * instantiate pages in the shared buffer cache.  All storage managers
 * return pages in the format that POSTGRES expects.
 */
void
smgrreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		  void **buffers, BlockNumber nblocks)
{
	smgrsw[reln->smgr_which].smgr_readv(reln, forknum, blocknum, buffers,
										nblocks);
}

/*
 * smgrwritev() -- Write the supplied buffers out.
 *
 * This is to be used only for updating already-existing blocks of a
 * relation (ie, those before the current EOF).  To extend a relation,
 * use smgrextend().
 *
 * This is not a synchronous write -- the block is not necessarily
 * on disk at return, only dumped out to the kernel.  However,
 * provisions will be made to fsync the write before the next checkpoint.
 *
 * NB: The mechanism to ensure fsync at next checkpoint assumes that there is
 * something that prevents a concurrent checkpoint from "racing ahead" of the
 * write.  One way to prevent that is by holding a lock on the buffer; the
 * buffer manager's writes are protected by that.  The bulk writer facility
 * in bulk_write.c checks the redo pointer and calls smgrimmedsync() if a
 * checkpoint happened; that relies on the fact that no other backend can be
 * concurrently modifying the page.
 *
 * skipFsync indicates that the caller will make other provisions to
 * fsync the relation, so we needn't bother.  Temporary relations also
 * do not require fsync.
 */
void
smgrwritev(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	smgrsw[reln->smgr_which].smgr_writev(reln, forknum, blocknum,
										 buffers, nblocks, skipFsync);
}

/*
 * smgrwriteback() -- Trigger kernel writeback for the supplied range of
 *					   blocks.
 */
void
smgrwriteback(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			  BlockNumber nblocks)
{
	smgrsw[reln->smgr_which].smgr_writeback(reln, forknum, blocknum,
											nblocks);
}

/*
 * smgrnblocks() -- Calculate the number of blocks in the
 *					supplied relation.
 */
BlockNumber
smgrnblocks(SMgrRelation reln, ForkNumber forknum)
{
	BlockNumber result;

	/* Check and return if we get the cached value for the number of blocks. */
	result = smgrnblocks_cached(reln, forknum);
	if (result != InvalidBlockNumber)
		return result;

	result = smgrsw[reln->smgr_which].smgr_nblocks(reln, forknum);

	reln->smgr_cached_nblocks[forknum] = result;

	return result;
}

/*
 * smgrnblocks_cached() -- Get the cached number of blocks in the supplied
 *						   relation.
 *
 * Returns an InvalidBlockNumber when not in recovery and when the relation
 * fork size is not cached.
 */
BlockNumber
smgrnblocks_cached(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * For now, this function uses cached values only in recovery due to lack
	 * of a shared invalidation mechanism for changes in file size.  Code
	 * elsewhere reads smgr_cached_nblocks and copes with stale data.
	 */
	if (InRecovery && reln->smgr_cached_nblocks[forknum] != InvalidBlockNumber)
		return reln->smgr_cached_nblocks[forknum];

	return InvalidBlockNumber;
}

/*
 * smgrtruncate() -- Truncate the given forks of supplied relation to
 *					 each specified numbers of blocks
 *
 * The truncation is done immediately, so this can't be rolled back.
 *
 * The caller must hold AccessExclusiveLock on the relation, to ensure that
 * other backends receive the smgr invalidation event that this function sends
 * before they access any forks of the relation again.
 */
void
smgrtruncate(SMgrRelation reln, ForkNumber *forknum, int nforks, BlockNumber *nblocks)
{
	int			i;

	/*
	 * Get rid of any buffers for the about-to-be-deleted blocks. bufmgr will
	 * just drop them without bothering to write the contents.
	 */
	DropRelationBuffers(reln, forknum, nforks, nblocks);

	/*
	 * Send a shared-inval message to force other backends to close any smgr
	 * references they may have for this rel.  This is useful because they
	 * might have open file pointers to segments that got removed, and/or
	 * smgr_targblock variables pointing past the new rel end.  (The inval
	 * message will come back to our backend, too, causing a
	 * probably-unnecessary local smgr flush.  But we don't expect that this
	 * is a performance-critical path.)  As in the unlink code, we want to be
	 * sure the message is sent before we start changing things on-disk.
	 */
	CacheInvalidateSmgr(reln->smgr_rlocator);

	/* Do the truncation */
	for (i = 0; i < nforks; i++)
	{
		/* Make the cached size is invalid if we encounter an error. */
		reln->smgr_cached_nblocks[forknum[i]] = InvalidBlockNumber;

		smgrsw[reln->smgr_which].smgr_truncate(reln, forknum[i], nblocks[i]);

		/*
		 * We might as well update the local smgr_cached_nblocks values. The
		 * smgr cache inval message that this function sent will cause other
		 * backends to invalidate their copies of smgr_cached_nblocks, and
		 * these ones too at the next command boundary. But ensure they aren't
		 * outright wrong until then.
		 */
		reln->smgr_cached_nblocks[forknum[i]] = nblocks[i];
	}
}

/*
 * smgrregistersync() -- Request a relation to be sync'd at next checkpoint
 *
 * This can be used after calling smgrwrite() or smgrextend() with skipFsync =
 * true, to register the fsyncs that were skipped earlier.
 *
 * Note: be mindful that a checkpoint could already have happened between the
 * smgrwrite or smgrextend calls and this!  In that case, the checkpoint
 * already missed fsyncing this relation, and you should use smgrimmedsync
 * instead.  Most callers should use the bulk loading facility in bulk_write.c
 * which handles all that.
 */
void
smgrregistersync(SMgrRelation reln, ForkNumber forknum)
{
	smgrsw[reln->smgr_which].smgr_registersync(reln, forknum);
}

/*
 * smgrimmedsync() -- Force the specified relation to stable storage.
 *
 * Synchronously force all previous writes to the specified relation
 * down to disk.
 *
 * This is useful for building completely new relations (eg, new
 * indexes).  Instead of incrementally WAL-logging the index build
 * steps, we can just write completed index pages to disk with smgrwrite
 * or smgrextend, and then fsync the completed index file before
 * committing the transaction.  (This is sufficient for purposes of
 * crash recovery, since it effectively duplicates forcing a checkpoint
 * for the completed index.  But it is *not* sufficient if one wishes
 * to use the WAL log for PITR or replication purposes: in that case
 * we have to make WAL entries as well.)
 *
 * The preceding writes should specify skipFsync = true to avoid
 * duplicative fsyncs.
 *
 * Note that you need to do FlushRelationBuffers() first if there is
 * any possibility that there are dirty buffers for the relation;
 * otherwise the sync is not very meaningful.
 *
 * Most callers should use the bulk loading facility in bulk_write.c
 * instead of calling this directly.
 */
void
smgrimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	smgrsw[reln->smgr_which].smgr_immedsync(reln, forknum);
}

/*
 * AtEOXact_SMgr
 *
 * This routine is called during transaction commit or abort (it doesn't
 * particularly care which).  All unpinned SMgrRelation objects are destroyed.
 *
 * We do this as a compromise between wanting transient SMgrRelations to
 * live awhile (to amortize the costs of blind writes of multiple blocks)
 * and needing them to not live forever (since we're probably holding open
 * a kernel file descriptor for the underlying file, and we need to ensure
 * that gets closed reasonably soon if the file gets deleted).
 */
void
AtEOXact_SMgr(void)
{
	smgrdestroyall();
}

/*
 * This routine is called when we are ordered to release all open files by a
 * ProcSignalBarrier.
 */
bool
ProcessBarrierSmgrRelease(void)
{
	smgrreleaseall();
	return true;
}
