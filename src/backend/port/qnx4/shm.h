/*-------------------------------------------------------------------------
 *
 * shm.h
 *	  System V Shared Memory Emulation
 *
 * Copyright (c) 1999, repas AEG Automation GmbH
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/port/qnx4/Attic/shm.h,v 1.7 2001/11/08 20:37:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef _SYS_SHM_H
#define _SYS_SHM_H

#include <sys/ipc.h>

#ifdef	__cplusplus
extern		"C"
{
#endif

#define SHM_R	0400			/* read permission */
#define SHM_W	0200			/* write permission */

struct shmid_ds
{
	int			dummy;
	int			shm_nattch;
};

extern void *shmat(int shmid, const void *shmaddr, int shmflg);
extern int	shmdt(const void *addr);
extern int	shmctl(int shmid, int cmd, struct shmid_ds * buf);
extern int	shmget(key_t key, size_t size, int flags);

#ifdef	__cplusplus
}
#endif

#endif   /* _SYS_SHM_H */
