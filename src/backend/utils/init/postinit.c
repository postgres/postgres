/*-------------------------------------------------------------------------
 *
 * postinit.c--
 *    postgres initialization utilities
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/init/postinit.c,v 1.9 1997/07/24 20:17:34 momjian Exp $
 *
 * NOTES
 *      InitPostgres() is the function called from PostgresMain
 *      which does all non-trival initialization, mainly by calling
 *      all the other initialization functions.  InitPostgres()
 *      is only used within the "postgres" backend and so that routine
 *      is in tcop/postgres.c  InitPostgres() is needed in cinterface.a
 *      because things like the bootstrap backend program need it. Hence
 *      you find that in this file...
 *
 *      If you feel the need to add more initialization code, it should be
 *      done in InitPostgres() or someplace lower.  Do not start
 *      putting stuff in PostgresMain - if you do then someone
 *      will have to clean it up later, and it's not going to be me!
 *      -cim 10/3/90
 *
 *-------------------------------------------------------------------------
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#include <unistd.h>

#include "postgres.h"
#include "version.h"

#include <storage/ipc.h>
#include <storage/backendid.h>
#include <storage/buf_internals.h>
#include <storage/smgr.h>
#include <storage/proc.h>
#include <utils/relcache.h>

#include "access/heapam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "access/transam.h"     /* XXX dependency problem */
#include "utils/tqual.h"
#include "utils/syscache.h"
#include "storage/bufpage.h"    /* for page layout, for InitMyDatabaseId() */
#include "storage/sinval.h"
#include "storage/sinvaladt.h"
#include "storage/lmgr.h"

#include "miscadmin.h"         /* for global decls */
#include "utils/portal.h"       /* for EnablePortalManager, etc. */

#include "utils/exc.h"          /* for EnableExceptionHandling, etc. */
#include "fmgr.h"               /* for EnableDynamicFunctionManager, etc. */
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/mcxt.h"         /* for EnableMemoryContext, etc. */

#include "catalog/catname.h"
#include "catalog/pg_database.h"

#include "port-protos.h"
#include "libpq/libpq-be.h"


static IPCKey           PostgresIpcKey;


#ifndef private
#ifndef EBUG
#define private static
#else   /* !defined(EBUG) */
#define private
#endif  /* !defined(EBUG) */
#endif  /* !defined(private) */

/* ----------------------------------------------------------------
 *                      InitPostgres support
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *  InitMyDatabaseId() -- Find and record the OID of the database we are
 *                        to open.
 *
 *      The database's oid forms half of the unique key for the system
 *      caches and lock tables.  We therefore want it initialized before
 *      we open any relations, since opening relations puts things in the
 *      cache.  To get around this problem, this code opens and scans the
 *      pg_database relation by hand.
 *
 *      This algorithm relies on the fact that first attribute in the
 *      pg_database relation schema is the database name.  It also knows
 *      about the internal format of tuples on disk and the length of
 *      the datname attribute.  It knows the location of the pg_database
 *      file.
 *
 *      This code is called from InitDatabase(), after we chdir() to the
 *      database directory but before we open any relations.
 * --------------------------------
 */
