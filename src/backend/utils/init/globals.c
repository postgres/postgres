/*-------------------------------------------------------------------------
 *
 * globals.c
 *	  global variable declarations
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/init/globals.c,v 1.87 2004/05/18 03:36:36 momjian Exp $
 *
 * NOTES
 *	  Globals used all over the place should be declared here and not
 *	  in other modules.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq/pqcomm.h"
#include "miscadmin.h"
#include "storage/backendid.h"


ProtocolVersion FrontendProtocol = PG_PROTOCOL_LATEST;

volatile bool InterruptPending = false;
volatile bool QueryCancelPending = false;
volatile bool ProcDiePending = false;
volatile bool ImmediateInterruptOK = false;
volatile uint32 InterruptHoldoffCount = 0;
volatile uint32 CritSectionCount = 0;

int			MyProcPid;
struct Port *MyProcPort;
long		MyCancelKey;

char	   *DataDir = NULL;

 /*
  * The PGDATA directory user says to use, or defaults to via environment
  * variable.  NULL if no option given and no environment variable set
  */

char		OutputFileName[MAXPGPATH];

char		my_exec_path[MAXPGPATH];	/* full path to postgres executable */
char		postgres_exec_path[MAXPGPATH];	/* full path to backend executable */
char		pkglib_path[MAXPGPATH];	/* full path to lib directory */

BackendId	MyBackendId;

char	   *DatabasePath = NULL;
Oid			MyDatabaseId = InvalidOid;

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

bool		ExitOnAnyError = false;

int			DateStyle = USE_ISO_DATES;
int			DateOrder = DATEORDER_MDY;
bool		HasCTZSet = false;
int			CTimeZone = 0;

bool		enableFsync = true;
bool		allowSystemTableMods = false;
int			work_mem = 1024;
int			maintenance_work_mem = 16384;
int			NBuffers = 1000;

int			VacuumCostPageHit = 1;			/* GUC parameters for vacuum */
int			VacuumCostPageMiss = 10;
int			VacuumCostPageDirty = 20;
int			VacuumCostLimit = 200;
int			VacuumCostNaptime = 0;

int			VacuumCostBalance = 0;			/* working state for vacuum */
bool		VacuumCostActive = false;
