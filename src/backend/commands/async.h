/*-------------------------------------------------------------------------
 *
 * async.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: async.h,v 1.1.1.1 1996/07/09 06:21:19 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	ASYNC_H
#define	ASYNC_H

#include "nodes/memnodes.h"

#if defined(PORTNAME_linux)
extern void Async_NotifyHandler(int);
#else
extern void Async_NotifyHandler(void);
#endif
extern void Async_Notify(char *relname);
extern void Async_NotifyAtCommit(void);
extern void Async_NotifyAtAbort(void);
extern void Async_Listen(char *relname, int pid);
extern void Async_Unlisten(char *relname, int pid);
extern void Async_UnlistenOnExit(int code, char *relname);

extern GlobalMemory notifyContext;
extern void Async_NotifyFrontEnd(void);

#endif	/* ASYNC_H */
