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
#include "stdio.h"
#include "errno.h"
#include "OS.h"

// Controle d'un pool de sémaphores
// On considere que le semId utilisé correspond bien a une area de notre adress space
// Les informations du pool de sémaphore sont stockés dans cette area
int semctl(int semId,int semNum,int flag,union semun semun)
{
	
	// Recherche de l'adresse de base de l'area
	int32* Address;
	area_info info; 
//	printf("semctl : semid  %d, semnum %d, cmd %d\n",semId,semNum,flag);
	if (get_area_info(semId,&info)!=B_OK)
	{
//		printf("area not found\n");
		errno=EINVAL;
		return -1;
	}
	Address=(int32*)info.address;
	
	// semnum peut etre égal à 0
	// semun.array contient la valeur de départ du sémaphore
	
	// si flag = set_all il faut définir la valeur du sémaphore sue semun.array
	if (flag==SETALL)
	{
		long i;
//		printf("setall %d\n",Address[0]);
		for (i=0;i<Address[0];i++)
		{
			int32 cnt;
			get_sem_count(Address[i+1],&cnt);
//			printf("Set de ALl %d  %d = %d\n",Address[i+1],semun.array[i],cnt);
			cnt-=semun.array[i];
			if (cnt > 0)
				acquire_sem_etc(Address[i+1],cnt,0,0);
			if (cnt < 0)
				release_sem_etc(Address[i+1],-cnt,0);
		}
		return 1;
	}
	
	/* si flag = SET_VAL il faut définir la valeur du sémaphore sur semun.val*/
	if (flag==SETVAL)
	{
		int32 cnt;
		get_sem_count(Address[semNum+1],&cnt);
//		printf("semctl set val id : %d val : %d = %d\n",semId,semun.val,cnt);
		cnt-=semun.val;
		if (cnt > 0)
			acquire_sem_etc(Address[semNum+1],cnt,0,0);
		if (cnt < 0)
			release_sem_etc(Address[semNum+1],-cnt,0);
		return 1;
	}
	
	/* si flag=rm_id il faut supprimer le sémaphore*/
	if (flag==IPC_RMID)
	{
		long i;
		// Suppression des sémaphores (ils appartienent au kernel maintenant)
		thread_info ti;
//		printf("remove set\n");
		get_thread_info(find_thread(NULL),&ti);
		for (i=0;i<Address[0];i++)
		{
			set_sem_owner(Address[i+1],ti.team);
			delete_sem(Address[i+1]);
		}
		// Il faudrait supprimer en boucle toutes les area portant le même nom
		delete_area(semId);
		return 1;
	}
	
	/* si flag = GETNCNT il faut renvoyer le semaphore count*/
	if (flag==GETNCNT)
	{
//		printf("getncnt : impossible sur BeOS\n");
		return 0; // a faire (peut etre impossible sur Beos)
	}
	
	/* si flag = GETVAL il faut renvoyer la valeur du sémaphore*/
	if (flag==GETVAL)
	{
		int32 cnt;
		get_sem_count(Address[semNum+1],&cnt);
//		printf("semctl getval id : %d cnt : %d\n",semId,cnt);
		return cnt;
	}
//	printf("semctl erreur\n");
	return 0;
}

// L'area dans laquelle est stockée le pool est identifiée par son nom (convention à moi : SYSV_IPC_SEM : "semId)
int semget(int semKey, int semNum, int flags)
{
	char Nom[50];
	area_id parea;
	void* Address;

//	printf("semget get k: %d n: %d fl:%d\n",semKey,semNum,flags);
	// Construction du nom que doit avoir l'area
	sprintf(Nom,"SYSV_IPC_SEM : %d",semKey);

	// Recherche de l'area
	parea=find_area(Nom);

	// L'area existe
	if (parea!=B_NAME_NOT_FOUND)
	{
//		printf("area found\n");
		// On demande une creatrion d'un pool existant : erreur
		if ((flags&IPC_CREAT)&&(flags&IPC_EXCL))
		{
//			printf("creat asking exist\n");
			errno=EEXIST;
			return -1;
		}
		
		// Clone de l'area et renvoi de son ID		
		parea=clone_area(Nom,&Address,B_ANY_ADDRESS,B_READ_AREA | B_WRITE_AREA,parea);
		return parea;
	}
	// L'area n'existe pas
	else
	{
//		printf("set don't exist\n");
		// Demande de creation
		if (flags&IPC_CREAT)
		{
			int32* Address;
			thread_info ti;
			void* Ad;
			long i;

//			printf("create set\n");
			// On ne peut pas creer plus de 500 semaphores dans un pool (limite tout à fait arbitraire de ma part)
			if (semNum>500)
			{
				errno=ENOSPC;
				return -1;
			}
					
			// Creation de la zone de mémoire partagée
			parea=create_area(Nom,&Ad,B_ANY_ADDRESS,4096,B_NO_LOCK,B_READ_AREA | B_WRITE_AREA);		
			if ((parea==B_BAD_VALUE)|| (parea==B_NO_MEMORY)||(parea==B_ERROR))
			{
				errno=ENOMEM;
				return -1;
			}
			Address=(int32*)Ad;
			Address[0]=semNum;
			for (i=1;i<=Address[0];i++)
			{
				// Creation des sémaphores 1 par 1
				Address[i]=create_sem(0,Nom);
				
				if ((Address[i]==B_BAD_VALUE)|| (Address[i]==B_NO_MEMORY)||(Address[i]==B_NO_MORE_SEMS))
				{
					errno=ENOMEM;
					return -1;
				}
			}

//			printf("returned %d\n",parea);
			return parea;
		}
		// Le pool n'existe pas et pas de demande de création
		else
		{
//			printf("set does not exist no creat requested\n");
			errno=ENOENT;
			return -1;
		}
	}
}

// Opération sur le pool de sémaphores
int semop(int semId, struct sembuf *sops, int nsops)
{
	// Recherche de l'adresse du pool
	int32* Address;
   	area_info info; 
	long i;

//	printf("semop id : %d n: %d\n",semId,sops->sem_op);
	get_area_info(semId,&info);
	Address=(int32*)info.address;
	if ((semId==B_BAD_VALUE)||(semId==B_NO_MEMORY)||(semId==B_ERROR))
	{
		errno=EINVAL;
		return -1;
	}

	// Execution de l'action
	for(i=0;i<nsops;i++)
	{

//		printf("semid %d, n %d\n",Address[sops[i].sem_num+1],sops[i].sem_op);
		if (sops[i].sem_op < 0)
		{
			acquire_sem_etc(Address[sops[i].sem_num+1],-sops[i].sem_op,0,0);
		}
		if (sops[i].sem_op > 0)
		{
			release_sem_etc(Address[sops[i].sem_num+1],sops[i].sem_op,0);
		}
	}
	return 0;
}
