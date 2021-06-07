/*-------------------------------------------------------------------------
 *
 * zenith_wal_redo.c
 *	  Entry point for WAL redo helper
 *
 *
 * This file contains an alternative main() function for the 'postgres'
 * binary. In the special mode, we go into a special mode that's similar
 * to the single user mode. We don't launch postmaster or any auxiliary
 * processes. Instead, we wait for command from 'stdin', and respond to
 * 'stdout'.
 *
 * There's a TAP test for this in contrib/zenith_store/t/002_wal_redo_helper.pl
 *
 * The protocol through stdin/stdout is loosely based on the libpq protocol.
 * The process accepts messages through stdin, and each message has the format:
 *
 * char   msgtype;
 * int32  length; // length of message including 'length' but excluding
 *                // 'msgtype', in network byte order
 * <payload>
 *
 * There are three message types:
 *
 * BeginRedoForBlock ('B'): Prepare for WAL replay for given block
 * PushPage ('P'): Copy a page image (in the payload) to buffer cache
 * ApplyRecord ('A'): Apply a WAL record (in the payload)
 * GetPage ('G'): Return a page image from buffer cache.
 *
 * Currently, you only get a response to GetPage requests; the response is
 * simply a 8k page, without any headers. Errors are logged to stderr.
 *
 * FIXME:
 * - this currently requires a valid PGDATA, and creates a lock file there
 *   like a normal postmaster. There's no fundamental reason for that, though.
 * - should have EndRedoForBlock, and flush page cache, to allow using this
 *   mechanism for more than one block without restarting the process.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/zenith_wal_redo.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/resource.h>
#endif

#ifndef HAVE_GETRUSAGE
#include "rusagestub.h"
#endif

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"

static int	ReadRedoCommand(StringInfo inBuf);
static void BeginRedoForBlock(StringInfo input_message);
static void PushPage(StringInfo input_message);
static void ApplyRecord(StringInfo input_message);
static bool redo_block_filter(XLogReaderState *record, uint8 block_id);
static void GetPage(StringInfo input_message);

static BufferTag target_redo_tag;

#define TRACE DEBUG5

/* ----------------------------------------------------------------
 * FIXME comment
 * PostgresMain
 *	   postgres main loop -- all backends, interactive or otherwise start here
 *
 * argc/argv are the command line arguments to be used.  (When being forked
 * by the postmaster, these are not the original argv array of the process.)
 * dbname is the name of the database to connect to, or NULL if the database
 * name should be extracted from the command line arguments or defaulted.
 * username is the PostgreSQL user name to be used for the session.
 * ----------------------------------------------------------------
 */
