/*-------------------------------------------------------------------------
 *
 * shm.c
 *	  BeOS System V Shared Memory Emulation
 *
 * Copyright (c) 1999-2000, Cyril VELTER
 * 
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include <stdio.h>
#include <OS.h>

/* Emulating SYS shared memory with beos areas. WARNING : fork clone
areas in copy on write mode */


/* Detach from a shared mem area based on its address */
int shmdt(char* shmaddr)
{
	/* Find area id for this address */
	area_id s;
	s=area_for(shmaddr);

	/* Delete area */
	return delete_area(s);
}

/* Attach to an existing area */
int* shmat(int memId,int m1,int m2)
{
	/* Get our team id */
	thread_info thinfo;
	team_info teinfo;
	area_info ainfo; 
	
	get_thread_info(find_thread(NULL),&thinfo);
	get_team_info(thinfo.team,&teinfo);
	
	/* Get area teamid */
	if (get_area_info(memId,&ainfo)!=B_OK)
		printf("AREA %d Invalide\n",memId);
	
	if (ainfo.team==teinfo.team)
	{
		/* the area is already in our address space, just return the address */
		return (int*)ainfo.address;
	}	
	else
	{
		/* the area is not in our address space, clone it before and return the address */
		area_id narea;
		narea = clone_area(ainfo.name,&(ainfo.address),B_CLONE_ADDRESS,B_READ_AREA | B_WRITE_AREA,memId);	
		get_area_info(narea,&ainfo);	
		return (int*)ainfo.address;
	}
}

/* Control a shared mem area */
int shmctl(int shmid, int flag, struct shmid_ds* dummy)
{
	if (flag == IPC_RMID)
	{
		/* Delete the area */
		delete_area(shmid);
		return 0;
	}
	if (flag == IPC_STAT)
	{
		/* Is there a way to check existence of an area given its ID?
		 * For now, punt and assume it does not exist.
		 */
		errno = EINVAL;
		return -1;
	}
	errno = EINVAL;
	return -1;
}

/* Get an area based on the IPC key */
int shmget(int memKey,int size,int flag)
{
	char nom[50];
	void* Address;
	area_id parea;

	/* Area name */
	sprintf(nom,"SYSV_IPC_SHM : %d",memKey);

	/* Find area */
	parea=find_area(nom);
	
	/* area exist, just return its id */
	if (parea!=B_NAME_NOT_FOUND)
	{
		return parea;
	}	

	/* area does not exist and no creation is requested : error */
	if (flag==0)
	{
		return -1;
	}
	
	/* area does not exist and its creation is requested, create it (be sure to have a 4ko multiple size */
	return create_area(nom,&Address,B_ANY_ADDRESS,((size/4096)+1)*4096,B_NO_LOCK,B_READ_AREA | B_WRITE_AREA);		
}

