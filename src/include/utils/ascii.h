
/* -----------------------------------------------------------------------
 * ascii.h
 *
 * $Id: ascii.h,v 1.9 2002/08/29 07:22:29 ishii Exp $
 *
 *	 Portions Copyright (c) 1999-2000, PostgreSQL Global Development Group
 *
 * -----------------------------------------------------------------------
 */

#ifndef _ASCII_H_
#define _ASCII_H_

#include "fmgr.h"

extern Datum to_ascii_encname(PG_FUNCTION_ARGS);
extern Datum to_ascii_enc(PG_FUNCTION_ARGS);
extern Datum to_ascii_default(PG_FUNCTION_ARGS);

extern char *pg_to_ascii(unsigned char *src, unsigned char *src_end,
			unsigned char *desc, int enc);

#endif   /* _ASCII_H_ */
