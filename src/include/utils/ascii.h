/*-----------------------------------------------------------------------
 * ascii.h
 *
 *	 Portions Copyright (c) 1999-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/utils/ascii.h,v 1.13 2005/01/01 05:43:09 momjian Exp $
 *
 *-----------------------------------------------------------------------
 */

#ifndef _ASCII_H_
#define _ASCII_H_

#include "fmgr.h"

extern Datum to_ascii_encname(PG_FUNCTION_ARGS);
extern Datum to_ascii_enc(PG_FUNCTION_ARGS);
extern Datum to_ascii_default(PG_FUNCTION_ARGS);

#endif   /* _ASCII_H_ */
