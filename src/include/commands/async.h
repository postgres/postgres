/*-------------------------------------------------------------------------
 *
 * async.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: async.h,v 1.9 1998/09/01 04:35:22 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ASYNC_H
#define ASYNC_H

#include <nodes/memnodes.h>

extern void Async_NotifyHandler(SIGNAL_ARGS);
extern void Async_Notify(char *relname);
extern void Async_NotifyAtCommit(void);
extern void Async_NotifyAtAbort(void);
extern void Async_Listen(char *relname, int pid);
extern void Async_Unlisten(char *relname, int pid);

extern GlobalMemory notifyContext;

#endif	 /* ASYNC_H */