void
WalRedoMain(int argc, char *argv[],
			const char *dbname,
			const char *username)
{
	int			firstchar;
	StringInfoData input_message;

	/* Initialize startup process environment if necessary. */
	InitStandaloneProcess(argv[0]);

	SetProcessingMode(InitProcessing);

	/*
	 * Set default values for command-line options.
	 */
	InitializeGUCOptions();

	/*
	 * Parse command-line options.
	 * TODO
	 */
	//process_postgres_switches(argc, argv, PGC_POSTMASTER, &dbname);

	/* Acquire configuration parameters */
	if (!SelectConfigFiles(NULL, progname))
		proc_exit(1);

	/*
	 * Set up signal handlers.  (InitPostmasterChild or InitStandaloneProcess
	 * has already set up BlockSig and made that the active signal mask.)
	 *
	 * Note that postmaster blocked all signals before forking child process,
	 * so there is no race condition whereby we might receive a signal before
	 * we have set up the handler.
	 *
	 * Also note: it's best not to use any signals that are SIG_IGNored in the
	 * postmaster.  If such a signal arrives before we are able to change the
	 * handler to non-SIG_IGN, it'll get dropped.  Instead, make a dummy
	 * handler in the postmaster to reserve the signal. (Of course, this isn't
	 * an issue for signals that are locally generated, such as SIGALRM and
	 * SIGPIPE.)
	 */
#if 0
	if (am_walsender)
		WalSndSignals();
	else
	{
		pqsignal(SIGHUP, SignalHandlerForConfigReload);
		pqsignal(SIGINT, StatementCancelHandler);	/* cancel current query */
		pqsignal(SIGTERM, die); /* cancel current query and exit */

		/*
		 * In a postmaster child backend, replace SignalHandlerForCrashExit
		 * with quickdie, so we can tell the client we're dying.
		 *
		 * In a standalone backend, SIGQUIT can be generated from the keyboard
		 * easily, while SIGTERM cannot, so we make both signals do die()
		 * rather than quickdie().
		 */
		if (IsUnderPostmaster)
			pqsignal(SIGQUIT, quickdie);	/* hard crash time */
		else
			pqsignal(SIGQUIT, die); /* cancel current query and exit */
		InitializeTimeouts();	/* establishes SIGALRM handler */

		/*
		 * Ignore failure to write to frontend. Note: if frontend closes
		 * connection, we will notice it and exit cleanly when control next
		 * returns to outer loop.  This seems safer than forcing exit in the
		 * midst of output during who-knows-what operation...
		 */
		pqsignal(SIGPIPE, SIG_IGN);
		pqsignal(SIGUSR1, procsignal_sigusr1_handler);
		pqsignal(SIGUSR2, SIG_IGN);
		pqsignal(SIGFPE, FloatExceptionHandler);

		/*
		 * Reset some signals that are accepted by postmaster but not by
		 * backend
		 */
		pqsignal(SIGCHLD, SIG_DFL); /* system() requires this on some
									 * platforms */
	}
#endif

	/*
	 * Validate we have been given a reasonable-looking DataDir and change into it.
	 */
	checkDataDir();
	ChangeToDataDir();

	/*
	 * Create lockfile for data directory.
	 */
	CreateDataDirLockFile(false);

	/* read control file (error checking and contains config ) */
	LocalProcessControlFile(false);

	process_shared_preload_libraries();

	/* Initialize MaxBackends (if under postmaster, was done already) */
	InitializeMaxBackends();

	/* Early initialization */
	BaseInit();

	/*
	 * Create a per-backend PGPROC struct in shared memory. We must do
	 * this before we can use LWLocks.
	 */
	InitAuxiliaryProcess();

	SetProcessingMode(NormalProcessing);

	/* Redo routines won't work if we're not "in recovery" */
	InRecovery = true;

	/*
	 * Create the memory context we will use in the main loop.
	 *
	 * MessageContext is reset once per iteration of the main loop, ie, upon
	 * completion of processing of each command message from the client.
	 */
	MessageContext = AllocSetContextCreate(TopMemoryContext,
										   "MessageContext",
										   ALLOCSET_DEFAULT_SIZES);

	/* we need a ResourceOwner to hold buffer pins */
	Assert(CurrentResourceOwner == NULL);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "wal redo");

	/* Initialize resource managers */
	for (int rmid = 0; rmid <= RM_MAX_ID; rmid++)
	{
		if (RmgrTable[rmid].rm_startup != NULL)
			RmgrTable[rmid].rm_startup();
	}

	/*
	 * Main processing loop
	 */
	for (;;)
	{
		/*
		 * Release storage left over from prior query cycle, and create a new
		 * query input buffer in the cleared MessageContext.
		 */
		MemoryContextSwitchTo(MessageContext);
		MemoryContextResetAndDeleteChildren(MessageContext);

		initStringInfo(&input_message);

		set_ps_display("idle");

		/*
		 * (3) read a command (loop blocks here)
		 */
		firstchar = ReadRedoCommand(&input_message);

		switch (firstchar)
		{
			case 'B':			/* BeginRedoForBlock */
				BeginRedoForBlock(&input_message);
				break;

			case 'P':			/* PushPage */
				PushPage(&input_message);
				break;

			case 'A':			/* ApplyRecord */
				ApplyRecord(&input_message);
				break;

			case 'G':			/* GetPage */
				GetPage(&input_message);
				break;

				/*
				 * EOF means we're done. Perform normal shutdown.
				 */
			case EOF:

				/*
				 * NOTE: if you are tempted to add more code here, DON'T!
				 * Whatever you had in mind to do should be set up as an
				 * on_proc_exit or on_shmem_exit callback, instead. Otherwise
				 * it will fail to be called during other backend-shutdown
				 * scenarios.
				 */
				proc_exit(0);

			default:
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid frontend message type %d",
								firstchar)));
		}
	}							/* end of input-reading loop */
}

/*
 * Some debug function that may be handy for now.
 */
pg_attribute_unused()
static char *
pprint_buffer(char *data, int len)
{
	StringInfoData s;
	initStringInfo(&s);
	appendStringInfo(&s, "\n");
	for (int i = 0; i < len; i++) {

		appendStringInfo(&s, "%02x ", (*(((char *) data) + i) & 0xff) );
		if (i % 32 == 31) {
			appendStringInfo(&s, "\n");
		}
	}
	appendStringInfo(&s, "\n");

	return s.data;
}

