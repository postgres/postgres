/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2011, PostgreSQL Global Development Group
 *
 * src/bin/psql/stringutils.h
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
