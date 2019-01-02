/*-------------------------------------------------------------------------
 *
 * bytea.h
 *	  Declarations for BYTEA data type support.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/bytea.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BYTEA_H
#define BYTEA_H

#include "fmgr.h"


typedef enum
{
	BYTEA_OUTPUT_ESCAPE,
	BYTEA_OUTPUT_HEX
}			ByteaOutputType;

extern int	bytea_output;		/* ByteaOutputType, but int for GUC enum */

#endif							/* BYTEA_H */
