/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/mainloop.h,v 1.10 2001/02/10 02:31:28 tgl Exp $
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

#endif	 /* MAINLOOP_H */
