/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Id: stringutils.h,v 1.19 2003/08/04 23:59:40 tgl Exp $
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
