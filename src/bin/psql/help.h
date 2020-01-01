/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2020, PostgreSQL Global Development Group
 *
 * src/bin/psql/help.h
 */
#ifndef HELP_H
#define HELP_H

void		usage(unsigned short int pager);

void		slashUsage(unsigned short int pager);

void		helpVariables(unsigned short int pager);

void		helpSQL(const char *topic, unsigned short int pager);

void		print_copyright(void);

#endif
