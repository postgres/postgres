/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/help.h,v 1.10 2002/10/23 19:23:57 momjian Exp $
 */
#ifndef HELP_H
#define HELP_H

void		usage(void);

void		slashUsage(bool pager);

void		helpSQL(const char *topic, bool pager);

void		print_copyright(void);

#endif
