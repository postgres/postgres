
/* -----------------------------------------------------------------------
 * ascii.h
 *
 * $Id: ascii.h,v 1.1 2000/08/04 20:22:52 momjian Exp $
 *
 *	 Portions Copyright (c) 1999-2000, PostgreSQL, Inc
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

#endif
