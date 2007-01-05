/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2007, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/mainloop.h,v 1.21 2007/01/05 22:19:49 momjian Exp $
 */
#ifndef MAINLOOP_H
#define MAINLOOP_H

#include "postgres_fe.h"

int			MainLoop(FILE *source);

#endif   /* MAINLOOP_H */
