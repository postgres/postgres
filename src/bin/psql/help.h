/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Team
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/help.h,v 1.4 2000/01/18 23:30:23 petere Exp $
 */
#ifndef HELP_H
#define HELP_H

#include <c.h>

void		usage(void);

void		slashUsage(void);

void		helpSQL(const char *topic);

void		print_copyright(void);

#endif
