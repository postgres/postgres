/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000-2002 by PostgreSQL Global Development Group
 *
 * $Id: stringutils.h,v 1.18 2002/10/19 00:22:14 tgl Exp $
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
		bool del_quotes,
		int encoding);

#endif   /* STRINGUTILS_H */
