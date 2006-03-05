/*-----------------------------------------------------------------------
 * ascii.h
 *
 *	 Portions Copyright (c) 1999-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/utils/ascii.h,v 1.14 2006/03/05 15:59:06 momjian Exp $
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
