/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Team
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/mainloop.h,v 1.5 2000/01/18 23:30:24 petere Exp $
 */
#ifndef MAINLOOP_H
#define MAINLOOP_H

#include <stdio.h>

int MainLoop(FILE *source);

#endif	 /* MAINLOOP_H */
