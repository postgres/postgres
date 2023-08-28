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

typedef struct PendingFileDelete
{
    char    *path;    /* file that may need to be deleted */
    bool    atCommit;       /* T=delete at commit; F=delete at abort */
    int     nestLevel;      /* xact nesting level of request */
    struct PendingFileDelete *next;  /* linked-list link */
} PendingFileDelete;

static PendingFileDelete *pendingDeletes = NULL; /* head of linked list */

static void cleanup_pending_deletes(bool atCommit);
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
        cleanup_pending_deletes(true);
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
RegisterFileForDeletion(const char *filePath, bool atCommit)
{
    PendingFileDelete *pending;
    pending = (PendingFileDelete *) MemoryContextAlloc(TopMemoryContext, sizeof(PendingFileDelete));
    pending->path = MemoryContextStrdup(TopMemoryContext, filePath);
    pending->atCommit = atCommit;  /* delete if abort */
    pending->nestLevel = GetCurrentTransactionNestLevel();
    pending->next = pendingDeletes;
    pendingDeletes = pending;
}

/*
  * cleanup_pending_deletes
  *      Mark a relation as not to be deleted after all.
  */
static void
cleanup_pending_deletes(bool atCommit)
{
    PendingFileDelete *pending;
    PendingFileDelete *prev;
    PendingFileDelete *next;

    prev = NULL;
    for (pending = pendingDeletes; pending != NULL; pending = next)
    {
        next = pending->next;
        if (pending->atCommit == atCommit)
        {
            /* unlink and delete list entry */
            if (prev)
                prev->next = next;
            else
                pendingDeletes = next;
            if (pending->path)
                pfree(pending->path);
            pfree(pending);
            /* prev does not change */
        }
        else
        {
            /* unrelated entry, don't touch it */
            prev = pending;
        }
    }
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
    int         nestLevel = GetCurrentTransactionNestLevel();
    PendingFileDelete *pending;
    PendingFileDelete *prev;
    PendingFileDelete *next;

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
                        (errmsg("pg_tde_xact_callback: deleting file %s",
                                pending->path)));
                durable_unlink(pending->path, WARNING); /* TODO: should it be ERROR? */
            }
            /* must explicitly free the list entry */
            if(pending->path)
                pfree(pending->path);
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
    PendingFileDelete *pending;
    PendingFileDelete *next;

    for (pending = pendingDeletes; pending != NULL; pending = next)
    {
        next = pending->next;
        pendingDeletes = next;
        /* must explicitly free the list entry */
        if(pending->path)
            pfree(pending->path);
        pfree(pending);
    }
}
