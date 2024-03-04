/*-------------------------------------------------------------------------
 * auxprocess.c
 *	  functions related to auxiliary processes.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/auxprocess.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <signal.h>

#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/auxprocess.h"
#include "postmaster/bgwriter.h"
#include "postmaster/startup.h"
#include "postmaster/walsummarizer.h"
#include "postmaster/walwriter.h"
#include "replication/walreceiver.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "utils/ps_status.h"


static void ShutdownAuxiliaryProcess(int code, Datum arg);


/*
 *	 AuxiliaryProcessMain
 *
 *	 The main entry point for auxiliary processes, such as the bgwriter,
 *	 walwriter, walreceiver, bootstrapper and the shared memory checker code.
 *
 *	 This code is here just because of historical reasons.
 */
void
AuxiliaryProcessMain(BackendType auxtype)
{
	Assert(IsUnderPostmaster);

	MyBackendType = auxtype;

	init_ps_display(NULL);

	SetProcessingMode(BootstrapProcessing);
	IgnoreSystemIndexes = true;

	/*
	 * As an auxiliary process, we aren't going to do the full InitPostgres
	 * pushups, but there are a couple of things that need to get lit up even
	 * in an auxiliary process.
	 */

	/*
	 * Create a PGPROC so we can use LWLocks and access shared memory.
	 */
	InitAuxiliaryProcess();

	BaseInit();

	ProcSignalInit();

	/*
	 * Auxiliary processes don't run transactions, but they may need a
	 * resource owner anyway to manage buffer pins acquired outside
	 * transactions (and, perhaps, other things in future).
	 */
	CreateAuxProcessResourceOwner();


	/* Initialize backend status information */
	pgstat_beinit();
	pgstat_bestart();

	/* register a before-shutdown callback for LWLock cleanup */
	before_shmem_exit(ShutdownAuxiliaryProcess, 0);

	SetProcessingMode(NormalProcessing);

	switch (MyBackendType)
	{
		case B_STARTUP:
			StartupProcessMain();
			proc_exit(1);

		case B_ARCHIVER:
			PgArchiverMain();
			proc_exit(1);

		case B_BG_WRITER:
			BackgroundWriterMain();
			proc_exit(1);

		case B_CHECKPOINTER:
			CheckpointerMain();
			proc_exit(1);

		case B_WAL_WRITER:
			WalWriterMain();
			proc_exit(1);

		case B_WAL_RECEIVER:
			WalReceiverMain();
			proc_exit(1);

		case B_WAL_SUMMARIZER:
			WalSummarizerMain();
			proc_exit(1);

		default:
			elog(PANIC, "unrecognized process type: %d", (int) MyBackendType);
			proc_exit(1);
	}
}

/*
 * Begin shutdown of an auxiliary process.  This is approximately the equivalent
 * of ShutdownPostgres() in postinit.c.  We can't run transactions in an
 * auxiliary process, so most of the work of AbortTransaction() is not needed,
 * but we do need to make sure we've released any LWLocks we are holding.
 * (This is only critical during an error exit.)
 */
static void
ShutdownAuxiliaryProcess(int code, Datum arg)
{
	LWLockReleaseAll();
	ConditionVariableCancelSleep();
	pgstat_report_wait_end();
}
