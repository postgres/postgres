/*-------------------------------------------------------------------------
 *
 * globals.c
 *	  global variable declarations
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/init/globals.c,v 1.61 2001/10/21 03:25:35 tgl Exp $
 *
 * NOTES
 *	  Globals used all over the place should be declared here and not
 *	  in other modules.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <math.h>
#include <unistd.h>

#include "catalog/catname.h"
#include "catalog/indexing.h"
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

Relation	reldesc;			/* current relation descriptor */

char		OutputFileName[MAXPGPATH];

char		pg_pathname[MAXPGPATH];	/* full path to postgres executable */

BackendId	MyBackendId;

char	   *DatabaseName = NULL;
char	   *DatabasePath = NULL;

Oid			MyDatabaseId = InvalidOid;

bool		IsUnderPostmaster = false;

int			DebugLvl = 0;

int			DateStyle = USE_ISO_DATES;
bool		EuroDates = false;
bool		HasCTZSet = false;
bool		CDayLight = false;
int			CTimeZone = 0;
char		CTZName[MAXTZLEN + 1] = "";

char		DateFormat[20] = "%d-%m-%Y";		/* mjl: sizes! or better
												 * malloc? XXX */
char		FloatFormat[20] = "%f";

bool		enableFsync = true;
bool		allowSystemTableMods = false;
int			SortMem = 512;
int			VacuumMem = 8192;
int			NBuffers = DEF_NBUFFERS;


/* ----------------
 * List of relations that are shared across all databases in an installation.
 *
 * This used to be binary-searched, requiring that it be kept in sorted order.
 * We just do a linear search now so there's no requirement that the list
 * be ordered.	The list is so small it shouldn't make much difference.
 * make sure the list is null-terminated
 *				- jolly 8/19/95
 * ----------------
 */
char	   *SharedSystemRelationNames[] = {
	DatabaseRelationName,
	DatabaseNameIndex,
	DatabaseOidIndex,
	GroupRelationName,
	GroupNameIndex,
	GroupSysidIndex,
	ShadowRelationName,
	ShadowNameIndex,
	ShadowSysidIndex,
	NULL
};
