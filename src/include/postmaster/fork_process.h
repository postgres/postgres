/*-------------------------------------------------------------------------
 *
 * fork_process.h
 *	  Exports from postmaster/fork_process.c.
 *
 * Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/postmaster/fork_process.h,v 1.7 2009/01/01 17:24:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FORK_PROCESS_H
#define FORK_PROCESS_H

extern pid_t fork_process(void);

#endif   /* FORK_PROCESS_H */