void
InitMyDatabaseId()
{
    int         dbfd;
    int         fileflags;
    int         nbytes;
    int         max, i;
    HeapTuple   tup;
    Page        pg;
    PageHeader  ph;
    char        *dbfname;
    Form_pg_database    tup_db;
    
    /*
     *  At bootstrap time, we don't need to check the oid of the database
     *  in use, since we're not using shared memory.  This is lucky, since
     *  the database may not be in the tables yet.
     */
    
    if (IsBootstrapProcessingMode()) {
        LockDisable(true);
        return;
    }
    
    dbfname = (char *) palloc(strlen(DataDir) + strlen("pg_database") + 2);
    sprintf(dbfname, "%s%cpg_database", DataDir, SEP_CHAR);
    fileflags = O_RDONLY;
    
    if ((dbfd = open(dbfname, O_RDONLY, 0666)) < 0)
        elog(FATAL, "Cannot open %s", dbfname);
    
    pfree(dbfname);
    
    /* ----------------
     *  read and examine every page in pg_database
     *
     *  Raw I/O! Read those tuples the hard way! Yow!
     *
     *  Why don't we use the access methods or move this code
     *  someplace else?  This is really pg_database schema dependent
     *  code.  Perhaps it should go in lib/catalog/pg_database?
     *  -cim 10/3/90
     *
     *  mao replies 4 apr 91:  yeah, maybe this should be moved to
     *  lib/catalog.  however, we CANNOT use the access methods since
     *  those use the buffer cache, which uses the relation cache, which
     *  requires that the dbid be set, which is what we're trying to do
     *  here.
     * ----------------
     */
    pg = (Page) palloc(BLCKSZ);
    ph = (PageHeader) pg;
    
    while ((nbytes = read(dbfd, pg, BLCKSZ)) == BLCKSZ) {
        max = PageGetMaxOffsetNumber(pg);
        
        /* look at each tuple on the page */
        for (i = 0; i <= max; i++) {
            int offset;
            
            /* if it's a freed tuple, ignore it */
            if (!(ph->pd_linp[i].lp_flags & LP_USED))
                continue;
            
            /* get a pointer to the tuple itself */
            offset = (int) ph->pd_linp[i].lp_off;
            tup = (HeapTuple) (((char *) pg) + offset);
            
            /*
             *  if the tuple has been deleted (the database was destroyed),
             *  skip this tuple.  XXX warning, will robinson:  violation of
             *  transaction semantics happens right here.  we should check
             *  to be sure that the xact that deleted this tuple actually
             *  committed.  only way to do this at init time is to paw over
             *  the log relation by hand, too.  let's be optimistic.
             *
             *  XXX This is an evil type cast.  tup->t_xmax is char[5] while
             *  TransactionId is struct * { char data[5] }.  It works but
             *  if data is ever moved and no longer the first field this 
             *  will be broken!! -mer 11 Nov 1991.
             */
            if (TransactionIdIsValid((TransactionId)tup->t_xmax))
                continue;
            
            /*
             *  Okay, see if this is the one we want.
             *  XXX 1 july 91:  mao and mer discover that tuples now squash
             *                  t_bits.  Why is this?
             *
             *     24 july 92:  mer realizes that the t_bits field is only
             *                  used in the event of null values.  If no
             *                  fields are null we reduce the header size
             *                  by doing the squash.  t_hoff tells you exactly
             *                  how big the header actually is. use the PC
             *                  means of getting at sys cat attrs.
             */
            tup_db = (Form_pg_database)GETSTRUCT(tup);
            
            if (strncmp(GetDatabaseName(),
                        &(tup_db->datname.data[0]),
                        16) == 0)
                {
                    MyDatabaseId = tup->t_oid;
                    goto done;
                }
        }
    }
    
 done:
    (void) close(dbfd);
    pfree(pg);
    
    if (!OidIsValid(MyDatabaseId))
        elog(FATAL,
             "Database %s does not exist in %s",
             GetDatabaseName(),
             DatabaseRelationName);
}



/*
 * DoChdirAndInitDatabaseNameAndPath --
 *      Set current directory to the database directory for the database
 *      named <name>.
 *      Also set global variables DatabasePath and DatabaseName to those
 *      values.  Also check for proper version of database system and
 *      database.  Exit program via elog() if anything doesn't check out.
 *
 * Arguments:
 *      Path and name are invalid if it invalid as a string.
 *      Path is "badly formated" if it is not a string containing a path
 *      to a writable directory.
 *      Name is "badly formated" if it contains more than 16 characters or if
 *      it is a bad file name (e.g., it contains a '/' or an 8-bit character).
 *
 * Exceptions:
 *      BadState if called more than once.
 *      BadArg if both path and name are "badly formated" or invalid.
 *      BadArg if path and name are both "inconsistent" and valid.
 *
 *      This routine is inappropriate in bootstrap mode, since the directories
 *      and version files need not exist yet if we're in bootstrap mode.
 */
static void
DoChdirAndInitDatabaseNameAndPath(char *name) {
    char *reason;  
      /* Failure reason returned by some function.  NULL if no failure */
    struct stat	statbuf;
    char errormsg[1000];

    if (stat(DataDir, &statbuf) < 0) 
        sprintf(errormsg, "Database system does not exist.  "
                "PGDATA directory '%s' not found.  Normally, you "
                "create a database system by running initdb.",
                DataDir);
    else {
        char myPath[MAXPGPATH];  /* DatabasePath points here! */
        
        if (strlen(DataDir) + strlen(name) + 10 > sizeof(myPath))
            sprintf(errormsg, "Internal error in postinit.c: database "
                    "pathname exceeds maximum allowable length.");
        else {
            sprintf(myPath, "%s/base/%s", DataDir, name);

            if (stat(myPath, &statbuf) < 0) 
                sprintf(errormsg, 
                        "Database '%s' does not exist.  "
                        "(We know this because the directory '%s' "
                        "does not exist).  You can create a database "
                        "with the SQL command CREATE DATABASE.  To see "
                        "what databases exist, look at the subdirectories "
                        "of '%s/base/'.",
                        name, myPath, DataDir);
            else {
                ValidatePgVersion(DataDir, &reason);
                if (reason != NULL) 
                    sprintf(errormsg, 
                        "InitPostgres could not validate that the database "
                        "system version is compatible with this level of "
                        "Postgres.  You may need to run initdb to create "
                        "a new database system.  %s", 
                        reason);
                else {
                    ValidatePgVersion(myPath, &reason);
                    if (reason != NULL)
                        sprintf(errormsg, 
                            "InitPostgres could not validate that the "
                            "database version is compatible with this level "
                            "of Postgres, even though the database system "
                            "as a whole appears to be at a compatible level.  "
                            "You may need to recreate the database with SQL "
                            "commands DROP DATABASE and CREATE DATABASE.  "
                            "%s",
                            reason);
                    else {
                        /* The directories and PG_VERSION files are in order.*/
                        int rc;  /* return code from some function we call */
                    
                        SetDatabasePath(myPath);
                        SetDatabaseName(name);
                        rc = chdir(myPath);
                        if (rc < 0)
                           sprintf(errormsg, 
                               "InitPostgres unable to change "
                               "current directory to '%s', errno = %s (%d).",
                               myPath, strerror(errno), errno);
                        else errormsg[0] = '\0';
                    }
                }
            }
        }
    }
    if (errormsg[0] != '\0')
        elog(FATAL, errormsg);
        /* Above does not return */
}



