/*-------------------------------------------------------------------------
 *
 * support.c
 *	  BeOS Support functions
 *
 * Copyright (c) 1999-2000, Cyril VELTER
 * 
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

/* Support Globals */
char* self_binary=NULL;
port_id beos_dl_port_in=0;
port_id beos_dl_port_out=0;
sem_id beos_shm_sem;

image_id beos_dl_open(char * filename)
{
	image_id im;

	/* Start the support server */
	if (self_binary==NULL)
	{
		/* Can't start support server without binary name */		
		elog(NOTICE, "Error loading BeOS support server : can't find binary");				
		return B_ERROR;
	}
	else
	{
		/* If a port doesn't exist, lauch support server */ 
		if ((beos_dl_port_in<=0)||(beos_dl_port_out<=0))
		{
			/* Create communication port */
			beos_dl_port_in=create_port(50,"beos_support_in");
			beos_dl_port_out=create_port(50,"beos_support_in");


			if ((beos_dl_port_in<=0)||(beos_dl_port_out<=0))
			{
				elog(NOTICE, "Error loading BeOS support server : can't create communication ports");				
				return B_ERROR;
			}
			else
			{
				char Cmd[4000]; 
			
				/* Build arg list */
				sprintf(Cmd,"%s -beossupportserver %d %d &",self_binary,(int)beos_dl_port_in,(int)beos_dl_port_out);
	
				/* Lauch process */
				system(Cmd);
			}
		}
	}
	
	/* Add-on loading */
	
	/* Send command '1' (load) to the support server */
	write_port(beos_dl_port_in,1,filename,strlen(filename)+1);
	
	/* Read Object Id */
	read_port(beos_dl_port_out,&im,NULL,0);

	/* Checking integrity */
	if (im<0)
	{	
		elog(NOTICE, "Can't load this add-on ");
		return B_ERROR;	
	}
	else
	{
		/* Map text and data segment in our address space */
		char datas[4000];
		int32 area;
		int32 resu;
		void* add;
	
		/* read text segment id and address */
		read_port(beos_dl_port_out,&area,datas,4000);
		read_port(beos_dl_port_out,(void*)&add,datas,4000);
		/* map text segment in our address space */
		resu=clone_area(datas,&add,B_EXACT_ADDRESS,B_READ_AREA|B_WRITE_AREA,area);
		if (resu<0)
		{
			/* If we can't map, we are in reload case */
			/* delete the mapping */
			resu=delete_area(area_for(add));
			/* Remap */
			resu=clone_area(datas,&add,B_EXACT_ADDRESS,B_READ_AREA|B_WRITE_AREA,area);
			if (resu<0)
			{
				elog(NOTICE, "Can't load this add-on : map text error");
			}
		}
		
		/* read text segment id and address */
		read_port(beos_dl_port_out,&area,datas,4000);
		read_port(beos_dl_port_out,(void*)&add,datas,4000);
		/* map text segment in our address space */
		resu=clone_area(datas,&add,B_EXACT_ADDRESS,B_READ_AREA|B_WRITE_AREA,area);
		if (resu<0)
		{
			/* If we can't map, we are in reload case */
			/* delete the mapping */
			resu=delete_area(area_for(add));
			/* Remap */
			resu=clone_area(datas,&add,B_EXACT_ADDRESS,B_READ_AREA|B_WRITE_AREA,area);
			if (resu<0)
			{
				elog(NOTICE, "Can't load this add-on : map data error");
			}
		}
		
		return im;
	}
}

status_t beos_dl_close(image_id im)
{
	/* unload add-on */
	int32 resu;
	write_port(beos_dl_port_in,2,&im,4);
	read_port(beos_dl_port_out,&resu,NULL,0);
	return resu;
}

/* Main support server loop */

void beos_startup(int argc,char** argv)
{
	if (strlen(argv[0]) >= 10 && !strcmp(argv[0] + strlen(argv[0]) - 10, "postmaster"))
	{
		/* Shared memory cloning protection semaphore */
		beos_shm_sem=create_sem(1,"beos_shm_sem");	
	}

	if (argc > 1 && strcmp(argv[1], "-beossupportserver") == 0)
	{
		port_id port_in;
		port_id port_out;
		
		/* Get back port ids from arglist */
		sscanf(argv[2],"%d",(int*)(&port_in));
		sscanf(argv[3],"%d",(int*)(&port_out));
			
		/* Main server loop */
		for (;;)
		{ 
			int32 opcode=0;
			char datas[4000];
	
			/* Wait for a message from the backend :
			1 : load a shared object 
			2 : unload a shared object
			any other : exit support server */
			read_port(port_in,&opcode,datas,4000);
	
			switch(opcode)
			{
				image_id addon;
				image_info info_im;
				area_info info_ar;
	
				/* Load Add-On */
				case 1 :
		
					/* Load shared object */
					addon=load_add_on(datas);
		
					/* send back the shared object Id */
					write_port(port_out,addon,NULL,0);
					
					/* Get Shared Object infos */
					get_image_info(addon,&info_im);
	
					/* get text segment info */
					get_area_info(area_for(info_im.text),&info_ar);
					/* Send back area_id of text segment */
					write_port(port_out,info_ar.area,info_ar.name,strlen(info_ar.name)+1);
					/* Send back real address of text segment */
					write_port(port_out,(int)info_ar.address,info_ar.name,strlen(info_ar.name)+1);
			
					
					/* get data segment info */
					get_area_info(area_for(info_im.data),&info_ar);
					/* Send back area_id of data segment */
					write_port(port_out,info_ar.area,info_ar.name,strlen(info_ar.name)+1);
					/* Send back real address of data segment */
					write_port(port_out,(int)info_ar.address,info_ar.name,strlen(info_ar.name)+1);
				break;
				/* UnLoad Add-On */
				case 2 :
					/* Unload shared object and send back the result of the operation */
					write_port(port_out,unload_add_on(*((int*)(datas))),NULL,0);
				break;
				/* Cleanup and exit */
				default:
					/* Free system resources */
					delete_port(port_in);
					delete_port(port_out);
					/* Exit */
					exit(0);
				break;
			}
		}
		/* Never be there */
		exit(1);
	}
}


void beos_backend_startup(char * binary)
{
	team_id ct;
	thread_info inft;
	char nom[50];
	char nvnom[50];
	area_info inf;
	int32 cook=0;

	/* remember full path binary name to load dl*/
	self_binary=strdup(binary);

	/* find the current team */
	get_thread_info(find_thread(NULL),&inft);
	ct=inft.team;
	
	/* find all area with a name begining by pgsql and destroy / clone then */

	/* This operation must be done by only one backend at a time */
	if(acquire_sem(beos_shm_sem)==B_OK)
	{
		while (get_next_area_info(0, &cook, &inf) == B_OK)
		{
			strcpy(nom,inf.name);
			strcpy(nvnom,inf.name);
			nom[9]=0;
			nvnom[5]='i';
			if (!strcmp(nom,"SYSV_IPC_"))
			{
				void* add;
				area_id ar;
				add=inf.address;
				delete_area(inf.area);
				ar=find_area(inf.name);
				clone_area(nvnom,&add,B_CLONE_ADDRESS,B_READ_AREA|B_WRITE_AREA,ar);
			}
		} 
		release_sem(beos_shm_sem);
	}
	else
	{
		/* Fatal error, exiting with error */
		exit(1);
	}
}
