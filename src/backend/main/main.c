/*-------------------------------------------------------------------------
 *
 * main.c--
 *    Stub main() routine for the postgres backend.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/main/main.c,v 1.6 1997/04/15 17:39:17 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "postgres.h"
#ifdef USE_LOCALE  
  #include <locale.h>
#endif
#include "miscadmin.h"
#include "bootstrap/bootstrap.h"	/* for BootstrapMain() */
#include "tcop/tcopprot.h"		/* for PostgresMain() */
#include "port-protos.h"		/* for init_address_fixup() */

#define NOROOTEXEC "\
\n\"root\" execution of the PostgreSQL backend is not permitted\n\n\
It is highly recommended that the backend be started under it's own userid\n\
to prevent possible system security compromise. This can be accomplished\n\
by placing the following command in the PostgreSQL startup script.\n\n\
echo \"postmaster -B 256 >/var/log/pglog 2>&1 &\" | su - postgres\n\n"

int
main(int argc, char *argv[])
{
    int len;
#ifdef USE_LOCALE
    setlocale(LC_CTYPE,""); /* take locale information from an environment */
    setlocale(LC_COLLATE,"");
    setlocale(LC_MONETARY,"");
#endif
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

    if (!geteuid()) {
      fprintf(stderr, "%s", NOROOTEXEC);
      exit(1);
    }

    if(len >= 10 && ! strcmp(argv[0] + len - 10, "postmaster"))
        exit(PostmasterMain(argc, argv));

    /* if the first argument is "-boot", then invoke the backend in
       bootstrap mode */
    if (argc > 1 && strcmp(argv[1], "-boot") == 0)
	exit(BootstrapMain(argc-1, argv+1)); /* remove the -boot arg from the command line */
    else
	exit(PostgresMain(argc, argv));
}
