/*-------------------------------------------------------------------------
 *
 * globals.c
 *	  global variable declarations
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/globals.c
 *
 * NOTES
 *	  Globals used all over the place should be declared here and not
 *	  in other modules.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/file_perm.h"
#include "libpq/libpq-be.h"
#include "libpq/pqcomm.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/procnumber.h"
#include "storage/procsignal.h"


ProtocolVersion FrontendProtocol;

volatile sig_atomic_t InterruptPending = false;
volatile sig_atomic_t QueryCancelPending = false;
volatile sig_atomic_t ProcDiePending = false;
volatile sig_atomic_t CheckClientConnectionPending = false;
volatile sig_atomic_t ClientConnectionLost = false;
volatile sig_atomic_t IdleInTransactionSessionTimeoutPending = false;
volatile sig_atomic_t TransactionTimeoutPending = false;
volatile sig_atomic_t IdleSessionTimeoutPending = false;
volatile sig_atomic_t ProcSignalBarrierPending = false;
volatile sig_atomic_t LogMemoryContextPending = false;
volatile sig_atomic_t IdleStatsUpdateTimeoutPending = false;
volatile uint32 InterruptHoldoffCount = 0;
volatile uint32 QueryCancelHoldoffCount = 0;
volatile uint32 CritSectionCount = 0;

int			MyProcPid;
pg_time_t	MyStartTime;
TimestampTz MyStartTimestamp;
struct ClientSocket *MyClientSocket;
struct Port *MyProcPort;
uint8		MyCancelKey[MAX_CANCEL_KEY_LENGTH];
int			MyCancelKeyLength = 0;
int			MyPMChildSlot;

/*
 * MyLatch points to the latch that should be used for signal handling by the
 * current process. It will either point to a process local latch if the
 * current process does not have a PGPROC entry in that moment, or to
 * PGPROC->procLatch if it has. Thus it can always be used in signal handlers,
 * without checking for its existence.
 */
struct Latch *MyLatch;

/*
 * DataDir is the absolute path to the top level of the PGDATA directory tree.
 * Except during early startup, this is also the server's working directory;
 * most code therefore can simply use relative paths and not reference DataDir
 * explicitly.
 */
char	   *DataDir = NULL;

/*
 * Mode of the data directory.  The default is 0700 but it may be changed in
 * checkDataDir() to 0750 if the data directory actually has that mode.
 */
int			data_directory_mode = PG_DIR_MODE_OWNER;

char		OutputFileName[MAXPGPATH];	/* debugging output file */

char		my_exec_path[MAXPGPATH];	/* full path to my executable */
char		pkglib_path[MAXPGPATH]; /* full path to lib directory */

#ifdef EXEC_BACKEND
char		postgres_exec_path[MAXPGPATH];	/* full path to backend */

/* note: currently this is not valid in backend processes */
#endif

ProcNumber	MyProcNumber = INVALID_PROC_NUMBER;

ProcNumber	ParallelLeaderProcNumber = INVALID_PROC_NUMBER;

Oid			MyDatabaseId = InvalidOid;

Oid			MyDatabaseTableSpace = InvalidOid;

bool		MyDatabaseHasLoginEventTriggers = false;

/*
 * DatabasePath is the path (relative to DataDir) of my database's
 * primary directory, ie, its directory in the default tablespace.
 */
char	   *DatabasePath = NULL;

pid_t		PostmasterPid = 0;

/*
 * IsPostmasterEnvironment is true in a postmaster process and any postmaster
 * child process; it is false in a standalone process (bootstrap or
 * standalone backend).  IsUnderPostmaster is true in postmaster child
 * processes.  Note that "child process" includes all children, not only
 * regular backends.  These should be set correctly as early as possible
 * in the execution of a process, so that error handling will do the right
 * things if an error should occur during process initialization.
 *
 * These are initialized for the bootstrap/standalone case.
 */
bool		IsPostmasterEnvironment = false;
bool		IsUnderPostmaster = false;
bool		IsBinaryUpgrade = false;

bool		ExitOnAnyError = false;

int			DateStyle = USE_ISO_DATES;
int			DateOrder = DATEORDER_MDY;
int			IntervalStyle = INTSTYLE_POSTGRES;

bool		enableFsync = true;
bool		allowSystemTableMods = false;
int			work_mem = 4096;
double		hash_mem_multiplier = 2.0;
int			maintenance_work_mem = 65536;
int			max_parallel_maintenance_workers = 2;

/*
 * Primary determinants of sizes of shared-memory structures.
 *
 * MaxBackends is computed by PostmasterMain after modules have had a chance to
 * register background workers.
 */
int			NBuffers = 16384;
int			MaxConnections = 100;
int			max_worker_processes = 8;
int			max_parallel_workers = 8;
int			MaxBackends = 0;

/* GUC parameters for vacuum */
int			VacuumBufferUsageLimit = 2048;

int			VacuumCostPageHit = 1;
int			VacuumCostPageMiss = 2;
int			VacuumCostPageDirty = 20;
int			VacuumCostLimit = 200;
double		VacuumCostDelay = 0;

int			VacuumCostBalance = 0;	/* working state for vacuum */
bool		VacuumCostActive = false;

/* configurable SLRU buffer sizes */
int			commit_timestamp_buffers = 0;
int			multixact_member_buffers = 32;
int			multixact_offset_buffers = 16;
int			notify_buffers = 16;
int			serializable_buffers = 32;
int			subtransaction_buffers = 0;
int			transaction_buffers = 0;
