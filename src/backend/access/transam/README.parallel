Overview
========

PostgreSQL provides some simple facilities to make writing parallel algorithms
easier.  Using a data structure called a ParallelContext, you can arrange to
launch background worker processes, initialize their state to match that of
the backend which initiated parallelism, communicate with them via dynamic
shared memory, and write reasonably complex code that can run either in the
user backend or in one of the parallel workers without needing to be aware of
where it's running.

The backend which starts a parallel operation (hereafter, the initiating
backend) starts by creating a dynamic shared memory segment which will last
for the lifetime of the parallel operation.  This dynamic shared memory segment
will contain (1) a shm_mq that can be used to transport errors (and other
messages reported via elog/ereport) from the worker back to the initiating
backend; (2) serialized representations of the initiating backend's private
state, so that the worker can synchronize its state with of the initiating
backend; and (3) any other data structures which a particular user of the
ParallelContext data structure may wish to add for its own purposes.  Once
the initiating backend has initialized the dynamic shared memory segment, it
asks the postmaster to launch the appropriate number of parallel workers.
These workers then connect to the dynamic shared memory segment, initiate
their state, and then invoke the appropriate entrypoint, as further detailed
below.

Error Reporting
===============

When started, each parallel worker begins by attaching the dynamic shared
memory segment and locating the shm_mq to be used for error reporting; it
redirects all of its protocol messages to this shm_mq.  Prior to this point,
any failure of the background worker will not be reported to the initiating
backend; from the point of view of the initiating backend, the worker simply
failed to start.  The initiating backend must anyway be prepared to cope
with fewer parallel workers than it originally requested, so catering to
this case imposes no additional burden.

Whenever a new message (or partial message; very large messages may wrap) is
sent to the error-reporting queue, PROCSIG_PARALLEL_MESSAGE is sent to the
initiating backend.  This causes the next CHECK_FOR_INTERRUPTS() in the
initiating backend to read and rethrow the message.  For the most part, this
makes error reporting in parallel mode "just work".  Of course, to work
properly, it is important that the code the initiating backend is executing
CHECK_FOR_INTERRUPTS() regularly and avoid blocking interrupt processing for
long periods of time, but those are good things to do anyway.

(A currently-unsolved problem is that some messages may get written to the
system log twice, once in the backend where the report was originally
generated, and again when the initiating backend rethrows the message.  If
we decide to suppress one of these reports, it should probably be second one;
otherwise, if the worker is for some reason unable to propagate the message
back to the initiating backend, the message will be lost altogether.)

State Sharing
=============

It's possible to write C code which works correctly without parallelism, but
which fails when parallelism is used.  No parallel infrastructure can
completely eliminate this problem, because any global variable is a risk.
There's no general mechanism for ensuring that every global variable in the
worker will have the same value that it does in the initiating backend; even
if we could ensure that, some function we're calling could update the variable
after each call, and only the backend where that update is performed will see
the new value.  Similar problems can arise with any more-complex data
structure we might choose to use.  For example, a pseudo-random number
generator should, given a particular seed value, produce the same predictable
series of values every time.  But it does this by relying on some private
state which won't automatically be shared between cooperating backends.  A
parallel-safe PRNG would need to store its state in dynamic shared memory, and
would require locking.  The parallelism infrastructure has no way of knowing
whether the user intends to call code that has this sort of problem, and can't
do anything about it anyway.

Instead, we take a more pragmatic approach. First, we try to make as many of
the operations that are safe outside of parallel mode work correctly in
parallel mode as well.  Second, we try to prohibit common unsafe operations
via suitable error checks.  These checks are intended to catch 100% of
unsafe things that a user might do from the SQL interface, but code written
in C can do unsafe things that won't trigger these checks.  The error checks
are engaged via EnterParallelMode(), which should be called before creating
a parallel context, and disarmed via ExitParallelMode(), which should be
called after all parallel contexts have been destroyed.  The most
significant restriction imposed by parallel mode is that all operations must
be strictly read-only; we allow no writes to the database and no DDL.  We
might try to relax these restrictions in the future.

To make as many operations as possible safe in parallel mode, we try to copy
the most important pieces of state from the initiating backend to each parallel
worker.  This includes:

  - The set of libraries dynamically loaded by dfmgr.c.

  - The authenticated user ID and current database.  Each parallel worker
    will connect to the same database as the initiating backend, using the
    same user ID.

  - The values of all GUCs.  Accordingly, permanent changes to the value of
    any GUC are forbidden while in parallel mode; but temporary changes,
    such as entering a function with non-NULL proconfig, are OK.

  - The current subtransaction's XID, the top-level transaction's XID, and
    the list of XIDs considered current (that is, they are in-progress or
    subcommitted).  This information is needed to ensure that tuple visibility
    checks return the same results in the worker as they do in the
    initiating backend.  See also the section Transaction Integration, below.

  - The combo CID mappings.  This is needed to ensure consistent answers to
    tuple visibility checks.  The need to synchronize this data structure is
    a major reason why we can't support writes in parallel mode: such writes
    might create new combo CIDs, and we have no way to let other workers
    (or the initiating backend) know about them.

  - The transaction snapshot.

  - The active snapshot, which might be different from the transaction
    snapshot.

  - The currently active user ID and security context.  Note that this is
    the fourth user ID we restore: the initial step of binding to the correct
    database also involves restoring the authenticated user ID.  When GUC
    values are restored, this incidentally sets SessionUserId and OuterUserId
    to the correct values.  This final step restores CurrentUserId.

  - State related to pending REINDEX operations, which prevents access to
    an index that is currently being rebuilt.

  - Active relmapper.c mapping state.  This is needed to allow consistent
    answers when fetching the current relfilenumber for relation oids of
    mapped relations.