/* --------------------------------
 *      InitUserid
 *
 *      initializes crap associated with the user id.
 * --------------------------------
 */
void
InitUserid()
{
    setuid(geteuid());
    SetUserId();
}

/* --------------------------------
 *      InitCommunication
 *
 *      This routine initializes stuff needed for ipc, locking, etc.
 *      it should be called something more informative.
 *
 * Note:
 *      This does not set MyBackendId.  MyBackendTag is set, however.
 * --------------------------------
 */
void
InitCommunication()
{
    char *postid;
    char *postport;
    IPCKey      key = 0;
    
    /* ----------------
     *  try and get the backend tag from POSTID
     * ----------------
     */
    MyBackendId = -1;
    
    postid = getenv("POSTID");
    if (!PointerIsValid(postid)) {
        MyBackendTag = -1;
    } else {
        MyBackendTag = atoi(postid);
        Assert(MyBackendTag >= 0);
    }
    
    /* ----------------
     *  try and get the ipc key from POSTPORT
     * ----------------
     */
    postport = getenv("POSTPORT");
    
    if (PointerIsValid(postport)) {
        SystemPortAddress address = atoi(postport);
        
        if (address == 0)
            elog(FATAL, "InitCommunication: invalid POSTPORT");
        
        if (MyBackendTag == -1)
            elog(FATAL, "InitCommunication: missing POSTID");
        
        key = SystemPortAddressCreateIPCKey(address);
        
        /*
         * Enable this if you are trying to force the backend to run as if it 
         * is running under the postmaster.
         *
         * This goto forces Postgres to attach to shared memory instead of 
         * using malloc'ed memory (which is the normal behavior if run
         * directly).
         *
         * To enable emulation, run the following shell commands (in addition
         * to enabling this goto)
         *
         *     % setenv POSTID 1
         *     % setenv POSTPORT 4321
         *     % postmaster &
         *     % kill -9 %1
         *
         * Upon doing this, Postmaster will have allocated the shared memory 
         * resources that Postgres will attach to if you enable
         * EMULATE_UNDER_POSTMASTER.
         *
         * This comment may well age with time - it is current as of
         * 8 January 1990
         * 
         * Greg
         */
        
#ifdef EMULATE_UNDER_POSTMASTER
        
        goto forcesharedmemory;
        
#endif
        
    } else if (IsUnderPostmaster) {
        elog(FATAL,
             "InitCommunication: under postmaster and POSTPORT not set");
    } else {
        /* ----------------
         *  assume we're running a postgres backend by itself with
         *  no front end or postmaster.
         * ----------------
         */
        if (MyBackendTag == -1) {
            MyBackendTag = 1;
        }
        
        key = PrivateIPCKey;
    }
    
    /* ----------------
     *  initialize shared memory and semaphores appropriately.
     * ----------------
     */
#ifdef EMULATE_UNDER_POSTMASTER
    
 forcesharedmemory:
    
#endif
    
    PostgresIpcKey = key;
    AttachSharedMemoryAndSemaphores(key);
}


/* --------------------------------
 *      InitStdio
 *
 *      this routine consists of a bunch of code fragments
 *      that used to be randomly scattered through cinit().
 *      they all seem to do stuff associated with io.
 * --------------------------------
 */
void
InitStdio()
{
    (void) DebugFileOpen();
}

/* --------------------------------
 * InitPostgres --
 *      Initialize POSTGRES.
 *
 * Note:
 *      Be very careful with the order of calls in the InitPostgres function.
 * --------------------------------
 */
bool PostgresIsInitialized = false;
extern int NBuffers;

/*
 *  this global is used by wei for testing his code, but must be declared
 *  here rather than in postgres.c so that it's defined for cinterface.a
 *  applications.
 */

/*int   testFlag = 0;*/
int     lockingOff = 0;

