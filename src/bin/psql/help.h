/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/help.h,v 1.9 2002/07/15 01:56:25 momjian Exp $
 */
#ifndef HELP_H
#define HELP_H

void		usage(void);

void		slashUsage(bool pager);

void		helpSQL(const char *topic);

void		print_copyright(void);

#endif
