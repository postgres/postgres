/*-------------------------------------------------------------------------
 *
 * tstrint.c
 *	  rint() test
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/tstrint.c,v 1.1 1999/12/16 16:52:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "os.h"


int main( int argc, char **argv )
{
  double x;

  if( argc != 2 ) exit( 1 );

  x = strtod( argv[1], NULL );
  printf( "rint( %f ) = %f\n", x, rint( x ) );

  return 0;
}
