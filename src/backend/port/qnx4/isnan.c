/*-------------------------------------------------------------------------
 *
 * isnan.c
 *	  isnan() implementation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/qnx4/isnan.c,v 1.4 2003/11/29 19:51:54 pgsql Exp $
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
