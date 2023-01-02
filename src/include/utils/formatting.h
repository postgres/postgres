/* -----------------------------------------------------------------------
 * formatting.h
 *
 * src/include/utils/formatting.h
 *
 *
 *	 Portions Copyright (c) 1999-2023, PostgreSQL Global Development Group
 *
 *	 The PostgreSQL routines for a DateTime/int/float/numeric formatting,
 *	 inspired by the Oracle TO_CHAR() / TO_DATE() / TO_NUMBER() routines.
 *
 *	 Karel Zak
 *
 * -----------------------------------------------------------------------
 */

#ifndef _FORMATTING_H_
#define _FORMATTING_H_


extern char *str_tolower(const char *buff, size_t nbytes, Oid collid);
extern char *str_toupper(const char *buff, size_t nbytes, Oid collid);
extern char *str_initcap(const char *buff, size_t nbytes, Oid collid);

extern char *asc_tolower(const char *buff, size_t nbytes);
extern char *asc_toupper(const char *buff, size_t nbytes);
extern char *asc_initcap(const char *buff, size_t nbytes);

extern Datum parse_datetime(text *date_txt, text *fmt, Oid collid, bool strict,
							Oid *typid, int32 *typmod, int *tz,
							struct Node *escontext);

#endif
