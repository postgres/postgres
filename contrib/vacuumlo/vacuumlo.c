/*-------------------------------------------------------------------------
 *
 * vacuumlo.c
 *	  This removes orphaned large objects from a database.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/contrib/vacuumlo/vacuumlo.c,v 1.1 1999/04/10 16:48:05 peter Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

#define BUFSIZE			1024

int vacuumlo(char *,int);


/*
 * This vacuums a database. It returns 1 on success, -1 on failure.
 */
int vacuumlo(char *database,int verbose)
{
    PGconn     *conn;
    PGresult   *res, *res2;
    char buf[BUFSIZE];
    int matched=0;	/* Number matched per scan */
    int i;
    
    conn = PQsetdb(NULL, NULL, NULL, NULL, database);
    
    /* check to see that the backend connection was successfully made */
    if (PQstatus(conn) == CONNECTION_BAD)
	{
	    fprintf(stderr, "Connection to database '%s' failed.\n", database);
	    fprintf(stderr, "%s", PQerrorMessage(conn));
	    return -1;
	}
    
    if(verbose)
	fprintf(stdout,"Connected to %s\n",database);
    
    /*
     * First we create and populate the lo temp table
     */
    buf[0]='\0';
    strcat(buf,"SELECT oid AS lo ");
    strcat(buf,"INTO TEMP TABLE vacuum_l ");
    strcat(buf,"FROM pg_class ");
    strcat(buf,"WHERE relkind='l'");
    if(!(res = PQexec(conn,buf))) {
	fprintf(stderr,"Failed to create temp table.\n");
	PQfinish(conn);
	return -1;
    }
    PQclear(res);
    
    /*
     * Now find any candidate tables who have columns of type oid (the column
     * oid is ignored, as it has attnum < 1)
     */
    buf[0]='\0';
    strcat(buf,"SELECT c.relname, a.attname ");
    strcat(buf,"FROM pg_class c, pg_attribute a, pg_type t ");
    strcat(buf,"WHERE a.attnum > 0 ");
    strcat(buf,"      AND a.attrelid = c.oid ");
    strcat(buf,"      AND a.atttypid = t.oid ");
    strcat(buf,"      AND t.typname = 'oid' ");
    strcat(buf,"      AND c.relname NOT LIKE 'pg_%'");
    if(!(res = PQexec(conn,buf))) {
	fprintf(stderr,"Failed to create temp table.\n");
	PQfinish(conn);
	return -1;
    }
    for(i=0;i<PQntuples(res);i++)
	{
	    char *table,*field;
	    
	    table = PQgetvalue(res,i,0);
	    field = PQgetvalue(res,i,1);
	    
	    if(verbose) {
		fprintf(stdout,"Checking %s in %s: ",field,table);
		fflush(stdout);
	    }
	    
	    res2 = PQexec(conn, "begin");
	    PQclear(res2);
	    
	    buf[0] = '\0';
	    strcat(buf,"DELETE FROM vacuum_l ");
	    strcat(buf,"WHERE lo IN (");
	    strcat(buf,"SELECT ");
	    strcat(buf,field);
	    strcat(buf," FROM ");
	    strcat(buf,table);
	    strcat(buf,");");
	    if(!(res2 = PQexec(conn,buf))) {
		fprintf(stderr,"Failed to check %s in table %s\n",field,table);
		PQclear(res);
		PQfinish(conn);
		return -1;
	    }
	    if(PQresultStatus(res2)!=PGRES_COMMAND_OK) {
		fprintf(stderr,
			"Failed to check %s in table %s\n%s\n",
			field,table,
			PQerrorMessage(conn)
			);
		PQclear(res2);
		PQclear(res);
		PQfinish(conn);
		return -1;
	    }
	    PQclear(res2);

	    res2 = PQexec(conn, "end");
	    PQclear(res2);
	    
	}
    PQclear(res);
    
    /* Start the transaction */
    res = PQexec(conn, "begin");
    PQclear(res);
    
    /*
     * Finally, those entries remaining in vacuum_l are orphans.
     */
    buf[0]='\0';
    strcat(buf,"SELECT lo ");
    strcat(buf,"FROM vacuum_l");
    if(!(res = PQexec(conn,buf))) {
	fprintf(stderr,"Failed to read temp table.\n");
	PQfinish(conn);
	return -1;
    }
    matched=PQntuples(res);
    for(i=0;i<matched;i++)
	{
	    Oid lo = (Oid) atoi(PQgetvalue(res,i,0));
	    
	    if(verbose) {
		fprintf(stdout,"\rRemoving lo %6d \n",lo);
		fflush(stdout);
	    }
	    
	    if(lo_unlink(conn,lo)<0) {
		fprintf(stderr,"Failed to remove lo %d\n",lo);
	    }
	}
    PQclear(res);
    
    /*
     * That's all folks!
     */
    res = PQexec(conn, "end");
    PQclear(res);
    PQfinish(conn);
    
    if(verbose)
	fprintf(stdout,"\rRemoved %d large objects from %s.\n",matched,database);
    
    return 0;
}

int
main(int argc, char **argv)
{
    int verbose = 0;
    int arg;
    int rc=0;
    
    if (argc < 2)
	{
	    fprintf(stderr, "Usage: %s [-v] database_name [db2 ... dbn]\n",
		    argv[0]);
	    exit(1);
	}
    
    for(arg=1;arg<argc;arg++) {
	if(strcmp("-v",argv[arg])==0)
	    verbose=!verbose;
	else
	    rc += vacuumlo(argv[arg],verbose);
    }
    
    return rc;
}
