/*-------------------------------------------------------------------------
 *
 * ipc.h
 *	  System V IPC Emulation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/qnx4/ipc.h,v 1.7 2003/11/29 19:51:54 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef _SYS_IPC_H
#define _SYS_IPC_H

/* Common IPC definitions. */
/* Mode bits. */
#define IPC_CREAT	0001000		/* create entry if key doesn't exist */
#define IPC_EXCL	0002000		/* fail if key exists */
#define IPC_NOWAIT	0004000		/* error if request must wait */

/* Keys. */
#define IPC_PRIVATE (key_t)0	/* private key */

/* Control Commands. */
#define IPC_RMID	0			/* remove identifier */
#define IPC_STAT	1			/* get shm status */

#endif   /* _SYS_IPC_H */
