/*-------------------------------------------------------------------------
 *
 * pg_version.c--
 *    
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/bin/pg_version/Attic/pg_version.c,v 1.3 1996/09/19 19:58:27 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>

int Noversion = 0;
char *DataDir = (char *) NULL;

extern void SetPgVersion(const char *);

int
main(int argc, char **argv)
{
    if (argc < 2) {
	fprintf(stderr, "pg_version: missing argument\n");
	exit(1);
    }
    SetPgVersion(argv[1]);
    return(0);
}

void
elog(void) {}

int
GetDataHome(void)
{
	return(0);
}
