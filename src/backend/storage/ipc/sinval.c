/*-------------------------------------------------------------------------
 *
 * sinval.c
 *	  POSTGRES shared cache invalidation communication code.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
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
 * State for catchup events consists of two flags: one saying whether
 * the signal handler is currently allowed to call ProcessCatchupEvent
 * directly, and one saying whether the signal has occurred but the handler
 * was not allowed to call ProcessCatchupEvent at the time.
 *
 * NB: the "volatile" on these declarations is critical!  If your compiler
 * does not grok "volatile", you'd be best advised to compile this file
 * with all optimization turned off.
 */
static volatile int catchupInterruptEnabled = 0;
static volatile int catchupInterruptOccurred = 0;

static void ProcessCatchupEvent(void);


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
	if (catchupInterruptOccurred)
	{
		catchupInterruptOccurred = 0;
		elog(DEBUG4, "sinval catchup complete, cleaning queue");
		SICleanupQueue(false, 0);
	}
}


/*
 * HandleCatchupInterrupt
 *
 * This is called when PROCSIG_CATCHUP_INTERRUPT is received.
 *
 * If we are idle (catchupInterruptEnabled is set), we can safely
 * invoke ProcessCatchupEvent directly.  Otherwise, just set a flag
 * to do it later.  (Note that it's quite possible for normal processing
 * of the current transaction to cause ReceiveSharedInvalidMessages()
 * to be run later on; in that case the flag will get cleared again,
 * since there's no longer any reason to do anything.)
 */
void
HandleCatchupInterrupt(void)
{
	/*
	 * Note: this is called by a SIGNAL HANDLER. You must be very wary what
	 * you do here.
	 */

	/* Don't joggle the elbow of proc_exit */
	if (proc_exit_inprogress)
		return;

	if (catchupInterruptEnabled)
	{
		bool		save_ImmediateInterruptOK = ImmediateInterruptOK;

		/*
		 * We may be called while ImmediateInterruptOK is true; turn it off
		 * while messing with the catchup state.  This prevents problems if
		 * SIGINT or similar arrives while we're working.  Just to be real
		 * sure, bump the interrupt holdoff counter as well.  That way, even
		 * if something inside ProcessCatchupEvent() transiently sets
		 * ImmediateInterruptOK (eg while waiting on a lock), we won't get
		 * interrupted until we're done with the catchup interrupt.
		 */
		ImmediateInterruptOK = false;
		HOLD_INTERRUPTS();

		/*
		 * I'm not sure whether some flavors of Unix might allow another
		 * SIGUSR1 occurrence to recursively interrupt this routine. To cope
		 * with the possibility, we do the same sort of dance that
		 * EnableCatchupInterrupt must do --- see that routine for comments.
		 */
		catchupInterruptEnabled = 0;	/* disable any recursive signal */
		catchupInterruptOccurred = 1;	/* do at least one iteration */
		for (;;)
		{
			catchupInterruptEnabled = 1;
			if (!catchupInterruptOccurred)
				break;
			catchupInterruptEnabled = 0;
			if (catchupInterruptOccurred)
			{
				/* Here, it is finally safe to do stuff. */
				ProcessCatchupEvent();
			}
		}

		/*
		 * Restore the holdoff level and ImmediateInterruptOK, and check for
		 * interrupts if needed.
		 */
		RESUME_INTERRUPTS();
		ImmediateInterruptOK = save_ImmediateInterruptOK;
		if (save_ImmediateInterruptOK)
			CHECK_FOR_INTERRUPTS();
	}
	else
	{
		/*
		 * In this path it is NOT SAFE to do much of anything, except this:
		 */
		catchupInterruptOccurred = 1;
	}
}

/*
 * EnableCatchupInterrupt
 *
 * This is called by the PostgresMain main loop just before waiting
 * for a frontend command.  We process any pending catchup events,
 * and enable the signal handler to process future events directly.
 *
 * NOTE: the signal handler starts out disabled, and stays so until
 * PostgresMain calls this the first time.
 */
void
EnableCatchupInterrupt(void)
{
	/*
	 * This code is tricky because we are communicating with a signal handler
	 * that could interrupt us at any point.  If we just checked
	 * catchupInterruptOccurred and then set catchupInterruptEnabled, we could
	 * fail to respond promptly to a signal that happens in between those two
	 * steps.  (A very small time window, perhaps, but Murphy's Law says you
	 * can hit it...)  Instead, we first set the enable flag, then test the
	 * occurred flag.  If we see an unserviced interrupt has occurred, we
	 * re-clear the enable flag before going off to do the service work. (That
	 * prevents re-entrant invocation of ProcessCatchupEvent() if another
	 * interrupt occurs.) If an interrupt comes in between the setting and
	 * clearing of catchupInterruptEnabled, then it will have done the service
	 * work and left catchupInterruptOccurred zero, so we have to check again
	 * after clearing enable.  The whole thing has to be in a loop in case
	 * another interrupt occurs while we're servicing the first. Once we get
	 * out of the loop, enable is set and we know there is no unserviced
	 * interrupt.
	 *
	 * NB: an overenthusiastic optimizing compiler could easily break this
	 * code. Hopefully, they all understand what "volatile" means these days.
	 */
	for (;;)
	{
		catchupInterruptEnabled = 1;
		if (!catchupInterruptOccurred)
			break;
		catchupInterruptEnabled = 0;
		if (catchupInterruptOccurred)
			ProcessCatchupEvent();
	}
}

/*
 * DisableCatchupInterrupt
 *
 * This is called by the PostgresMain main loop just after receiving
 * a frontend command.  Signal handler execution of catchup events
 * is disabled until the next EnableCatchupInterrupt call.
 *
 * The PROCSIG_NOTIFY_INTERRUPT signal handler also needs to call this,
 * so as to prevent conflicts if one signal interrupts the other.  So we
 * must return the previous state of the flag.
 */
bool
DisableCatchupInterrupt(void)
{
	bool		result = (catchupInterruptEnabled != 0);

	catchupInterruptEnabled = 0;

	return result;
}

/*
 * ProcessCatchupEvent
 *
 * Respond to a catchup event (PROCSIG_CATCHUP_INTERRUPT) from another
 * backend.
 *
 * This is called either directly from the PROCSIG_CATCHUP_INTERRUPT
 * signal handler, or the next time control reaches the outer idle loop
 * (assuming there's still anything to do by then).
 */
static void
ProcessCatchupEvent(void)
{
	bool		notify_enabled;

	/* Must prevent notify interrupt while I am running */
	notify_enabled = DisableNotifyInterrupt();

	/*
	 * What we need to do here is cause ReceiveSharedInvalidMessages() to run,
	 * which will do the necessary work and also reset the
	 * catchupInterruptOccurred flag.  If we are inside a transaction we can
	 * just call AcceptInvalidationMessages() to do this.  If we aren't, we
	 * start and immediately end a transaction; the call to
	 * AcceptInvalidationMessages() happens down inside transaction start.
	 *
	 * It is awfully tempting to just call AcceptInvalidationMessages()
	 * without the rest of the xact start/stop overhead, and I think that
	 * would actually work in the normal case; but I am not sure that things
	 * would clean up nicely if we got an error partway through.
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

	if (notify_enabled)
		EnableNotifyInterrupt();
}
