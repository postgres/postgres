/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/mainloop.h,v 1.8 2000/02/20 14:28:20 petere Exp $
 */
#ifndef MAINLOOP_H
#define MAINLOOP_H

#include "postgres.h"
#include <stdio.h>
#ifndef WIN32
#include <setjmp.h>

extern sigjmp_buf main_loop_jmp;
#endif

int MainLoop(FILE *source);

#endif	 /* MAINLOOP_H */
