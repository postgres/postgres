/*-------------------------------------------------------------------------
 *
 * globals.c--
 *	  global variable declarations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/init/globals.c,v 1.25 1998/08/25 21:24:10 scrappy Exp $
 *
 * NOTES
 *	  Globals used all over the place should be declared here and not
 *	  in other modules.
 *
 *-------------------------------------------------------------------------
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <math.h>
#include <unistd.h>

#include "postgres.h"
#include "miscadmin.h"			/* where the declarations go */

#include <storage/backendid.h>
#include "access/heapam.h"
#include "storage/sinval.h"
#include "storage/sinvaladt.h"
#include "storage/lmgr.h"
#include "utils/elog.h"
#include "libpq/pqcomm.h"
#include "catalog/catname.h"

ProtocolVersion FrontendProtocol = PG_PROTOCOL_LATEST;
int			Portfd = -1;

bool		Noversion = false;
bool		Quiet = false;
bool		QueryCancel = false;

int			MyProcPid;
struct Port *MyProcPort;
long		MyCancelKey;

char	   *DataDir;

 /*
  * The PGDATA directory user says to use, or defaults to via environment
  * variable.  NULL if no option given and no environment variable set
  */
Relation	reldesc;			/* current relation descriptor */

char		OutputFileName[MAXPGPATH] = "";

BackendId	MyBackendId;
BackendTag	MyBackendTag;

char	   *UserName = NULL;
char	   *DatabaseName = NULL;
char	   *DatabasePath = NULL;

bool		MyDatabaseIdIsInitialized = false;
Oid			MyDatabaseId = InvalidOid;
bool		TransactionInitWasProcessed = false;

bool		IsUnderPostmaster = false;

short		DebugLvl = 0;

int			DateStyle = USE_POSTGRES_DATES;
bool		EuroDates = false;
bool		HasCTZSet = false;
bool		CDayLight = false;
int			CTimeZone = 0;
char		CTZName[MAXTZLEN + 1] = "";

char		DateFormat[20] = "%d-%m-%Y";		/* mjl: sizes! or better
												 * malloc? XXX */
char		FloatFormat[20] = "%f";

int			fsyncOff = 0;
int			SortMem = 512;

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
	LogRelationName,
	ShadowRelationName,
	VariableRelationName,
	0
};
