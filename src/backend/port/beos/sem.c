/*-------------------------------------------------------------------------
 *
 * sem.c
 *	  BeOS System V Semaphores Emulation
 *
 * Copyright (c) 1999-2000, Cyril VELTER
 * 
 *-------------------------------------------------------------------------
 */


#include "postgres.h"
#include <stdio.h>
#include <errno.h>
#include <OS.h>
#include "utils/elog.h"


/* Control of a semaphore pool. The pool is an area in which we stored all
the semIds of the pool. The first 4 bytes are the number of semaphore allocated
in the pool followed by SemIds */ 

int semctl(int semId,int semNum,int flag,union semun semun)
{
	int32* Address;
	area_info info; 

	/* Try to find the pool */
	if (get_area_info(semId,&info)!=B_OK)
	{
		/* pool is invalid (BeOS area id is invalid) */
		errno=EINVAL;
		return -1;
	}
	
	/* Get the pool address */
	Address=(int32*)info.address;
	
	
	/* semNum might be 0 */
	/* semun.array contain the sem initial values */
	
	/* Fix the count of all sem of the pool to semun.array */
	if (flag==SETALL)
	{
		long i;
		for (i=0;i<Address[0];i++)
		{
			int32 cnt;
			/* Get the current count */
			get_sem_count(Address[i+1],&cnt);

			/* Compute and set the new count (relative to the old one) */
			cnt-=semun.array[i];
			if (cnt > 0)
				while(acquire_sem_etc(Address[i+1],cnt,0,0)==B_INTERRUPTED);
			if (cnt < 0)
				release_sem_etc(Address[i+1],-cnt,0);
		}
		return 1;
	}
	
	/* Fix the count of one semaphore to semun.val */
	if (flag==SETVAL)
	{
		int32 cnt;
		/* Get the current count */
		get_sem_count(Address[semNum+1],&cnt);

		/* Compute and set the new count (relative to the old one) */
		cnt-=semun.val;
		if (cnt > 0)
			while(acquire_sem_etc(Address[semNum+1],cnt,0,0)==B_INTERRUPTED);
		if (cnt < 0)
			release_sem_etc(Address[semNum+1],-cnt,0);
		return 1;
	}
	
	/* Delete the pool */
	if (flag==IPC_RMID)
	{
		long i;

		thread_info ti;
		get_thread_info(find_thread(NULL),&ti);
		
		/* Loop over all semaphore to delete them */
		for (i=0;i<Address[0];i++)
		{
			/* Don't remember why I do that */
			set_sem_owner(Address[i+1],ti.team);
			
			/* Delete the semaphore */
			delete_sem(Address[i+1]);

			/* Reset to an invalid semId (in case other process try to get the infos from a cloned area */
			Address[i+1]=0;
		}
		
		/* Set the semaphore count to 0 */
		Address[0]=0;
		
		/* Delete the area (it might be cloned by other process. Let them live with it,
		in all cases semIds are 0 so if another process try to use it, it will fail */
		delete_area(semId);

		return 1;
	}
	
	/* Get the current semaphore count */
	if (flag==GETNCNT)
	{
		/* TO BE IMPLEMENTED */
		elog(ERROR,"beos : semctl error : GETNCNT not implemented");
		return 0;
	}
	
	/* Get the current semaphore count of the first semaphore in the pool */
	if (flag==GETVAL)
	{
		int32 cnt;
		get_sem_count(Address[semNum+1],&cnt);
		return cnt;
	}

	elog(ERROR,"beos : semctl error : unknown flag");

	return 0;
}

/* Find a pool id based on IPC key */
int semget(int semKey, int semNum, int flags)
{
	char Nom[50];
	area_id parea;
	void* Address;

	/* Name of the area to find */
	sprintf(Nom,"SYSV_IPC_SEM : %d",semKey);

	/* find area */
	parea=find_area(Nom);

	/* Test of area existance */
	if (parea!=B_NAME_NOT_FOUND)
	{
		/* Area exist and creation is requested, error */
		if ((flags&IPC_CREAT)&&(flags&IPC_EXCL))
		{
			errno=EEXIST;
			return -1;
		}
		
		/* Get an area clone (in case it's not in our address space) */
		/* TODO : a check of address space might be done to avoid duplicate areas in the same address space*/
		parea=clone_area(Nom,&Address,B_ANY_ADDRESS,B_READ_AREA | B_WRITE_AREA,parea);
		return parea;
	}
	else
	{
		/* Area does not  exist, but creation is requested, so create it */
		if (flags&IPC_CREAT)
		{
			int32* Address;
			void* Ad;
			long i;

			/* Limit to 500 semaphore in a pool */
			if (semNum>500)
			{
				errno=ENOSPC;
				return -1;
			}

			/* Create the shared memory area which will hold the pool */
			parea=create_area(Nom,&Ad,B_ANY_ADDRESS,4096,B_NO_LOCK,B_READ_AREA | B_WRITE_AREA);		
			if ((parea==B_BAD_VALUE)|| (parea==B_NO_MEMORY)||(parea==B_ERROR))
			{
				errno=ENOMEM;
				return -1;
			}
			
			/* fill up informations (sem number and sem ids) */
			Address=(int32*)Ad;
			Address[0]=semNum;
			for (i=1;i<=Address[0];i++)
			{
				/* Create the semaphores */
				Address[i]=create_sem(0,Nom);
				
				if ((Address[i]==B_BAD_VALUE)|| (Address[i]==B_NO_MEMORY)||(Address[i]==B_NO_MORE_SEMS))
				{
					errno=ENOMEM;
					return -1;
				}
			}

			return parea;
		}
		else
		{
			/* Area does not exist and no creation is requested */
			errno=ENOENT;
			return -1;
		}
	}
}

/* Acquire or release in the semaphore pool */
int semop(int semId, struct sembuf *sops, int nsops)
{
	int32* Address; /*Pool address*/
   	area_info info; 
	long i;

	/* Get the pool address (semId IS an area id) */
	get_area_info(semId,&info);
	Address=(int32*)info.address;
	
	/* Check the validity of semId (it should be an area id) */
	if ((semId==B_BAD_VALUE)||(semId==B_NO_MEMORY)||(semId==B_ERROR))
	{
		errno=EINVAL;
		return -1;
	}

	/* Perform acquire or release */
	for(i=0;i<nsops;i++)
	{
		/* For each sem in the pool, check the operation to perform */
		if (sops[i].sem_op < 0)
		{
			/* Try acuiring the semaphore till we are not inteerupted by a signal */
			while (acquire_sem_etc(Address[sops[i].sem_num+1],-sops[i].sem_op,0,0)==B_INTERRUPTED);
		}
		if (sops[i].sem_op > 0)
		{
			release_sem_etc(Address[sops[i].sem_num+1],sops[i].sem_op,0);
		}
	}
	return 0;
}
