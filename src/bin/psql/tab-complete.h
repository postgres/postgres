/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 *
 * src/bin/psql/tab-complete.h
 */
#ifndef TAB_COMPLETE_H
#define TAB_COMPLETE_H

#include "pqexpbuffer.h"

extern PQExpBuffer tab_completion_query_buf;

extern void initialize_readline(void);

#endif							/* TAB_COMPLETE_H */
