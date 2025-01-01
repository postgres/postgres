/*-------------------------------------------------------------------------
 *
 * pqmq.c
 *	  Use the frontend/backend protocol for communication over a shm_mq
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/libpq/pqmq.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/parallel.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqmq.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/logicalworker.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"

static shm_mq_handle *pq_mq_handle;
static bool pq_mq_busy = false;
static pid_t pq_mq_parallel_leader_pid = 0;
static ProcNumber pq_mq_parallel_leader_proc_number = INVALID_PROC_NUMBER;

static void pq_cleanup_redirect_to_shm_mq(dsm_segment *seg, Datum arg);
static void mq_comm_reset(void);
static int	mq_flush(void);
static int	mq_flush_if_writable(void);
static bool mq_is_send_pending(void);
static int	mq_putmessage(char msgtype, const char *s, size_t len);
static void mq_putmessage_noblock(char msgtype, const char *s, size_t len);

static const PQcommMethods PqCommMqMethods = {
	.comm_reset = mq_comm_reset,
	.flush = mq_flush,
	.flush_if_writable = mq_flush_if_writable,
	.is_send_pending = mq_is_send_pending,
	.putmessage = mq_putmessage,
	.putmessage_noblock = mq_putmessage_noblock
};

/*
 * Arrange to redirect frontend/backend protocol messages to a shared-memory
 * message queue.
 */
void
pq_redirect_to_shm_mq(dsm_segment *seg, shm_mq_handle *mqh)
{
	PqCommMethods = &PqCommMqMethods;
	pq_mq_handle = mqh;
	whereToSendOutput = DestRemote;
	FrontendProtocol = PG_PROTOCOL_LATEST;
	on_dsm_detach(seg, pq_cleanup_redirect_to_shm_mq, (Datum) 0);
}

/*
 * When the DSM that contains our shm_mq goes away, we need to stop sending
 * messages to it.
 */
static void
pq_cleanup_redirect_to_shm_mq(dsm_segment *seg, Datum arg)
{
	pq_mq_handle = NULL;
	whereToSendOutput = DestNone;
}

/*
 * Arrange to SendProcSignal() to the parallel leader each time we transmit
 * message data via the shm_mq.
 */
void
pq_set_parallel_leader(pid_t pid, ProcNumber procNumber)
{
	Assert(PqCommMethods == &PqCommMqMethods);
	pq_mq_parallel_leader_pid = pid;
	pq_mq_parallel_leader_proc_number = procNumber;
}

static void
mq_comm_reset(void)
{
	/* Nothing to do. */
}

static int
mq_flush(void)
{
	/* Nothing to do. */
	return 0;
}

static int
mq_flush_if_writable(void)
{
	/* Nothing to do. */
	return 0;
}

static bool
mq_is_send_pending(void)
{
	/* There's never anything pending. */
	return 0;
}

/*
 * Transmit a libpq protocol message to the shared memory message queue
 * selected via pq_mq_handle.  We don't include a length word, because the
 * receiver will know the length of the message from shm_mq_receive().
 */
static int
mq_putmessage(char msgtype, const char *s, size_t len)
{
	shm_mq_iovec iov[2];
	shm_mq_result result;

	/*
	 * If we're sending a message, and we have to wait because the queue is
	 * full, and then we get interrupted, and that interrupt results in trying
	 * to send another message, we respond by detaching the queue.  There's no
	 * way to return to the original context, but even if there were, just
	 * queueing the message would amount to indefinitely postponing the
	 * response to the interrupt.  So we do this instead.
	 */
	if (pq_mq_busy)
	{
		if (pq_mq_handle != NULL)
			shm_mq_detach(pq_mq_handle);
		pq_mq_handle = NULL;
		return EOF;
	}

	/*
	 * If the message queue is already gone, just ignore the message. This
	 * doesn't necessarily indicate a problem; for example, DEBUG messages can
	 * be generated late in the shutdown sequence, after all DSMs have already
	 * been detached.
	 */
	if (pq_mq_handle == NULL)
		return 0;

	pq_mq_busy = true;

	iov[0].data = &msgtype;
	iov[0].len = 1;
	iov[1].data = s;
	iov[1].len = len;

	Assert(pq_mq_handle != NULL);

	for (;;)
	{
		/*
		 * Immediately notify the receiver by passing force_flush as true so
		 * that the shared memory value is updated before we send the parallel
		 * message signal right after this.
		 */
		result = shm_mq_sendv(pq_mq_handle, iov, 2, true, true);

		if (pq_mq_parallel_leader_pid != 0)
		{
			if (IsLogicalParallelApplyWorker())
				SendProcSignal(pq_mq_parallel_leader_pid,
							   PROCSIG_PARALLEL_APPLY_MESSAGE,
							   pq_mq_parallel_leader_proc_number);
			else
			{
				Assert(IsParallelWorker());
				SendProcSignal(pq_mq_parallel_leader_pid,
							   PROCSIG_PARALLEL_MESSAGE,
							   pq_mq_parallel_leader_proc_number);
			}
		}

		if (result != SHM_MQ_WOULD_BLOCK)
			break;

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
						 WAIT_EVENT_MESSAGE_QUEUE_PUT_MESSAGE);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}

	pq_mq_busy = false;

	Assert(result == SHM_MQ_SUCCESS || result == SHM_MQ_DETACHED);
	if (result != SHM_MQ_SUCCESS)
		return EOF;
	return 0;
}

