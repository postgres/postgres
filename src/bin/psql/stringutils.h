/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/stringutils.h,v 1.13 2000/01/29 16:58:49 petere Exp $
 */
#ifndef STRINGUTILS_H
#define STRINGUTILS_H

/* The cooler version of strtok() which knows about quotes and doesn't
 * overwrite your input */
extern char *strtokx(const char *s,
		const char *delim,
		const char *quote,
		char escape,
		char *was_quoted,
		unsigned int *token_pos,
		int encoding);

#endif	 /* STRINGUTILS_H */
