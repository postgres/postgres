/*--------------------------------------------------------------------------
 *
 * worker.c
 *		Code for sample worker making use of shared memory message queues.
 *		Our test worker simply reads messages from one message queue and
 *		writes them back out to another message queue.  In a real
 *		application, you'd presumably want the worker to do some more
 *		complex calculation rather than simply returning the input,
 *		but it should be possible to use much of the control logic just
 *		as presented here.
 *
 * Copyright (c) 2013-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_shm_mq/worker.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "tcop/tcopprot.h"

#include "test_shm_mq.h"

static void attach_to_queues(dsm_segment *seg, shm_toc *toc,
							 int myworkernumber, shm_mq_handle **inqhp,
							 shm_mq_handle **outqhp);
static void copy_messages(shm_mq_handle *inqh, shm_mq_handle *outqh);

/*
 * Background worker entrypoint.
 *
 * This is intended to demonstrate how a background worker can be used to
 * facilitate a parallel computation.  Most of the logic here is fairly
 * boilerplate stuff, designed to attach to the shared memory segment,
 * notify the user backend that we're alive, and so on.  The
 * application-specific bits of logic that you'd replace for your own worker
 * are attach_to_queues() and copy_messages().
 */
void
test_shm_mq_main(Datum main_arg)
{
	dsm_segment *seg;
	shm_toc    *toc;
	shm_mq_handle *inqh;
	shm_mq_handle *outqh;
	volatile test_shm_mq_header *hdr;
	int			myworkernumber;
	PGPROC	   *registrant;

	/*
	 * Establish signal handlers.
	 *
	 * We want CHECK_FOR_INTERRUPTS() to kill off this worker process just as
	 * it would a normal user backend.  To make that happen, we use die().
	 */
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/*
	 * Connect to the dynamic shared memory segment.
	 *
	 * The backend that registered this worker passed us the ID of a shared
	 * memory segment to which we must attach for further instructions.  Once
	 * we've mapped the segment in our address space, attach to the table of
	 * contents so we can locate the various data structures we'll need to
	 * find within the segment.
	 *
	 * Note: at this point, we have not created any ResourceOwner in this
	 * process.  This will result in our DSM mapping surviving until process
	 * exit, which is fine.  If there were a ResourceOwner, it would acquire
	 * ownership of the mapping, but we have no need for that.
	 */
	seg = dsm_attach(DatumGetInt32(main_arg));
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("unable to map dynamic shared memory segment")));
	toc = shm_toc_attach(PG_TEST_SHM_MQ_MAGIC, dsm_segment_address(seg));
	if (toc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("bad magic number in dynamic shared memory segment")));

	/*
	 * Acquire a worker number.
	 *
	 * By convention, the process registering this background worker should
	 * have stored the control structure at key 0.  We look up that key to
	 * find it.  Our worker number gives our identity: there may be just one
	 * worker involved in this parallel operation, or there may be many.
	 */
	hdr = shm_toc_lookup(toc, 0, false);
	SpinLockAcquire(&hdr->mutex);
	myworkernumber = ++hdr->workers_attached;
	SpinLockRelease(&hdr->mutex);
	if (myworkernumber > hdr->workers_total)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("too many message queue testing workers already")));

	/*
	 * Attach to the appropriate message queues.
	 */
	attach_to_queues(seg, toc, myworkernumber, &inqh, &outqh);

	/*
	 * Indicate that we're fully initialized and ready to begin the main part
	 * of the parallel operation.
	 *
	 * Once we signal that we're ready, the user backend is entitled to assume
	 * that our on_dsm_detach callbacks will fire before we disconnect from
	 * the shared memory segment and exit.  Generally, that means we must have
	 * attached to all relevant dynamic shared memory data structures by now.
	 */
	SpinLockAcquire(&hdr->mutex);
	++hdr->workers_ready;
	SpinLockRelease(&hdr->mutex);
	registrant = BackendPidGetProc(MyBgworkerEntry->bgw_notify_pid);
	if (registrant == NULL)
	{
		elog(DEBUG1, "registrant backend has exited prematurely");
		proc_exit(1);
	}
	SetLatch(&registrant->procLatch);

	/* Do the work. */
	copy_messages(inqh, outqh);

	/*
	 * We're done.  For cleanliness, explicitly detach from the shared memory
	 * segment (that would happen anyway during process exit, though).
	 */
	dsm_detach(seg);
	proc_exit(1);
}

/*
 * Attach to shared memory message queues.
 *
 * We use our worker number to determine to which queue we should attach.
 * The queues are registered at keys 1..<number-of-workers>.  The user backend
 * writes to queue #1 and reads from queue #<number-of-workers>; each worker
 * reads from the queue whose number is equal to its worker number and writes
 * to the next higher-numbered queue.
 */
static void
attach_to_queues(dsm_segment *seg, shm_toc *toc, int myworkernumber,
				 shm_mq_handle **inqhp, shm_mq_handle **outqhp)
{
	shm_mq	   *inq;
	shm_mq	   *outq;

	inq = shm_toc_lookup(toc, myworkernumber, false);
	shm_mq_set_receiver(inq, MyProc);
	*inqhp = shm_mq_attach(inq, seg, NULL);
	outq = shm_toc_lookup(toc, myworkernumber + 1, false);
	shm_mq_set_sender(outq, MyProc);
	*outqhp = shm_mq_attach(outq, seg, NULL);
}

/*
 * Loop, receiving and sending messages, until the connection is broken.
 *
 * This is the "real work" performed by this worker process.  Everything that
 * happens before this is initialization of one form or another, and everything
 * after this point is cleanup.
 */
static void
copy_messages(shm_mq_handle *inqh, shm_mq_handle *outqh)
{
	Size		len;
	void	   *data;
	shm_mq_result res;

	for (;;)
	{
		/* Notice any interrupts that have occurred. */
		CHECK_FOR_INTERRUPTS();

		/* Receive a message. */
		res = shm_mq_receive(inqh, &len, &data, false);
		if (res != SHM_MQ_SUCCESS)
			break;

		/* Send it back out. */
		res = shm_mq_send(outqh, len, data, false);
		if (res != SHM_MQ_SUCCESS)
			break;
	}
}
