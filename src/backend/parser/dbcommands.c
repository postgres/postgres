/*-------------------------------------------------------------------------
 *
 * dbcommands.c--
 *    
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/parser/Attic/dbcommands.c,v 1.3 1997/01/10 20:18:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "postgres.h"
#include "miscadmin.h"  /* for DataDir */
#include "access/heapam.h"
#include "access/htup.h"
#include "access/relscan.h"
#include "utils/rel.h"
#include "utils/elog.h"
#include "catalog/catname.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_user.h"
#include "catalog/pg_database.h"
#include "utils/syscache.h"
#include "parser/dbcommands.h"
#include "tcop/tcopprot.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"


/* non-export function prototypes */
static void check_permissions(char *command, char *dbname,
			      Oid *dbIdP, Oid *userIdP);
static HeapTuple get_pg_dbtup(char *command, char *dbname, Relation dbrel);

void
createdb(char *dbname)
{
    Oid db_id, user_id;
    char buf[512];
    
    /*
     *  If this call returns, the database does not exist and we're allowed
     *  to create databases.
     */
    check_permissions("createdb", dbname, &db_id, &user_id);
    
    /* close virtual file descriptors so we can do system() calls */
    closeAllVfds();
    
    sprintf(buf, "mkdir %s%cbase%c%s", DataDir, SEP_CHAR, SEP_CHAR, dbname);
    system(buf);
    sprintf(buf, "%s %s%cbase%ctemplate1%c* %s%cbase%c%s",
	    COPY_CMD, DataDir, SEP_CHAR, SEP_CHAR, SEP_CHAR, DataDir,
		SEP_CHAR, SEP_CHAR, dbname);
    system(buf);

/*    sprintf(buf, "insert into pg_database (datname, datdba, datpath) \
                  values (\'%s\'::char16, \'%d\'::oid, \'%s\'::text);",
	    dbname, user_id, dbname);
*/
    sprintf(buf, "insert into pg_database (datname, datdba, datpath) \
                  values (\'%s\', \'%d\', \'%s\');",
	    dbname, user_id, dbname);

    pg_eval(buf, (char **) NULL, (Oid *) NULL, 0);
}

void
destroydb(char *dbname)
{
    Oid user_id, db_id;
    char buf[512];
    
    /*
     *  If this call returns, the database exists and we're allowed to
     *  remove it.
     */
    check_permissions("destroydb", dbname, &db_id, &user_id);
    
    if (!OidIsValid(db_id)) {
	elog(FATAL, "impossible: pg_database instance with invalid OID.");
    }
    
    /* stop the vacuum daemon */
    stop_vacuum(dbname);
    
    /* remove the pg_database tuple FIRST,
       this may fail due to permissions problems*/
    sprintf(buf, "delete from pg_database where pg_database.oid = \'%d\'::oid",
	    db_id);
    pg_eval(buf, (char **) NULL, (Oid *) NULL, 0);
    
    /* remove the data directory. If the DELETE above failed, this will
       not be reached */
    sprintf(buf, "rm -r %s/base/%s", DataDir, dbname);
    system(buf);
    
    /* drop pages for this database that are in the shared buffer cache */
    DropBuffers(db_id);
}

static HeapTuple
get_pg_dbtup(char *command, char *dbname, Relation dbrel)
{
    HeapTuple dbtup;
    HeapTuple tup;
    Buffer buf;
    HeapScanDesc scan;
    ScanKeyData scanKey;
    
    ScanKeyEntryInitialize(&scanKey, 0, Anum_pg_database_datname,
			   NameEqualRegProcedure, NameGetDatum(dbname));
    
    scan = heap_beginscan(dbrel, 0, NowTimeQual, 1, &scanKey);
    if (!HeapScanIsValid(scan))
	elog(WARN, "%s: cannot begin scan of pg_database.", command);
    
    /*
     *  since we want to return the tuple out of this proc, and we're
     *  going to close the relation, copy the tuple and return the copy.
     */
    tup = heap_getnext(scan, 0, &buf);
    
    if (HeapTupleIsValid(tup)) {
	dbtup = heap_copytuple(tup);
	ReleaseBuffer(buf);
    } else
	dbtup = tup;
    
    heap_endscan(scan);
    return (dbtup);
}

