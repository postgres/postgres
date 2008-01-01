/*-------------------------------------------------------------------------
 *
 * ipc.h
 *	  POSTGRES inter-process communication definitions.
 *
 * This file is misnamed, as it no longer has much of anything directly
 * to do with IPC.	The functionality here is concerned with managing
 * exit-time cleanup for either a postmaster or a backend.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/ipc.h,v 1.74 2008/01/01 19:45:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef IPC_H
#define IPC_H


/* ipc.c */
extern bool proc_exit_inprogress;

extern void proc_exit(int code);
extern void shmem_exit(int code);
extern void on_proc_exit(void (*function) (int code, Datum arg), Datum arg);
extern void on_shmem_exit(void (*function) (int code, Datum arg), Datum arg);
extern void on_exit_reset(void);

/* ipci.c */
extern void CreateSharedMemoryAndSemaphores(bool makePrivate, int port);

#endif   /* IPC_H */
