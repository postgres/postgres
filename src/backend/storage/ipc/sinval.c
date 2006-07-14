/*-------------------------------------------------------------------------
 *
 * sinval.c
 *	  POSTGRES shared cache invalidation communication code.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/sinval.c,v 1.81 2006/07/14 14:52:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "access/xact.h"
#include "commands/async.h"
#include "miscadmin.h"
#include "storage/backendid.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/sinvaladt.h"
#include "utils/inval.h"


/*
 * Because backends sitting idle will not be reading sinval events, we
 * need a way to give an idle backend a swift kick in the rear and make
 * it catch up before the sinval queue overflows and forces everyone
 * through a cache reset exercise.	This is done by broadcasting SIGUSR1
 * to all backends when the queue is threatening to become full.
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


/****************************************************************************/
/*	CreateSharedInvalidationState()		 Initialize SI buffer				*/
/*																			*/
/*	should be called only by the POSTMASTER									*/
/****************************************************************************/
void
CreateSharedInvalidationState(void)
{
	/* SInvalLock must be initialized already, during LWLock init */
	SIBufferInit();
}

/*
 * InitBackendSharedInvalidationState
 *		Initialize new backend's state info in buffer segment.
 */
void
InitBackendSharedInvalidationState(void)
{
	int			flag;

	LWLockAcquire(SInvalLock, LW_EXCLUSIVE);
	flag = SIBackendInit(shmInvalBuffer);
	LWLockRelease(SInvalLock);
	if (flag < 0)				/* unexpected problem */
		elog(FATAL, "shared cache invalidation initialization failed");
	if (flag == 0)				/* expected problem: MaxBackends exceeded */
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
				 errmsg("sorry, too many clients already")));
}

/*
 * SendSharedInvalidMessage
 *	Add a shared-cache-invalidation message to the global SI message queue.
 */
void
SendSharedInvalidMessage(SharedInvalidationMessage *msg)
{
	bool		insertOK;

	LWLockAcquire(SInvalLock, LW_EXCLUSIVE);
	insertOK = SIInsertDataEntry(shmInvalBuffer, msg);
	LWLockRelease(SInvalLock);
	if (!insertOK)
		elog(DEBUG4, "SI buffer overflow");
}

/*
 * ReceiveSharedInvalidMessages
 *		Process shared-cache-invalidation messages waiting for this backend
 *
 * NOTE: it is entirely possible for this routine to be invoked recursively
 * as a consequence of processing inside the invalFunction or resetFunction.
 * Hence, we must be holding no SI resources when we call them.  The only
 * bad side-effect is that SIDelExpiredDataEntries might be called extra
 * times on the way out of a nested call.
 */
void
ReceiveSharedInvalidMessages(
					  void (*invalFunction) (SharedInvalidationMessage *msg),
							 void (*resetFunction) (void))
{
	SharedInvalidationMessage data;
	int			getResult;
	bool		gotMessage = false;

	for (;;)
	{
		/*
		 * We can discard any pending catchup event, since we will not exit
		 * this loop until we're fully caught up.
		 */
		catchupInterruptOccurred = 0;

		/*
		 * We can run SIGetDataEntry in parallel with other backends running
		 * SIGetDataEntry for themselves, since each instance will modify only
		 * fields of its own backend's ProcState, and no instance will look at
		 * fields of other backends' ProcStates.  We express this by grabbing
		 * SInvalLock in shared mode.  Note that this is not exactly the
		 * normal (read-only) interpretation of a shared lock! Look closely at
		 * the interactions before allowing SInvalLock to be grabbed in shared
		 * mode for any other reason!
		 */
		LWLockAcquire(SInvalLock, LW_SHARED);
		getResult = SIGetDataEntry(shmInvalBuffer, MyBackendId, &data);
		LWLockRelease(SInvalLock);

		if (getResult == 0)
			break;				/* nothing more to do */
		if (getResult < 0)
		{
			/* got a reset message */
			elog(DEBUG4, "cache state reset");
			resetFunction();
		}
		else
		{
			/* got a normal data message */
			invalFunction(&data);
		}
		gotMessage = true;
	}

	/* If we got any messages, try to release dead messages */
	if (gotMessage)
	{
		LWLockAcquire(SInvalLock, LW_EXCLUSIVE);
		SIDelExpiredDataEntries(shmInvalBuffer);
		LWLockRelease(SInvalLock);
	}
}


/*
 * CatchupInterruptHandler
 *
 * This is the signal handler for SIGUSR1.
 *
 * If we are idle (catchupInterruptEnabled is set), we can safely
 * invoke ProcessCatchupEvent directly.  Otherwise, just set a flag
 * to do it later.	(Note that it's quite possible for normal processing
 * of the current transaction to cause ReceiveSharedInvalidMessages()
 * to be run later on; in that case the flag will get cleared again,
 * since there's no longer any reason to do anything.)
 */
void
CatchupInterruptHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/*
	 * Note: this is a SIGNAL HANDLER.	You must be very wary what you do
	 * here.
	 */

	/* Don't joggle the elbow of proc_exit */
	if (proc_exit_inprogress)
		return;

	if (catchupInterruptEnabled)
	{
		bool		save_ImmediateInterruptOK = ImmediateInterruptOK;

		/*
		 * We may be called while ImmediateInterruptOK is true; turn it off
		 * while messing with the catchup state.  (We would have to save and
		 * restore it anyway, because PGSemaphore operations inside
		 * ProcessCatchupEvent() might reset it.)
		 */
		ImmediateInterruptOK = false;

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
		 * Restore ImmediateInterruptOK, and check for interrupts if needed.
		 */
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

	errno = save_errno;
}

/*
 * EnableCatchupInterrupt
 *
 * This is called by the PostgresMain main loop just before waiting
 * for a frontend command.	We process any pending catchup events,
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
 * a frontend command.	Signal handler execution of catchup events
 * is disabled until the next EnableCatchupInterrupt call.
 *
 * The SIGUSR2 signal handler also needs to call this, so as to
 * prevent conflicts if one signal interrupts the other.  So we
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
 * Respond to a catchup event (SIGUSR1) from another backend.
 *
 * This is called either directly from the SIGUSR1 signal handler,
 * or the next time control reaches the outer idle loop (assuming
 * there's still anything to do by then).
 */
static void
ProcessCatchupEvent(void)
{
	bool		notify_enabled;

	/* Must prevent SIGUSR2 interrupt while I am running */
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
