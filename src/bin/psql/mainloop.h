/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/bin/psql/mainloop.h
 */
#ifndef MAINLOOP_H
#define MAINLOOP_H

#include "fe_utils/psqlscan.h"

extern const PsqlScanCallbacks psqlscan_callbacks;

extern int	MainLoop(FILE *source);

#endif							/* MAINLOOP_H */
