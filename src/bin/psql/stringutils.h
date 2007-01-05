/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2007, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/stringutils.h,v 1.25 2007/01/05 22:19:49 momjian Exp $
 */
#ifndef STRINGUTILS_H
#define STRINGUTILS_H

/* The cooler version of strtok() which knows about quotes and doesn't
 * overwrite your input */
extern char *strtokx(const char *s,
		const char *whitespace,
		const char *delim,
		const char *quote,
		char escape,
		bool e_strings,
		bool del_quotes,
		int encoding);

#endif   /* STRINGUTILS_H */
