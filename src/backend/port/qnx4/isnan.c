/*-------------------------------------------------------------------------
 *
 * isnan.c
 *	  isnan() implementation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/qnx4/isnan.c,v 1.5 2004/03/15 03:29:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <math.h>

#ifndef __nan_bytes
#define __nan_bytes			{ 0, 0, 0, 0, 0, 0, 0xf8, 0x7f }
#endif   /* __nan_bytes */

static unsigned char __nan[8] = __nan_bytes;

int
isnan(double dsrc)
{
	return memcmp(&dsrc, __nan, sizeof(double)) == 0;
}
