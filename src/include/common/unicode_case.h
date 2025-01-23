/*-------------------------------------------------------------------------
 *
 * unicode_case.h
 *	  Routines for converting character case.
 *
 * These definitions can be used by both frontend and backend code.
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * src/include/common/unicode_case.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNICODE_CASE_H
#define UNICODE_CASE_H

#include "mb/pg_wchar.h"

typedef size_t (*WordBoundaryNext) (void *wbstate);

pg_wchar	unicode_lowercase_simple(pg_wchar code);
pg_wchar	unicode_titlecase_simple(pg_wchar code);
pg_wchar	unicode_uppercase_simple(pg_wchar code);
pg_wchar	unicode_casefold_simple(pg_wchar code);
size_t		unicode_strlower(char *dst, size_t dstsize, const char *src,
							 ssize_t srclen, bool full);
size_t		unicode_strtitle(char *dst, size_t dstsize, const char *src,
							 ssize_t srclen, bool full,
							 WordBoundaryNext wbnext, void *wbstate);
size_t		unicode_strupper(char *dst, size_t dstsize, const char *src,
							 ssize_t srclen, bool full);
size_t		unicode_strfold(char *dst, size_t dstsize, const char *src,
							ssize_t srclen, bool full);

#endif							/* UNICODE_CASE_H */
