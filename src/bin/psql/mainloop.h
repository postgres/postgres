/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2008, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/mainloop.h,v 1.22 2008/01/01 19:45:56 momjian Exp $
 */
#ifndef MAINLOOP_H
#define MAINLOOP_H

#include "postgres_fe.h"

int			MainLoop(FILE *source);

#endif   /* MAINLOOP_H */