static char *
pprint_tag(BufferTag *tag)
{
	StringInfoData s;

	initStringInfo(&s);

	appendStringInfo(&s, "%u/%u/%u.%d blk %u",
		tag->rnode.spcNode,
		tag->rnode.dbNode,
		tag->rnode.relNode,
		tag->forkNum,
		tag->blockNum
	);

	return s.data;
}
/* ----------------------------------------------------------------
 *		routines to obtain user input
 * ----------------------------------------------------------------
 */

/*
 * Read next command from the client.
 *
 *	the string entered by the user is placed in its parameter inBuf,
 *	and we act like a Q message was received.
 *
 *	EOF is returned if end-of-file input is seen; time to shut down.
 * ----------------
 */

/*
 * Wait until there is data in stdin. Prints a log message every 10 s whil
 * waiting.
 */
static void
wait_with_timeout(void)
{
	for (;;)
	{
		struct timeval timeout = {10, 0};
		fd_set		fds;
		int			ret;

		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);

		ret = select(1, &fds, NULL, NULL, &timeout);
		if (ret != 0)
			break;
		elog(DEBUG1, "still alive");
	}
}

static int
ReadRedoCommand(StringInfo inBuf)
{
	char		c;
	int			qtype;
	int32		len;
	int			nread;

	/* FIXME: Use unbuffered I/O here, because the WAL redo process was getting
	 * stuck with buffered I/O. I'm not sure why, or whether the bug was somewhere
	 * in here or in the calling page server side.
	 */
	wait_with_timeout();
	if (read(STDIN_FILENO, &c, 1) == 0)
		return EOF;
	qtype = c;

	/*
	 * Like in the FE/BE protocol, all messages have a length word next
	 * after the type code; we can read the message contents independently of
	 * the type.
	 */
	if (read(STDIN_FILENO, &len, 4) != 4)
	{
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("could not read message length")));
	}

	len = pg_ntoh32(len);

	if (len < 4)
	{
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid message length")));
		return EOF;
	}

	len -= 4;					/* discount length itself */

	enlargeStringInfo(inBuf, len);
	nread = 0;
	while (nread < len) {
		int n = read(STDIN_FILENO, inBuf->data + nread, len - nread);
		if (n == -1)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("read error: %m")));
		if (n == 0)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("unexpected EOF")));
		nread += n;
	}
	inBuf->len = len;
	inBuf->data[len] = '\0';

	return qtype;
}


/*
 * Prepare for WAL replay on given block
 */
static void
BeginRedoForBlock(StringInfo input_message)
{
	RelFileNode rnode;
	ForkNumber forknum;
	BlockNumber blknum;
	MemoryContext oldcxt;
	SMgrRelation reln;

	/*
	 * message format:
	 *
	 * spcNode
	 * dbNode
	 * relNode
	 * ForkNumber
	 * BlockNumber
	 */
	forknum = pq_getmsgbyte(input_message);
	rnode.spcNode = pq_getmsgint(input_message, 4);
	rnode.dbNode = pq_getmsgint(input_message, 4);
	rnode.relNode = pq_getmsgint(input_message, 4);
	blknum = pq_getmsgint(input_message, 4);

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	INIT_BUFFERTAG(target_redo_tag, rnode, forknum, blknum);

	{
		char* buf = pprint_tag(&target_redo_tag);
		elog(TRACE, "BeginRedoForBlock %s", buf);
		pfree(buf);
	}

	MemoryContextSwitchTo(oldcxt);

	reln = smgropen(rnode, InvalidBackendId);
	if (reln->smgr_cached_nblocks[forknum] == InvalidBlockNumber ||
		reln->smgr_cached_nblocks[forknum] < blknum + 1)
	{
		reln->smgr_cached_nblocks[forknum] = blknum + 1;
	}
}

/*
 * Receive a page given by the client, and put it into buffer cache.
 */