/*
 */
void
InitPostgres(char *name)        /* database name */
{
    bool        bootstrap;      /* true if BootstrapProcessing */
    
    /* ----------------
     *  see if we're running in BootstrapProcessing mode
     * ----------------
     */
    bootstrap = IsBootstrapProcessingMode();
    
    /* ----------------
     *  turn on the exception handler.  Note: we cannot use elog, Assert,
     *  AssertState, etc. until after exception handling is on.
     * ----------------
     */
    EnableExceptionHandling(true);
    
    /* ----------------
     *  A stupid check to make sure we don't call this more than once.
     *  But things like ReinitPostgres() get around this by just diddling
     *  the PostgresIsInitialized flag.
     * ----------------
     */
    AssertState(!PostgresIsInitialized);
    
    /* ----------------
     *  Memory system initialization.
     *  (we may call palloc after EnableMemoryContext())
     *
     *  Note EnableMemoryContext() must happen before EnablePortalManager().
     * ----------------
     */
    EnableMemoryContext(true);  /* initializes the "top context" */
    EnablePortalManager(true);  /* memory for portal/transaction stuff */
    
    /* ----------------
     *  initialize the backend local portal stack used by
     *  internal PQ function calls.  see src/lib/libpq/be-dumpdata.c
     *  This is different from the "portal manager" so this goes here.
     *  -cim 2/12/91
     * ----------------
     */    
    be_portalinit();
    
    /* ----------------
     *   attach to shared memory and semaphores, and initialize our
     *   input/output/debugging file descriptors.
     * ----------------
     */
    InitCommunication();
    InitStdio();
    
    /*
     * initialize the local buffer manager
     */
    InitLocalBuffer();

    if (!TransactionFlushEnabled())
        on_exitpg(FlushBufferPool, (caddr_t) NULL);
    
    if (bootstrap) {
        SetDatabasePath(".");
        SetDatabaseName(name);
    } else {
        DoChdirAndInitDatabaseNameAndPath(name);
    }
    
    /* ********************************
     *  code after this point assumes we are in the proper directory!
     * ********************************
     */
    
    /* ----------------
     *  initialize the database id used for system caches and lock tables
     * ----------------
     */
    InitMyDatabaseId();
    
    smgrinit();
    
    /* ----------------
     *  initialize the transaction system and the relation descriptor
     *  cache.  Note we have to make certain the lock manager is off while
     *  we do this.
     * ----------------
     */
    AmiTransactionOverride(IsBootstrapProcessingMode());
    LockDisable(true);
    
    /*
     * Part of the initialization processing done here sets a read
     * lock on pg_log.  Since locking is disabled the set doesn't have
     * intended effect of locking out writers, but this is ok, since
     * we only lock it to examine AMI transaction status, and this is
     * never written after initdb is done. -mer 15 June 1992
     */
    RelationInitialize();          /* pre-allocated reldescs created here */
    InitializeTransactionSystem(); /* pg_log,etc init/crash recovery here */
    
    LockDisable(false);
    
    /* ----------------
     *  anyone knows what this does?  something having to do with
     *  system catalog cache invalidation in the case of multiple
     *  backends, I think -cim 10/3/90
     *  Sets up MyBackendId a unique backend identifier.
     * ----------------
     */
    InitSharedInvalidationState();
    
    /* ----------------
     * Set up a per backend process in shared memory.  Must be done after
     * InitSharedInvalidationState() as it relies on MyBackendId being
     * initialized already.  XXX -mer 11 Aug 1991
     * ----------------
     */
    InitProcess(PostgresIpcKey);
    
    if (MyBackendId > MaxBackendId || MyBackendId <= 0) {
        elog(FATAL, "cinit2: bad backend id %d (%d)",
             MyBackendTag,
             MyBackendId);
    }
    
    /* ----------------
     *  initialize the access methods.
     * ----------------
     */
    initam();
    
    /* ----------------
     *  initialize all the system catalog caches.
     * ----------------
     */
    zerocaches();
    InitCatalogCache();
    
    /* ----------------
     *   set ourselves to the proper user id and figure out our postgres
     *   user id.  If we ever add security so that we check for valid
     *   postgres users, we might do it here.
     * ----------------
     */
    InitUserid();
    
    /* ----------------
     *  ok, all done, now let's make sure we don't do it again.
     * ----------------
     */
    PostgresIsInitialized = true;
/*    on_exitpg(DestroyLocalRelList, (caddr_t) NULL); */
    
    /* ----------------
     *  Done with "InitPostgres", now change to NormalProcessing unless
     *  we're in BootstrapProcessing mode.
     * ----------------
     */
    if (!bootstrap)
        SetProcessingMode(NormalProcessing);
/*    if (testFlag || lockingOff) */
    if (lockingOff)
        LockDisable(true);
}


