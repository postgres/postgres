/*-----------------------------------------------------------------------
 * ascii.h
 *
 *	 Portions Copyright (c) 1999-2002, PostgreSQL Global Development Group
 *
 * $Id: ascii.h,v 1.10 2003/07/14 16:58:24 tgl Exp $
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
