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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/init/globals.c,v 1.75 2003/08/26 15:38:25 tgl Exp $
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

bool		Noversion = false;

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

char		pg_pathname[MAXPGPATH];		/* full path to postgres
										 * executable */

BackendId	MyBackendId;

char	   *DatabasePath = NULL;
Oid			MyDatabaseId = InvalidOid;

/* these are initialized for the bootstrap/standalone case: */
bool		IsPostmasterEnvironment = false;
bool		IsUnderPostmaster = false;

bool		ExitOnAnyError = false;

int			DateStyle = USE_ISO_DATES;
int			DateOrder = DATEORDER_MDY;
bool		HasCTZSet = false;
int			CTimeZone = 0;

bool		enableFsync = true;
bool		allowSystemTableMods = false;
int			SortMem = 1024;
int			VacuumMem = 8192;
int			NBuffers = 1000;
