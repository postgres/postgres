/*-------------------------------------------------------------------------
 *
 * fork_process.h
 *	  Exports from postmaster/fork_process.c.
 *
 * Copyright (c) 1996-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/postmaster/fork_process.h,v 1.4 2006/03/05 15:58:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FORK_PROCESS_H
#define FORK_PROCESS_H

extern pid_t fork_process(void);

#endif   /* FORK_PROCESS_H */