static void
PushPage(StringInfo input_message)
{
	RelFileNode rnode;
	ForkNumber forknum;
	BlockNumber blknum;
	const char *content;
	Buffer		buf;
	Page		page;

	/*
	 * message format:
	 *
	 * spcNode
	 * dbNode
	 * relNode
	 * ForkNumber
	 * BlockNumber
	 * 8k page content
	 */
	forknum = pq_getmsgbyte(input_message);
	rnode.spcNode = pq_getmsgint(input_message, 4);
	rnode.dbNode = pq_getmsgint(input_message, 4);
	rnode.relNode = pq_getmsgint(input_message, 4);
	blknum = pq_getmsgint(input_message, 4);
	content = pq_getmsgbytes(input_message, BLCKSZ);

	buf = ReadBufferWithoutRelcache(rnode, forknum, blknum, RBM_ZERO_AND_LOCK, NULL);
	page = BufferGetPage(buf);
	memcpy(page, content, BLCKSZ);
	MarkBufferDirty(buf); /* pro forma */
	UnlockReleaseBuffer(buf);
}

/*
 * Receive a WAL record, and apply it.
 *
 * All the pages should be loaded into the buffer cache by PushPage calls already.
 */
static void
ApplyRecord(StringInfo input_message)
{
	/* recovery here */
	char	   *errormsg;
	XLogRecPtr	lsn;
	XLogRecord *record;
	int			nleft;
	XLogReaderState reader_state;

	/*
	 * message format:
	 *
	 * LSN (the *end* of the record)
	 * record
	 */
	lsn = pq_getmsgint64(input_message);

	/* note: the input must be aligned here */
	record = (XLogRecord *) pq_getmsgbytes(input_message, sizeof(XLogRecord));

	nleft = input_message->len - input_message->cursor;
	if (record->xl_tot_len != sizeof(XLogRecord) + nleft)
		elog(ERROR, "mismatch between record (%d) and message size (%d)",
			 record->xl_tot_len, (int) sizeof(XLogRecord) + nleft);

	/* FIXME: use XLogReaderAllocate() */
	memset(&reader_state, 0, sizeof(XLogReaderState));
	reader_state.ReadRecPtr = 0; /* no 'prev' record */
	reader_state.EndRecPtr = lsn; /* this record */
	reader_state.decoded_record = record;
	reader_state.errormsg_buf = palloc(1000 + 1); /* MAX_ERRORMSG_LEN */

	if (!DecodeXLogRecord(&reader_state, record, &errormsg))
		elog(ERROR, "failed to decode WAL record: %s", errormsg);

	/* Ignore any other blocks than the ones the caller is interested in */
	redo_read_buffer_filter = redo_block_filter;

	RmgrTable[record->xl_rmid].rm_redo(&reader_state);

	redo_read_buffer_filter = NULL;

	elog(TRACE, "applied WAL record with LSN %X/%X",
		 (uint32) (lsn >> 32), (uint32) lsn);
}

static bool
redo_block_filter(XLogReaderState *record, uint8 block_id)
{
	BufferTag	target_tag;

	if (!XLogRecGetBlockTag(record, block_id,
							&target_tag.rnode, &target_tag.forkNum, &target_tag.blockNum))
	{
		/* Caller specified a bogus block_id */
		elog(PANIC, "failed to locate backup block with ID %d", block_id);
	}

	/*
	 * If this block isn't one we are currently restoring, then return 'true'
	 * so that this gets ignored
	 */
	return !BUFFERTAGS_EQUAL(target_tag, target_redo_tag);
}

/*
 * Get a page image back from buffer cache.
 *
 * After applying some records.
 */
static void
GetPage(StringInfo input_message)
{
	RelFileNode rnode;
	ForkNumber forknum;
	BlockNumber blknum;
	Buffer		buf;
	Page		page;

	/*
	 * message format:
	 *
	 * spcNode
	 * dbNode
	 * relNode
	 * ForkNumber
	 * BlockNumber
	 */
	forknum = pq_getmsgbyte(input_message);
	rnode.spcNode = pq_getmsgint(input_message, 4);
	rnode.dbNode = pq_getmsgint(input_message, 4);
	rnode.relNode = pq_getmsgint(input_message, 4);
	blknum = pq_getmsgint(input_message, 4);

	/* FIXME: check that we got a BeginRedoForBlock message or this earlier */

	buf = ReadBufferWithoutRelcache(rnode, forknum, blknum, RBM_NORMAL, NULL);
	page = BufferGetPage(buf);
	/* single thread, so don't bother locking the page */

	/* Response: Page content */
	fwrite(page, 1, BLCKSZ, stdout); /* FIXME: check errors */
	fflush(stdout);

	ReleaseBuffer(buf);
	DropDatabaseBuffers(rnode.dbNode);
	smgrinit(); //reset inmem smgr state

	elog(TRACE, "Page sent back for block %u", blknum);
}
