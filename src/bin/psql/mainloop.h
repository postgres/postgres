/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/mainloop.h,v 1.19 2006/06/14 16:49:02 tgl Exp $
 */
#ifndef MAINLOOP_H
#define MAINLOOP_H

#include "postgres_fe.h"
#include <stdio.h>

int			MainLoop(FILE *source);

#endif   /* MAINLOOP_H */
