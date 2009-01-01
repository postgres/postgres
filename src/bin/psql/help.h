/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2009, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/help.h,v 1.20 2009/01/01 17:23:55 momjian Exp $
 */
#ifndef HELP_H
#define HELP_H

void		usage(void);

void		slashUsage(unsigned short int pager);

void		helpSQL(const char *topic, unsigned short int pager);

void		print_copyright(void);

#endif