/*
 *  check_permissions() -- verify that the user is permitted to do this.
 *
 *  If the user is not allowed to carry out this operation, this routine
 *  elog(WARN, ...)s, which will abort the xact.  As a side effect, the
 *  user's pg_user tuple OID is returned in userIdP and the target database's
 *  OID is returned in dbIdP.
 */

static void
check_permissions(char *command,
		  char *dbname,
		  Oid *dbIdP,
		  Oid *userIdP)
{
    Relation dbrel;
    HeapTuple dbtup, utup;
    Oid dbowner = (Oid)0;
    char use_createdb;
    bool dbfound;
    bool use_super;
    char *userName;

    userName = GetPgUserName();
    utup = SearchSysCacheTuple(USENAME, PointerGetDatum(userName),
			       0,0,0);
    *userIdP = ((Form_pg_user)GETSTRUCT(utup))->usesysid;
    use_super = ((Form_pg_user)GETSTRUCT(utup))->usesuper;
    use_createdb = ((Form_pg_user)GETSTRUCT(utup))->usecreatedb;
    
    /* Check to make sure user has permission to use createdb */
    if (!use_createdb) {
        elog(WARN, "user \"%-.*s\" is not allowed to create/destroy databases",
             NAMEDATALEN, userName);
    }
    
    /* Make sure we are not mucking with the template database */
    if (!strcmp(dbname, "template1")) {
        elog(WARN, "%s cannot be executed on the template database.", command);
    }
    
    /* Check to make sure database is not the currently open database */
    if (!strcmp(dbname, GetDatabaseName())) {
        elog(WARN, "%s cannot be executed on an open database", command);
    }
    
    /* Check to make sure database is owned by this user */
    
    /* 
     * need the reldesc to get the database owner out of dbtup 
     * and to set a write lock on it.
     */
    dbrel = heap_openr(DatabaseRelationName);
    
    if (!RelationIsValid(dbrel))
	elog(FATAL, "%s: cannot open relation \"%-.*s\"",
	     command, DatabaseRelationName);
    
    /*
     * Acquire a write lock on pg_database from the beginning to avoid 
     * upgrading a read lock to a write lock.  Upgrading causes long delays 
     * when multiple 'createdb's or 'destroydb's are run simult. -mer 7/3/91
     */
    RelationSetLockForWrite(dbrel);
    dbtup = get_pg_dbtup(command, dbname, dbrel);
    dbfound = HeapTupleIsValid(dbtup);
    
    if (dbfound) {
	dbowner = (Oid) heap_getattr(dbtup, InvalidBuffer,
				          Anum_pg_database_datdba,
				          RelationGetTupleDescriptor(dbrel),
				          (char *) NULL);
	*dbIdP = dbtup->t_oid;
    } else {
	*dbIdP = InvalidOid;
    }
    
    heap_close(dbrel);
    
    /*
     *  Now be sure that the user is allowed to do this.
     */
    
    if (dbfound && !strcmp(command, "createdb")) {
	
        elog(WARN, "createdb: database %s already exists.", dbname);
	
    } else if (!dbfound && !strcmp(command, "destroydb")) {
	
        elog(WARN, "destroydb: database %s does not exist.", dbname);
	
    } else if (dbfound && !strcmp(command, "destroydb")
	       && dbowner != *userIdP && use_super == false) {
	
        elog(WARN, "%s: database %s is not owned by you.", command, dbname);
	
    }
}

/*
 *  stop_vacuum() -- stop the vacuum daemon on the database, if one is
 *		     running.
 */
void
stop_vacuum(char *dbname)
{
    char filename[256];
    FILE *fp;
    int pid;
    
    sprintf(filename, "%s%cbase%c%s%c%s.vacuum", DataDir, SEP_CHAR, SEP_CHAR,
       dbname, SEP_CHAR, dbname);
    if ((fp = fopen(filename, "r")) != (FILE *) NULL) {
	fscanf(fp, "%d", &pid);
	fclose(fp);
	if (kill(pid, SIGKILLDAEMON1) < 0) {
	    elog(WARN, "can't kill vacuum daemon (pid %d) on %s",
		 pid, dbname);
	}
    }
}
