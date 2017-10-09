/*
 *	string.h
 *		string handling helpers
 *
 *	Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/include/common/string.h
 */
#ifndef COMMON_STRING_H
#define COMMON_STRING_H

extern bool pg_str_endswith(const char *str, const char *end);

/*
 * Portable version of posix' strnlen.
 *
 * Returns the number of characters before a null-byte in the string pointed
 * to by str, unless there's no null-byte before maxlen. In the latter case
 * maxlen is returned.
 *
 * Use the system strnlen if provided, it's likely to be faster.
 */
#ifdef HAVE_STRNLEN
#define pg_strnlen(str, maxlen) strnlen(str, maxlen)
#else
extern size_t pg_strnlen(const char *str, size_t maxlen);
#endif

#endif							/* COMMON_STRING_H */
