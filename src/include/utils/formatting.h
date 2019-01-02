/* -----------------------------------------------------------------------
 * formatting.h
 *
 * src/include/utils/formatting.h
 *
 *
 *	 Portions Copyright (c) 1999-2019, PostgreSQL Global Development Group
 *
 *	 The PostgreSQL routines for a DateTime/int/float/numeric formatting,
 *	 inspire with Oracle TO_CHAR() / TO_DATE() / TO_NUMBER() routines.
 *
 *	 Karel Zak
 *
 * -----------------------------------------------------------------------
 */

#ifndef _FORMATTING_H_
#define _FORMATTING_H_

#include "fmgr.h"


extern char *str_tolower(const char *buff, size_t nbytes, Oid collid);
extern char *str_toupper(const char *buff, size_t nbytes, Oid collid);
extern char *str_initcap(const char *buff, size_t nbytes, Oid collid);

extern char *asc_tolower(const char *buff, size_t nbytes);
extern char *asc_toupper(const char *buff, size_t nbytes);
extern char *asc_initcap(const char *buff, size_t nbytes);

#endif
