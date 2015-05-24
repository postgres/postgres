/*-------------------------------------------------------------------------
 *
 * sinval.c
 *	  POSTGRES shared cache invalidation communication code.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/sinval.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "commands/async.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/sinvaladt.h"
#include "utils/inval.h"


uint64		SharedInvalidMessageCounter;


/*
 * Because backends sitting idle will not be reading sinval events, we
 * need a way to give an idle backend a swift kick in the rear and make
 * it catch up before the sinval queue overflows and forces it to go
 * through a cache reset exercise.  This is done by sending
 * PROCSIG_CATCHUP_INTERRUPT to any backend that gets too far behind.
 *
 * The signal handler will set an interrupt pending flag and will set the
 * processes latch. Whenever starting to read from the client, or when
 * interrupted while doing so, ProcessClientReadInterrupt() will call
 * ProcessCatchupEvent().
 */
volatile sig_atomic_t catchupInterruptPending = false;


/*
 * SendSharedInvalidMessages
 *	Add shared-cache-invalidation message(s) to the global SI message queue.
 */
void
SendSharedInvalidMessages(const SharedInvalidationMessage *msgs, int n)
{
	SIInsertDataEntries(msgs, n);
}

/*
 * ReceiveSharedInvalidMessages
 *		Process shared-cache-invalidation messages waiting for this backend
 *
 * We guarantee to process all messages that had been queued before the
 * routine was entered.  It is of course possible for more messages to get
 * queued right after our last SIGetDataEntries call.
 *
 * NOTE: it is entirely possible for this routine to be invoked recursively
 * as a consequence of processing inside the invalFunction or resetFunction.
 * Furthermore, such a recursive call must guarantee that all outstanding
 * inval messages have been processed before it exits.  This is the reason
 * for the strange-looking choice to use a statically allocated buffer array
 * and counters; it's so that a recursive call can process messages already
 * sucked out of sinvaladt.c.
 */
void
ReceiveSharedInvalidMessages(
					  void (*invalFunction) (SharedInvalidationMessage *msg),
							 void (*resetFunction) (void))
{
#define MAXINVALMSGS 32
	static SharedInvalidationMessage messages[MAXINVALMSGS];

	/*
	 * We use volatile here to prevent bugs if a compiler doesn't realize that
	 * recursion is a possibility ...
	 */
	static volatile int nextmsg = 0;
	static volatile int nummsgs = 0;

	/* Deal with any messages still pending from an outer recursion */
	while (nextmsg < nummsgs)
	{
		SharedInvalidationMessage msg = messages[nextmsg++];

		SharedInvalidMessageCounter++;
		invalFunction(&msg);
	}

	do
	{
		int			getResult;

		nextmsg = nummsgs = 0;

		/* Try to get some more messages */
		getResult = SIGetDataEntries(messages, MAXINVALMSGS);

		if (getResult < 0)
		{
			/* got a reset message */
			elog(DEBUG4, "cache state reset");
			SharedInvalidMessageCounter++;
			resetFunction();
			break;				/* nothing more to do */
		}

		/* Process them, being wary that a recursive call might eat some */
		nextmsg = 0;
		nummsgs = getResult;

		while (nextmsg < nummsgs)
		{
			SharedInvalidationMessage msg = messages[nextmsg++];

			SharedInvalidMessageCounter++;
			invalFunction(&msg);
		}

		/*
		 * We only need to loop if the last SIGetDataEntries call (which might
		 * have been within a recursive call) returned a full buffer.
		 */
	} while (nummsgs == MAXINVALMSGS);

	/*
	 * We are now caught up.  If we received a catchup signal, reset that
	 * flag, and call SICleanupQueue().  This is not so much because we need
	 * to flush dead messages right now, as that we want to pass on the
	 * catchup signal to the next slowest backend.  "Daisy chaining" the
	 * catchup signal this way avoids creating spikes in system load for what
	 * should be just a background maintenance activity.
	 */
	if (catchupInterruptPending)
	{
		catchupInterruptPending = false;
		elog(DEBUG4, "sinval catchup complete, cleaning queue");
		SICleanupQueue(false, 0);
	}
}


/*
 * HandleCatchupInterrupt
 *
 * This is called when PROCSIG_CATCHUP_INTERRUPT is received.
 *
 * We used to directly call ProcessCatchupEvent directly when idle. These days
 * we just set a flag to do it later and notify the process of that fact by
 * setting the process's latch.
 */
void
HandleCatchupInterrupt(void)
{
	/*
	 * Note: this is called by a SIGNAL HANDLER. You must be very wary what
	 * you do here.
	 */

	catchupInterruptPending = true;

	/* make sure the event is processed in due course */
	SetLatch(MyLatch);
}

/*
 * ProcessCatchupInterrupt
 *
 * The portion of catchup interrupt handling that runs outside of the signal
 * handler, which allows it to actually process pending invalidations.
 */
void
ProcessCatchupInterrupt(void)
{
	while (catchupInterruptPending)
	{
		/*
		 * What we need to do here is cause ReceiveSharedInvalidMessages() to
		 * run, which will do the necessary work and also reset the
		 * catchupInterruptPending flag.  If we are inside a transaction we
		 * can just call AcceptInvalidationMessages() to do this.  If we
		 * aren't, we start and immediately end a transaction; the call to
		 * AcceptInvalidationMessages() happens down inside transaction start.
		 *
		 * It is awfully tempting to just call AcceptInvalidationMessages()
		 * without the rest of the xact start/stop overhead, and I think that
		 * would actually work in the normal case; but I am not sure that
		 * things would clean up nicely if we got an error partway through.
		 */
		if (IsTransactionOrTransactionBlock())
		{
			elog(DEBUG4, "ProcessCatchupEvent inside transaction");
			AcceptInvalidationMessages();
		}
		else
		{
			elog(DEBUG4, "ProcessCatchupEvent outside transaction");
			StartTransactionCommand();
			CommitTransactionCommand();
		}
	}
}
