/*-------------------------------------------------------------------------
 *
 * async.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: async.h,v 1.3 1996/11/06 10:29:30 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	ASYNC_H
#define	ASYNC_H


extern void Async_NotifyHandler(SIGNAL_ARGS);
extern void Async_Notify(char *relname);
extern void Async_NotifyAtCommit(void);
extern void Async_NotifyAtAbort(void);
extern void Async_Listen(char *relname, int pid);
extern void Async_Unlisten(char *relname, int pid);
extern void Async_UnlistenOnExit(int code, char *relname);

extern GlobalMemory notifyContext;
extern void Async_NotifyFrontEnd(void);

#endif	/* ASYNC_H */
