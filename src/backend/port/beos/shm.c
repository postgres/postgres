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

// Detachement d'une zone de mémoire partagée
// On detruit le clone de l'area dans notre adress-space
int shmdt(char* shmaddr)
{
	// Recherche de l'id de l'area présente à cette adresse
	area_id s;
	s=area_for(shmaddr);
//	printf("detach area %d\n",s);
	
	// Suppression de l'area
	return delete_area(s);
}

// Attachement à une zone de mémoire partagée
// L'area doit bien partie de notre adress-space et on retourne directement l'adress
int* shmat(int memId,int m1,int m2)
{
//	printf("shmat %d %d %d\n",memId,m1,m2);

	// Lecture de notre team_id
	thread_info thinfo;
	team_info teinfo;
	area_info ainfo; 
	
	get_thread_info(find_thread(NULL),&thinfo);
	get_team_info(thinfo.team,&teinfo);
	
	// Lecture du teamid de l'area
	if (get_area_info(memId,&ainfo)!=B_OK)
		printf("AREA %d Invalide\n",memId);
	
	if (ainfo.team==teinfo.team)
	{
		//retour de l'adresse
//		printf("attach area %d add %d\n",memId,ainfo.address);
		return (int*)ainfo.address;
	}	
	else
	{
		// Clone de l'area
		area_id narea;
		narea = clone_area(ainfo.name,&(ainfo.address),B_CLONE_ADDRESS,B_READ_AREA | B_WRITE_AREA,memId);	
		get_area_info(narea,&ainfo);	
//		printf("attach area %d in %d add %d\n",memId,narea,ainfo.address);
		return (int*)ainfo.address;
	}
}

// Utilisé uniquement pour supprimer une zone de mémoire partagée
// On fait la meme chose que le detach mais avec un id direct
int shmctl(int shmid,int flag, struct shmid_ds* dummy)
{
//	printf("shmctl %d %d \n",shmid,flag);
	delete_area(shmid);
	return 0;
}

// Recupération d'une area en fonction de sa référence
// L'area source est identifiée par son nom (convention à moi : SYSV_IPC_SHM : "memId)
int shmget(int memKey,int size,int flag)
{
	int32 n_size;
	char nom[50];
	area_id parea;
	void* Address;
	area_id a;
	
	n_size=((size/4096)+1)*4096;

//	printf("shmget %d %d %d %d\n",memKey,size,flag,nsize);

	// Determination du nom que doit avoir l'area
	sprintf(nom,"SYSV_IPC_SHM : %d",memKey);


	// Recherche de cette area
	parea=find_area(nom);
	
	// L'area existe
	if (parea!=B_NAME_NOT_FOUND)
	{
//		printf("area found\n");
		return parea;
	}	

	// L'area n'existe pas et on n'en demande pas la création : erreur
	if (flag==0)
	{
//		printf("area %s not found\n",nom);
		return -1;
	}
	
	// L'area n'existe pas mais on demande sa création
	a=create_area(nom,&Address,B_ANY_ADDRESS,n_size,B_NO_LOCK,B_READ_AREA | B_WRITE_AREA);		
//	printf("area %s : %d created addresse %d\n",nom,a,Address);
	return a;
}

