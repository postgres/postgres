/*-------------------------------------------------------------------------
 *
 * fork_process.h
 *	  Exports from postmaster/fork_process.c.
 *
 * Copyright (c) 1996-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/postmaster/fork_process.h,v 1.2 2005/03/13 23:32:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FORK_PROCESS_H
#define FORK_PROCESS_H

extern pid_t fork_process(void);

#endif /* FORK_PROCESS_H */
