/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/mainloop.h,v 1.17 2005/01/01 05:43:08 momjian Exp $
 */
#ifndef MAINLOOP_H
#define MAINLOOP_H

#include "postgres_fe.h"
#include <stdio.h>
#ifndef WIN32
#include <setjmp.h>

extern sigjmp_buf main_loop_jmp;
#endif

int			MainLoop(FILE *source);

#endif   /* MAINLOOP_H */