static void
mq_putmessage_noblock(char msgtype, const char *s, size_t len)
{
	/*
	 * While the shm_mq machinery does support sending a message in
	 * non-blocking mode, there's currently no way to try sending beginning to
	 * send the message that doesn't also commit us to completing the
	 * transmission.  This could be improved in the future, but for now we
	 * don't need it.
	 */
	elog(ERROR, "not currently supported");
}

/*
 * Parse an ErrorResponse or NoticeResponse payload and populate an ErrorData
 * structure with the results.
 */
void
pq_parse_errornotice(StringInfo msg, ErrorData *edata)
{
	/* Initialize edata with reasonable defaults. */
	MemSet(edata, 0, sizeof(ErrorData));
	edata->elevel = ERROR;
	edata->assoc_context = CurrentMemoryContext;

	/* Loop over fields and extract each one. */
	for (;;)
	{
		char		code = pq_getmsgbyte(msg);
		const char *value;

		if (code == '\0')
		{
			pq_getmsgend(msg);
			break;
		}
		value = pq_getmsgrawstring(msg);

		switch (code)
		{
			case PG_DIAG_SEVERITY:
				/* ignore, trusting we'll get a nonlocalized version */
				break;
			case PG_DIAG_SEVERITY_NONLOCALIZED:
				if (strcmp(value, "DEBUG") == 0)
				{
					/*
					 * We can't reconstruct the exact DEBUG level, but
					 * presumably it was >= client_min_messages, so select
					 * DEBUG1 to ensure we'll pass it on to the client.
					 */
					edata->elevel = DEBUG1;
				}
				else if (strcmp(value, "LOG") == 0)
				{
					/*
					 * It can't be LOG_SERVER_ONLY, or the worker wouldn't
					 * have sent it to us; so LOG is the correct value.
					 */
					edata->elevel = LOG;
				}
				else if (strcmp(value, "INFO") == 0)
					edata->elevel = INFO;
				else if (strcmp(value, "NOTICE") == 0)
					edata->elevel = NOTICE;
				else if (strcmp(value, "WARNING") == 0)
					edata->elevel = WARNING;
				else if (strcmp(value, "ERROR") == 0)
					edata->elevel = ERROR;
				else if (strcmp(value, "FATAL") == 0)
					edata->elevel = FATAL;
				else if (strcmp(value, "PANIC") == 0)
					edata->elevel = PANIC;
				else
					elog(ERROR, "unrecognized error severity: \"%s\"", value);
				break;
			case PG_DIAG_SQLSTATE:
				if (strlen(value) != 5)
					elog(ERROR, "invalid SQLSTATE: \"%s\"", value);
				edata->sqlerrcode = MAKE_SQLSTATE(value[0], value[1], value[2],
												  value[3], value[4]);
				break;
			case PG_DIAG_MESSAGE_PRIMARY:
				edata->message = pstrdup(value);
				break;
			case PG_DIAG_MESSAGE_DETAIL:
				edata->detail = pstrdup(value);
				break;
			case PG_DIAG_MESSAGE_HINT:
				edata->hint = pstrdup(value);
				break;
			case PG_DIAG_STATEMENT_POSITION:
				edata->cursorpos = pg_strtoint32(value);
				break;
			case PG_DIAG_INTERNAL_POSITION:
				edata->internalpos = pg_strtoint32(value);
				break;
			case PG_DIAG_INTERNAL_QUERY:
				edata->internalquery = pstrdup(value);
				break;
			case PG_DIAG_CONTEXT:
				edata->context = pstrdup(value);
				break;
			case PG_DIAG_SCHEMA_NAME:
				edata->schema_name = pstrdup(value);
				break;
			case PG_DIAG_TABLE_NAME:
				edata->table_name = pstrdup(value);
				break;
			case PG_DIAG_COLUMN_NAME:
				edata->column_name = pstrdup(value);
				break;
			case PG_DIAG_DATATYPE_NAME:
				edata->datatype_name = pstrdup(value);
				break;
			case PG_DIAG_CONSTRAINT_NAME:
				edata->constraint_name = pstrdup(value);
				break;
			case PG_DIAG_SOURCE_FILE:
				edata->filename = pstrdup(value);
				break;
			case PG_DIAG_SOURCE_LINE:
				edata->lineno = pg_strtoint32(value);
				break;
			case PG_DIAG_SOURCE_FUNCTION:
				edata->funcname = pstrdup(value);
				break;
			default:
				elog(ERROR, "unrecognized error field code: %d", (int) code);
				break;
		}
	}
}
