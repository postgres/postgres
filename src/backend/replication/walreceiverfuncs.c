/*-------------------------------------------------------------------------
 *
 * walreceiverfuncs.c
 *
 * This file contains functions used by the startup process to communicate
 * with the walreceiver process. Functions implementing walreceiver itself
 * are in walreceiver.c.
 *
 * Portions Copyright (c) 2010-2015, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/walreceiverfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include "access/xlog_internal.h"
#include "postmaster/startup.h"
#include "replication/walreceiver.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "utils/timestamp.h"

WalRcvData *WalRcv = NULL;

/*
 * How long to wait for walreceiver to start up after requesting
 * postmaster to launch it. In seconds.
 */
#define WALRCV_STARTUP_TIMEOUT 10

/* Report shared memory space needed by WalRcvShmemInit */
Size
WalRcvShmemSize(void)
{
	Size		size = 0;

	size = add_size(size, sizeof(WalRcvData));

	return size;
}

/* Allocate and initialize walreceiver-related shared memory */
void
WalRcvShmemInit(void)
{
	bool		found;

	WalRcv = (WalRcvData *)
		ShmemInitStruct("Wal Receiver Ctl", WalRcvShmemSize(), &found);

	if (!found)
	{
		/* First time through, so initialize */
		MemSet(WalRcv, 0, WalRcvShmemSize());
		WalRcv->walRcvState = WALRCV_STOPPED;
		SpinLockInit(&WalRcv->mutex);
		InitSharedLatch(&WalRcv->latch);
	}
}

/* Is walreceiver running (or starting up)? */
bool
WalRcvRunning(void)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;
	WalRcvState state;
	pg_time_t	startTime;

	SpinLockAcquire(&walrcv->mutex);

	state = walrcv->walRcvState;
	startTime = walrcv->startTime;

	SpinLockRelease(&walrcv->mutex);

	/*
	 * If it has taken too long for walreceiver to start up, give up. Setting
	 * the state to STOPPED ensures that if walreceiver later does start up
	 * after all, it will see that it's not supposed to be running and die
	 * without doing anything.
	 */
	if (state == WALRCV_STARTING)
	{
		pg_time_t	now = (pg_time_t) time(NULL);

		if ((now - startTime) > WALRCV_STARTUP_TIMEOUT)
		{
			SpinLockAcquire(&walrcv->mutex);

			if (walrcv->walRcvState == WALRCV_STARTING)
				state = walrcv->walRcvState = WALRCV_STOPPED;

			SpinLockRelease(&walrcv->mutex);
		}
	}

	if (state != WALRCV_STOPPED)
		return true;
	else
		return false;
}

/*
 * Is walreceiver running and streaming (or at least attempting to connect,
 * or starting up)?
 */
bool
WalRcvStreaming(void)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;
	WalRcvState state;
	pg_time_t	startTime;

	SpinLockAcquire(&walrcv->mutex);

	state = walrcv->walRcvState;
	startTime = walrcv->startTime;

	SpinLockRelease(&walrcv->mutex);

	/*
	 * If it has taken too long for walreceiver to start up, give up. Setting
	 * the state to STOPPED ensures that if walreceiver later does start up
	 * after all, it will see that it's not supposed to be running and die
	 * without doing anything.
	 */
	if (state == WALRCV_STARTING)
	{
		pg_time_t	now = (pg_time_t) time(NULL);

		if ((now - startTime) > WALRCV_STARTUP_TIMEOUT)
		{
			SpinLockAcquire(&walrcv->mutex);

			if (walrcv->walRcvState == WALRCV_STARTING)
				state = walrcv->walRcvState = WALRCV_STOPPED;

			SpinLockRelease(&walrcv->mutex);
		}
	}

	if (state == WALRCV_STREAMING || state == WALRCV_STARTING ||
		state == WALRCV_RESTARTING)
		return true;
	else
		return false;
}

/*
 * Stop walreceiver (if running) and wait for it to die.
 * Executed by the Startup process.
 */
void
ShutdownWalRcv(void)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;
	pid_t		walrcvpid = 0;

	/*
	 * Request walreceiver to stop. Walreceiver will switch to WALRCV_STOPPED
	 * mode once it's finished, and will also request postmaster to not
	 * restart itself.
	 */
	SpinLockAcquire(&walrcv->mutex);
	switch (walrcv->walRcvState)
	{
		case WALRCV_STOPPED:
			break;
		case WALRCV_STARTING:
			walrcv->walRcvState = WALRCV_STOPPED;
			break;

		case WALRCV_STREAMING:
		case WALRCV_WAITING:
		case WALRCV_RESTARTING:
			walrcv->walRcvState = WALRCV_STOPPING;
			/* fall through */
		case WALRCV_STOPPING:
			walrcvpid = walrcv->pid;
			break;
	}
	SpinLockRelease(&walrcv->mutex);

	/*
	 * Signal walreceiver process if it was still running.
	 */
	if (walrcvpid != 0)
		kill(walrcvpid, SIGTERM);

	/*
	 * Wait for walreceiver to acknowledge its death by setting state to
	 * WALRCV_STOPPED.
	 */
	while (WalRcvRunning())
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
 * Request postmaster to start walreceiver.
 *
 * recptr indicates the position where streaming should begin, conninfo
 * is a libpq connection string to use, and slotname is, optionally, the name
 * of a replication slot to acquire.
 */
