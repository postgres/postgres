/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/help.h,v 1.6 2000/02/16 13:15:26 momjian Exp $
 */
#ifndef HELP_H
#define HELP_H

void		usage(void);

void		slashUsage(void);

void		helpSQL(const char *topic);

void		print_copyright(void);

#endif
