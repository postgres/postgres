/*-------------------------------------------------------------------------
 *
 * globals.c--
 *    global variable declarations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/init/globals.c,v 1.4 1997/01/14 08:05:36 bryanh Exp $
 *
 * NOTES
 *    Globals used all over the place should be declared here and not
 *    in other modules.  
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
#include "miscadmin.h"		/* where the declarations go */

#include <storage/backendid.h>
#include "access/heapam.h"
#include "utils/tqual.h"
#include "storage/sinval.h"
#include "storage/sinvaladt.h"
#include "storage/lmgr.h"
#include "utils/elog.h"

#include "catalog/catname.h"

int		Portfd = -1;
int		Noversion = 0;
int             Quiet = 1;

int		MasterPid;
char            *DataDir;
  /* The PGDATA directory user says to use, or defaults to via environment
     variable.  NULL if no option given and no environment variable set 
  */
Relation        reldesc;                /* current relation descriptor */
    
char		OutputFileName[MAXPGPATH] = "";

BackendId	MyBackendId;
BackendTag	MyBackendTag;

char            *UserName = NULL;
char            *DatabaseName = NULL;
char 		*DatabasePath = NULL;

bool		MyDatabaseIdIsInitialized = false;
Oid		MyDatabaseId = InvalidOid;
bool		TransactionInitWasProcessed = false;

bool		IsUnderPostmaster = false;
bool		IsPostmaster = false;

short		DebugLvl = 0;

char *IndexedCatalogNames[] = {
    AttributeRelationName,
    ProcedureRelationName,
    TypeRelationName,
    RelationRelationName,
    0
};


/* ----------------
 * we just do a linear search now so there's no requirement that the list
 * be ordered.  The list is so small it shouldn't make much difference.
 * make sure the list is null-terminated
 *              - jolly 8/19/95
 *                                  
 * OLD COMMENT
 *	WARNING  WARNING  WARNING  WARNING  WARNING  WARNING
 *
 *	keep SharedSystemRelationNames[] in SORTED order!  A binary search
 *	is done on it in catalog.c!
 *
 *	XXX this is a serious hack which should be fixed -cim 1/26/90
 * ----------------
 */
char *SharedSystemRelationNames[] = {
    DatabaseRelationName, 
    DefaultsRelationName,
    DemonRelationName,
    GroupRelationName,
    HostsRelationName,
    LogRelationName,
    MagicRelationName,
    ServerRelationName,
    TimeRelationName,
    UserRelationName,
    VariableRelationName,
    0
};
