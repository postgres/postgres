/*-------------------------------------------------------------------------
 *
 * tstrint.c
 *	  rint() test
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/tstrint.c,v 1.4 2002/11/08 20:23:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <errno.h>


int
main(int argc, char **argv)
{
	double		x;

	if (argc != 2)
		exit(1);

	x = strtod(argv[1], NULL);
	printf("rint( %f ) = %f\n", x, rint(x));

	return 0;
}
