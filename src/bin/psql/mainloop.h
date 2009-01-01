/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2009, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/mainloop.h,v 1.23 2009/01/01 17:23:55 momjian Exp $
 */
#ifndef MAINLOOP_H
#define MAINLOOP_H

#include "postgres_fe.h"

int			MainLoop(FILE *source);

#endif   /* MAINLOOP_H */
