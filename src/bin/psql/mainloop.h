/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/mainloop.h,v 1.6 2000/01/29 16:58:49 petere Exp $
 */
#ifndef MAINLOOP_H
#define MAINLOOP_H

#include <stdio.h>

int MainLoop(FILE *source);

#endif	 /* MAINLOOP_H */
