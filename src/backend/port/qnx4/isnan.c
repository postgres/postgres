/*-------------------------------------------------------------------------
 *
 * isnan.c
 *	  isnan() implementation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/isnan.c,v 1.1 1999/12/16 16:52:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "os.h"

unsigned char __nan[8] = __nan_bytes;

int isnan(double dsrc)
{
  return !memcmp( &dsrc, &NAN, sizeof( double ) );
}
