/*-------------------------------------------------------------------------
 *
 * walreceiverfuncs.c
 *
 * This file contains functions used by the startup process to communicate
 * with the walreceiver process. Functions implementing walreceiver itself
 * are in walreceiver.c.
 *
 * Portions Copyright (c) 2010-2010, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/replication/walreceiverfuncs.c,v 1.2 2010/01/20 09:16:24 heikki Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>

#include "access/xlog_internal.h"
#include "replication/walreceiver.h"
#include "storage/fd.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "utils/guc.h"

WalRcvData *WalRcv = NULL;

static bool CheckForStandbyTrigger(void);
static void ShutdownWalRcv(void);

/* Report shared memory space needed by WalRcvShmemInit */
Size
WalRcvShmemSize(void)
{
	Size size = 0;

	size = add_size(size, sizeof(WalRcvData));

	return size;
}

/* Allocate and initialize walreceiver-related shared memory */
void
WalRcvShmemInit(void)
{
	bool	found;

	WalRcv = (WalRcvData *)
		ShmemInitStruct("Wal Receiver Ctl", WalRcvShmemSize(), &found);

	if (WalRcv == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("not enough shared memory for walreceiver")));
	if (found)
		return;					/* already initialized */

	/* Initialize the data structures */
	MemSet(WalRcv, 0, WalRcvShmemSize());
	WalRcv->walRcvState = WALRCV_NOT_STARTED;
	SpinLockInit(&WalRcv->mutex);
}

/* Is walreceiver in progress (or starting up)? */
bool
WalRcvInProgress(void)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;
	WalRcvState state;

	SpinLockAcquire(&walrcv->mutex);
	state = walrcv->walRcvState;
	SpinLockRelease(&walrcv->mutex);

	if (state == WALRCV_RUNNING || state == WALRCV_STOPPING)
		return true;
	else
		return false;
}

/*
 * Wait for the XLOG record at given position to become available.
 *
 * 'recptr' indicates the byte position which caller wants to read the
 * XLOG record up to. The byte position actually written and flushed
 * by walreceiver is returned. It can be higher than the requested
 * location, and the caller can safely read up to that point without
 * calling WaitNextXLogAvailable() again.
 *
 * If WAL streaming is ended (because a trigger file is found), *finished
 * is set to true and function returns immediately. The returned position
 * can be lower than requested in that case.
 *
 * Called by the startup process during streaming recovery.
 */
XLogRecPtr
WaitNextXLogAvailable(XLogRecPtr recptr, bool *finished)
{
	static XLogRecPtr receivedUpto = {0, 0};

	*finished = false;

	/* Quick exit if already known available */
	if (XLByteLT(recptr, receivedUpto))
		return receivedUpto;

	for (;;)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalRcvData *walrcv = WalRcv;

		/* Update local status */
		SpinLockAcquire(&walrcv->mutex);
		receivedUpto = walrcv->receivedUpto;
		SpinLockRelease(&walrcv->mutex);

		/* If available already, leave here */
		if (XLByteLT(recptr, receivedUpto))
			return receivedUpto;

		/* Check to see if the trigger file exists */
		if (CheckForStandbyTrigger())
		{
			*finished = true;
			return receivedUpto;
		}

		pg_usleep(100000L); /* 100ms */

		/*
		 * This possibly-long loop needs to handle interrupts of startup
		 * process.
		 */
		HandleStartupProcInterrupts();

		/*
		 * Emergency bailout if postmaster has died.  This is to avoid the
		 * necessity for manual cleanup of all postmaster children.
		 */
		if (!PostmasterIsAlive(true))
			exit(1);
	}
}

/*
 * Stop walreceiver and wait for it to die.
 */
static void
ShutdownWalRcv(void)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;
	pid_t walrcvpid;

	/*
	 * Request walreceiver to stop. Walreceiver will switch to WALRCV_STOPPED
	 * mode once it's finished, and will also request postmaster to not
	 * restart itself.
	 */
	SpinLockAcquire(&walrcv->mutex);
	Assert(walrcv->walRcvState == WALRCV_RUNNING);
	walrcv->walRcvState = WALRCV_STOPPING;
	walrcvpid = walrcv->pid;
	SpinLockRelease(&walrcv->mutex);

	/*
	 * Pid can be 0, if no walreceiver process is active right now.
	 * Postmaster should restart it, and when it does, it will see the
	 * STOPPING state.
	 */
	if (walrcvpid != 0)
		kill(walrcvpid, SIGTERM);

	/*
	 * Wait for walreceiver to acknowledge its death by setting state to
	 * WALRCV_STOPPED.
	 */
	while (WalRcvInProgress())
	{
		/*
		 * This possibly-long loop needs to handle interrupts of startup
		 * process.
		 */
		HandleStartupProcInterrupts();

		pg_usleep(100000);		/* 100ms */
	}
}

/*
 * Check to see if the trigger file exists. If it does, request postmaster
 * to shut down walreceiver and wait for it to exit, and remove the trigger
 * file.
 */
static bool
CheckForStandbyTrigger(void)
{
	struct stat stat_buf;

	if (TriggerFile == NULL)
		return false;

	if (stat(TriggerFile, &stat_buf) == 0)
	{
		ereport(LOG,
				(errmsg("trigger file found: %s", TriggerFile)));
		ShutdownWalRcv();
		unlink(TriggerFile);
		return true;
	}
	return false;
}

/*
 * Request postmaster to start walreceiver.
 *
 * recptr indicates the position where streaming should begin, and conninfo
 * is a libpq connection string to use.
 */
void
RequestXLogStreaming(XLogRecPtr recptr, const char *conninfo)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;

	Assert(walrcv->walRcvState == WALRCV_NOT_STARTED);

	/* locking is just pro forma here; walreceiver isn't started yet */
	SpinLockAcquire(&walrcv->mutex);
	walrcv->receivedUpto = recptr;
	if (conninfo != NULL)
		strlcpy((char *) walrcv->conninfo, conninfo, MAXCONNINFO);
	else
		walrcv->conninfo[0] = '\0';
	walrcv->walRcvState = WALRCV_RUNNING;
	SpinLockRelease(&walrcv->mutex);

	SendPostmasterSignal(PMSIGNAL_START_WALRECEIVER);
}

/*
 * Returns the byte position that walreceiver has written
 */
XLogRecPtr
GetWalRcvWriteRecPtr(void)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;
	XLogRecPtr	recptr;

	SpinLockAcquire(&walrcv->mutex);
	recptr = walrcv->receivedUpto;
	SpinLockRelease(&walrcv->mutex);

	return recptr;
}
