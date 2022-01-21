/*-------------------------------------------------------------------------
 *
 * Multibyte character printing support for frontend code
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/mbprint.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MBPRINT_H
#define MBPRINT_H

struct lineptr
{
	unsigned char *ptr;
	int			width;
};

extern unsigned char *mbvalidate(unsigned char *pwcs, int encoding);
extern int	pg_wcswidth(const char *pwcs, size_t len, int encoding);
extern void pg_wcsformat(const unsigned char *pwcs, size_t len, int encoding,
						 struct lineptr *lines, int count);
extern void pg_wcssize(const unsigned char *pwcs, size_t len, int encoding,
					   int *width, int *height, int *format_size);

#endif							/* MBPRINT_H */
