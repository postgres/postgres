/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/mainloop.h,v 1.15 2003/11/29 19:52:06 pgsql Exp $
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
