/*-------------------------------------------------------------------------
 *
 * lotest.cc--
 *    test using large objects with libpq
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/interfaces/libpq++/examples/Attic/testlo.cc,v 1.1.1.1 1996/07/09 06:22:19 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include "libpq++.H"
extern "C" {
#include "libpq/libpq-fs.h"
}

int
main(int argc, char **argv)
{
    char *in_filename, *out_filename;
    char *database;
    Oid lobjOid;
    PGenv env;
    PGlobj *object;

    if (argc < 4 || argc > 5) {
	fprintf(stderr, "Usage: %s database_name in_filename out_filename [oid]\n",
		argv[0]);
	exit(1);
    }

    database = argv[1];
    in_filename = argv[2];
    out_filename = argv[3];

    /*
     * set up the connection and create a largeobject for us
     */
    if (argc == 4) {
      object = new PGlobj(&env, database);
    } else {
      object = new PGlobj(&env, database, atoi(argv[4]));
    }

    /* check to see that the backend connection was successfully made */
    if (object->status() == CONNECTION_BAD) {
	fprintf(stderr,"Connection to database '%s' failed.\n", database);
	fprintf(stderr,"%s",object->errormessage());
	delete object;
	exit(1);
    }
	
    object->exec("BEGIN");
    printf("importing file \"%s\" ...\n", in_filename);
    object->import(in_filename);
    printf("exporting large object to file \"%s\" ...\n", out_filename);
    object->export(out_filename);
    object->exec("END"); // WHY DOES IT CORE DUMP HERE ???
    delete object;
}
