/*
 * utils.c --
 *
 * This file defines various Postgres utility functions.
 *
 * Copyright (c) 1996, Massimo Dal Zotto <dz@cs.unitn.it>
 */

#include <unistd.h>

#include "postgres.h"
#include "utils/palloc.h"

#include "misc_utils.h"

extern int ExecutorLimit(int limit);
extern void Async_Unlisten(char *relname, int pid);

int
query_limit(int limit)
{
    return ExecutorLimit(limit);
}

int
backend_pid()
{
    return getpid();
}

int
unlisten(char *relname)
{
    Async_Unlisten(relname, getpid());
    return 0;
}

int
max(int x, int y)
{
    return ((x > y) ? x : y);
}

int
min(int x, int y)
{
    return ((x < y) ? x : y);
}

/* end of file */
