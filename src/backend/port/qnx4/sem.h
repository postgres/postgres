/*-------------------------------------------------------------------------
 *
 * sem.h
 *	  System V Semaphore Emulation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/sem.h,v 1.1 1999/12/16 16:52:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef	_SYS_SEM_H
#define	_SYS_SEM_H

#include <sys/ipc.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
*       Semctl Command Definitions.
 */

#define GETNCNT 3	/* get semncnt */
#define GETPID  4	/* get sempid */
#define GETVAL  5	/* get semval */
#define GETALL  6	/* get all semval's */
#define GETZCNT 7	/* get semzcnt */
#define SETVAL  8	/* set semval */
#define SETALL  9	/* set all semval's */

/*
* User semaphore template for semop system calls.
 */

struct sembuf {
        ushort_t        sem_num;        /* semaphore # */
        short           sem_op;         /* semaphore operation */
        short           sem_flg;        /* operation flags */
};

extern int semctl( int semid, int semnum, int cmd, /*...*/union semun arg );
extern int semget( key_t key, int nsems, int semflg );
extern int semop( int semid, struct sembuf *sops, size_t nsops );

#ifdef	__cplusplus
}
#endif

#endif /* _SYS_SEM_H */