void
RequestXLogStreaming(TimeLineID tli, XLogRecPtr recptr, const char *conninfo,
					 const char *slotname)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;
	bool		launch = false;
	pg_time_t	now = (pg_time_t) time(NULL);

	/*
	 * We always start at the beginning of the segment. That prevents a broken
	 * segment (i.e., with no records in the first half of a segment) from
	 * being created by XLOG streaming, which might cause trouble later on if
	 * the segment is e.g archived.
	 */
	if (recptr % XLogSegSize != 0)
		recptr -= recptr % XLogSegSize;

	SpinLockAcquire(&walrcv->mutex);

	/* It better be stopped if we try to restart it */
	Assert(walrcv->walRcvState == WALRCV_STOPPED ||
		   walrcv->walRcvState == WALRCV_WAITING);

	if (conninfo != NULL)
		strlcpy((char *) walrcv->conninfo, conninfo, MAXCONNINFO);
	else
		walrcv->conninfo[0] = '\0';

	if (slotname != NULL)
		strlcpy((char *) walrcv->slotname, slotname, NAMEDATALEN);
	else
		walrcv->slotname[0] = '\0';

	if (walrcv->walRcvState == WALRCV_STOPPED)
	{
		launch = true;
		walrcv->walRcvState = WALRCV_STARTING;
	}
	else
		walrcv->walRcvState = WALRCV_RESTARTING;
	walrcv->startTime = now;

	/*
	 * If this is the first startup of walreceiver (on this timeline),
	 * initialize receivedUpto and latestChunkStart to the starting point.
	 */
	if (walrcv->receiveStart == 0 || walrcv->receivedTLI != tli)
	{
		walrcv->receivedUpto = recptr;
		walrcv->receivedTLI = tli;
		walrcv->latestChunkStart = recptr;
	}
	walrcv->receiveStart = recptr;
	walrcv->receiveStartTLI = tli;

	SpinLockRelease(&walrcv->mutex);

	if (launch)
		SendPostmasterSignal(PMSIGNAL_START_WALRECEIVER);
	else
		SetLatch(&walrcv->latch);
}

/*
 * Returns the last+1 byte position that walreceiver has written.
 *
 * Optionally, returns the previous chunk start, that is the first byte
 * written in the most recent walreceiver flush cycle.  Callers not
 * interested in that value may pass NULL for latestChunkStart. Same for
 * receiveTLI.
 */
XLogRecPtr
GetWalRcvWriteRecPtr(XLogRecPtr *latestChunkStart, TimeLineID *receiveTLI)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;
	XLogRecPtr	recptr;

	SpinLockAcquire(&walrcv->mutex);
	recptr = walrcv->receivedUpto;
	if (latestChunkStart)
		*latestChunkStart = walrcv->latestChunkStart;
	if (receiveTLI)
		*receiveTLI = walrcv->receivedTLI;
	SpinLockRelease(&walrcv->mutex);

	return recptr;
}

/*
 * Returns the replication apply delay in ms or -1
 * if the apply delay info is not available
 */
int
GetReplicationApplyDelay(void)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;

	XLogRecPtr	receivePtr;
	XLogRecPtr	replayPtr;

	long		secs;
	int			usecs;

	TimestampTz chunckReplayStartTime;

	SpinLockAcquire(&walrcv->mutex);
	receivePtr = walrcv->receivedUpto;
	SpinLockRelease(&walrcv->mutex);

	replayPtr = GetXLogReplayRecPtr(NULL);

	if (receivePtr == replayPtr)
		return 0;

	chunckReplayStartTime = GetCurrentChunkReplayStartTime();

	if (chunckReplayStartTime == 0)
		return -1;

	TimestampDifference(chunckReplayStartTime,
						GetCurrentTimestamp(),
						&secs, &usecs);

	return (((int) secs * 1000) + (usecs / 1000));
}

/*
 * Returns the network latency in ms, note that this includes any
 * difference in clock settings between the servers, as well as timezone.
 */
int
GetReplicationTransferLatency(void)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile WalRcvData *walrcv = WalRcv;

	TimestampTz lastMsgSendTime;
	TimestampTz lastMsgReceiptTime;

	long		secs = 0;
	int			usecs = 0;
	int			ms;

	SpinLockAcquire(&walrcv->mutex);
	lastMsgSendTime = walrcv->lastMsgSendTime;
	lastMsgReceiptTime = walrcv->lastMsgReceiptTime;
	SpinLockRelease(&walrcv->mutex);

	TimestampDifference(lastMsgSendTime,
						lastMsgReceiptTime,
						&secs, &usecs);

	ms = ((int) secs * 1000) + (usecs / 1000);

	return ms;
}
