/*-------------------------------------------------------------------------
 *
 * pg_tde_xact_handler.c
 *	  Transaction handling routines for pg_tde
 *
 *
 * IDENTIFICATION
 *	  src/transam/pg_tde_xact_handler.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/elog.h"
#include "storage/fd.h"
#include "transam/pg_tde_xact_handler.h"
#include "access/pg_tde_tdemap.h"

typedef struct PendingMapEntryDelete
{
    off_t   map_entry_offset;               /* map entry offset */
    RelFileLocator rlocator;                /* main for use as relation OID */
    bool    atCommit;                       /* T=delete at commit; F=delete at abort */
    int     nestLevel;                      /* xact nesting level of request */
    struct  PendingMapEntryDelete *next;    /* linked-list link */
} PendingMapEntryDelete;

static PendingMapEntryDelete *pendingDeletes = NULL; /* head of linked list */

static void do_pending_deletes(bool isCommit);
static void pending_delete_cleanup(void);

/* Transaction Callbacks from Backend*/
void
pg_tde_xact_callback(XactEvent event, void *arg)
{
    if (event == XACT_EVENT_PARALLEL_ABORT ||
        event == XACT_EVENT_ABORT)
    {
        ereport(DEBUG2,
                (errmsg("pg_tde_xact_callback: aborting transaction")));
        do_pending_deletes(false);
    }
    else if (event == XACT_EVENT_COMMIT)
    {
        do_pending_deletes(true);
        pending_delete_cleanup();
    }
    else if (event == XACT_EVENT_PREPARE)
    {
        pending_delete_cleanup();
    }
}

void
pg_tde_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
                       SubTransactionId parentSubid, void *arg)
{
    /* TODO: takle all possible transaction states */
    if (event == SUBXACT_EVENT_ABORT_SUB)
    {
        ereport(DEBUG2,
                (errmsg("pg_tde_subxact_callback: aborting subtransaction")));
        do_pending_deletes(false);
    }
}

void
RegisterEntryForDeletion(const RelFileLocator *rlocator, off_t map_entry_offset, bool atCommit)
{
    PendingMapEntryDelete *pending;
    pending = (PendingMapEntryDelete *) MemoryContextAlloc(TopMemoryContext, sizeof(PendingMapEntryDelete));
    pending->map_entry_offset = map_entry_offset;
    memcpy(&pending->rlocator, rlocator, sizeof(RelFileLocator));
    pending->atCommit = atCommit;  /* delete if abort */
    pending->nestLevel = GetCurrentTransactionNestLevel();
    pending->next = pendingDeletes;
    pendingDeletes = pending;
}

/*
  *  do_pending_deletes() -- Take care of file deletes at end of xact.
  *
  * This also runs when aborting a subxact; we want to clean up a failed
  * subxact immediately.
  * 
  */
static void
do_pending_deletes(bool isCommit)
{
    int nestLevel = GetCurrentTransactionNestLevel();
    PendingMapEntryDelete *pending;
    PendingMapEntryDelete *prev;
    PendingMapEntryDelete *next;

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
            {
                ereport(LOG,
                        (errmsg("pg_tde_xact_callback: deleting entry at offset %d",
                                (int)(pending->map_entry_offset))));

                pg_tde_free_key_map_entry(&pending->rlocator, pending->map_entry_offset);
            }

            pfree(pending);
            /* prev does not change */
        }
    }
}

/*
  *  pending_delete_cleanup -- Clean up after a successful PREPARE or COMMIT
  *
  * What we have to do here is throw away the in-memory state about pending
  * file deletes.  It's all been recorded in the 2PC state file and
  * it's no longer our job to worry about it.
  */
static void
pending_delete_cleanup(void)
{
    PendingMapEntryDelete *pending;
    PendingMapEntryDelete *next;

    for (pending = pendingDeletes; pending != NULL; pending = next)
    {
        next = pending->next;
        pendingDeletes = next;
        pfree(pending);
    }
}