To prevent unprincipled deadlocks when running in parallel mode, this code
also arranges for the leader and all workers to participate in group
locking.  See src/backend/storage/lmgr/README for more details.

Transaction Integration
=======================

Regardless of what the TransactionState stack looks like in the parallel
leader, each parallel worker begins with a stack of depth 1.  This stack
entry is marked with the special transaction block state
TBLOCK_PARALLEL_INPROGRESS so that it's not confused with an ordinary
toplevel transaction.  The XID of this TransactionState is set to the XID of
the innermost currently-active subtransaction in the initiating backend.  The
initiating backend's toplevel XID, and the XIDs of all current (in-progress
or subcommitted) XIDs are stored separately from the TransactionState stack,
but in such a way that GetTopTransactionId(), GetTopTransactionIdIfAny(), and
TransactionIdIsCurrentTransactionId() return the same values that they would
in the initiating backend.  We could copy the entire transaction state stack,
but most of it would be useless: for example, you can't roll back to a
savepoint from within a parallel worker, and there are no resources to
associated with the memory contexts or resource owners of intermediate
subtransactions.

No meaningful change to the transaction state can be made while in parallel
mode.  No XIDs can be assigned, and no command counter increments can happen,
because we have no way of communicating these state changes to cooperating
backends, or of synchronizing them.  It's clearly unworkable for the initiating
backend to exit any transaction or subtransaction that was in progress when
parallelism was started before all parallel workers have exited; and it's even
more clearly crazy for a parallel worker to try to subcommit or subabort the
current subtransaction and execute in some other transaction context than was
present in the initiating backend.  However, we allow internal subtransactions
(e.g. those used to implement a PL/pgSQL EXCEPTION block) to be used in
parallel mode, provided that they remain XID-less, because other backends
don't really need to know about those transactions or do anything differently
because of them.

At the end of a parallel operation, which can happen either because it
completed successfully or because it was interrupted by an error, parallel
workers associated with that operation exit.  In the error case, transaction
abort processing in the parallel leader kills off any remaining workers, and
the parallel leader then waits for them to die.  In the case of a successful
parallel operation, the parallel leader does not send any signals, but must
wait for workers to complete and exit of their own volition.  In either
case, it is very important that all workers actually exit before the
parallel leader cleans up the (sub)transaction in which they were created;
otherwise, chaos can ensue.  For example, if the leader is rolling back the
transaction that created the relation being scanned by a worker, the
relation could disappear while the worker is still busy scanning it.  That's
not safe.

Generally, the cleanup performed by each worker at this point is similar to
top-level commit or abort.  Each backend has its own resource owners: buffer
pins, catcache or relcache reference counts, tuple descriptors, and so on
are managed separately by each backend, and must free them before exiting.
There are, however, some important differences between parallel worker
commit or abort and a real top-level transaction commit or abort.  Most
importantly:

  - No commit or abort record is written; the initiating backend is
    responsible for this.

  - Cleanup of pg_temp namespaces is not done.  Parallel workers cannot
    safely access the initiating backend's pg_temp namespace, and should
    not create one of their own.

Coding Conventions
===================

Before beginning any parallel operation, call EnterParallelMode(); after all
parallel operations are completed, call ExitParallelMode().  To actually
parallelize a particular operation, use a ParallelContext.  The basic coding
pattern looks like this:

	EnterParallelMode();		/* prohibit unsafe state changes */

	pcxt = CreateParallelContext("library_name", "function_name", nworkers);

	/* Allow space for application-specific data here. */
	shm_toc_estimate_chunk(&pcxt->estimator, size);
	shm_toc_estimate_keys(&pcxt->estimator, keys);

	InitializeParallelDSM(pcxt);	/* create DSM and copy state to it */

	/* Store the data for which we reserved space. */
	space = shm_toc_allocate(pcxt->toc, size);
	shm_toc_insert(pcxt->toc, key, space);

	LaunchParallelWorkers(pcxt);

	/* do parallel stuff */

	WaitForParallelWorkersToFinish(pcxt);

	/* read any final results from dynamic shared memory */

	DestroyParallelContext(pcxt);

	ExitParallelMode();

If desired, after WaitForParallelWorkersToFinish() has been called, the
context can be reset so that workers can be launched anew using the same
parallel context.  To do this, first call ReinitializeParallelDSM() to
reinitialize state managed by the parallel context machinery itself; then,
perform any other necessary resetting of state; after that, you can again
call LaunchParallelWorkers.
