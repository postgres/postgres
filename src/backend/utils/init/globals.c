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
 *	  $Header: /cvsroot/pgsql/src/backend/utils/init/globals.c,v 1.54 2001/03/13 01:17:06 tgl Exp $
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
bool		Quiet = false;

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

char		OutputFileName[MAXPGPATH] = "";

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

bool        enableFsync = true;
bool		allowSystemTableMods = false;
int			SortMem = 512;
int			NBuffers = DEF_NBUFFERS;


char	   *IndexedCatalogNames[] = {
	AttributeRelationName,
	ProcedureRelationName,
	TypeRelationName,
	RelationRelationName,
	0
};


/* ----------------
 * we just do a linear search now so there's no requirement that the list
 * be ordered.	The list is so small it shouldn't make much difference.
 * make sure the list is null-terminated
 *				- jolly 8/19/95
 *
 * OLD COMMENT
 *		WARNING  WARNING  WARNING  WARNING	WARNING  WARNING
 *
 *		keep SharedSystemRelationNames[] in SORTED order!  A binary search
 *		is done on it in catalog.c!
 *
 *		XXX this is a serious hack which should be fixed -cim 1/26/90
 * ----------------
 */
char	   *SharedSystemRelationNames[] = {
	DatabaseRelationName,
	GroupRelationName,
	GroupNameIndex,
	GroupSysidIndex,
	LogRelationName,
	ShadowRelationName,
	VariableRelationName,
	0
};
