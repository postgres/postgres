/*-------------------------------------------------------------------------
 *
 * isnan.c
 *	  isnan() implementation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/isnan.c,v 1.3 2001/08/24 14:07:49 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

unsigned char __nan[8] = __nan_bytes;

int
isnan(double dsrc)
{
	return !memcmp(&dsrc, &NAN, sizeof(double));
}
