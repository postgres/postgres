/*-------------------------------------------------------------------------
 *
 * unicode_case.h
 *	  Routines for converting character case.
 *
 * These definitions can be used by both frontend and backend code.
 *
 * Copyright (c) 2017-2026, PostgreSQL Global Development Group
 *
 * src/include/common/unicode_case.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNICODE_CASE_H
#define UNICODE_CASE_H

typedef size_t (*WordBoundaryNext) (void *wbstate);

char32_t	unicode_lowercase_simple(char32_t code);
char32_t	unicode_titlecase_simple(char32_t code);
char32_t	unicode_uppercase_simple(char32_t code);
char32_t	unicode_casefold_simple(char32_t code);
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
