/*-------------------------------------------------------------------------
 *
 * main.c--
 *    Stub main() routine for the postgres backend.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/main/main.c,v 1.2 1996/11/08 05:56:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "miscadmin.h"
#include "bootstrap/bootstrap.h"	/* for BootstrapMain() */
#include "tcop/tcopprot.h"		/* for PostgresMain() */
#include "port-protos.h"		/* for init_address_fixup() */

int
main(int argc, char *argv[])
{
    int len;
#if defined(NOFIXADE) || defined(NOPRINTADE)
    /*
     * Must be first so that the bootstrap code calls it, too.
     * (Only needed on some RISC architectures.)
     */
    init_address_fixup();
#endif /* NOFIXADE || NOPRINTADE */
    
    /* use one executable for both postgres and postmaster,
       invoke one or the other depending on the name of the executable */
    len = strlen(argv[0]);
    if(len >= 10 && ! strcmp(argv[0] + len - 10, "postmaster"))
        exit(PostmasterMain(argc, argv));

    /* if the first argument is "-boot", then invoke the backend in
       bootstrap mode */
    if (argc > 1 && strcmp(argv[1], "-boot") == 0)
	exit(BootstrapMain(argc-1, argv+1)); /* remove the -boot arg from the command line */
    else
	exit(PostgresMain(argc, argv));
}
