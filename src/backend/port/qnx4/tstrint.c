/*-------------------------------------------------------------------------
 *
 * tstrint.c
 *	  rint() test
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/tstrint.c,v 1.3 2001/08/24 14:07:49 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>


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
