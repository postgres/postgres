/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/help.h,v 1.17 2006/03/05 15:58:51 momjian Exp $
 */
#ifndef HELP_H
#define HELP_H

void		usage(void);

void		slashUsage(unsigned short int pager);

void		helpSQL(const char *topic, unsigned short int pager);

void		print_copyright(void);

#endif
