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
port_id beos_dl_port_in=0;
port_id beos_dl_port_out=0;
sem_id beos_shm_sem;

/* Global var containing the postgres path */
extern char		pg_pathname[];


/* Shared library loading doesn't work after fork in beos. The solution is to use an exact
copy of the process and use it to perform the loading, then just map the Text and Data segment
of the add-on in our address space. Both process must have the exact same memory mapping, so
we use the postgres executable. When it's lauched with the -beossupportserver parameter, the
postgres executable just run a loop to wait command on a port. Its only action is to load the addon,
the beos_dl_open will then remap the good areas in the backend address space. */


image_id beos_dl_open(char * filename)
{
	image_id im;

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
			sprintf(Cmd,"%s -beossupportserver %d %d &",pg_pathname,(int)beos_dl_port_in,(int)beos_dl_port_out);

			/* Lauch process */
			system(Cmd);
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
		/* We are in the postmaster, create the protection semaphore for shared mem remapping */
		beos_shm_sem=create_sem(1,"beos_shm_sem");	
	}

	if (argc > 1 && strcmp(argv[1], "-beossupportserver") == 0)
	{
		/* We are in the support server, run it ... */

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



/* The behavior of fork is borken on beos regarding shared memory. In fact 
all shared memory areas are clones in copy on write mode in the new process.

We need to do a remapping of these areas. Just afer the fork we performe the
following actions :

	* Find all areas with a name begining by SYS_V_IPC_ in our process
	(areas created by the SYSV IPC emulation functions). The name is 
	followed by the IPC KEY in decimal format 
	
	* For each area we do :
	
		* 1 : Get its name
		* 2 : destroy it
		* 3 : find another area with the exact same name	
		* 4 : clone it in our address space with a different name
		
	There is a race condition in 3-4 : if there two fork in a very short
	time, in step 3 we might end up with two areas with the same name, and no
	possibility to find the postmaster one. So the whole process is protected
	by a semaphore which is acquires just before the fork and released in case
	of fork failure or just after the end of the remapping.*/
	
void beos_before_backend_startup(void)
{
	/* Just before forking, acquire the semaphore */	
	if(acquire_sem(beos_shm_sem)!=B_OK)
		exit(1); 		/* Fatal error, exiting with error */
}

void beos_backend_startup_failed(void)
{
	/* The foek failed, just release the semaphore */
	release_sem(beos_shm_sem);
}


void beos_backend_startup(void)
{
	char nom[50];
	char nvnom[50];
	area_info inf;
	int32 cook=0;

	/* Perform the remapping process */

	/* Loop in all our team areas */
	while (get_next_area_info(0, &cook, &inf) == B_OK)
	{
		strcpy(nom,inf.name);
		strcpy(nvnom,inf.name);
		nom[9]=0;
		nvnom[5]='i';
		/* Is it a SYS V area ? */
		if (!strcmp(nom,"SYSV_IPC_"))
		{
			void* area_address;
			area_id area_postmaster;
			/* Get the area address */
			area_address=inf.address;
			/* Destroy the bad area */
			delete_area(inf.area);
			/* Find the postmaster area */
			area_postmaster=find_area(inf.name);
			/* Clone it at the exact same address */
			clone_area(nvnom,&area_address,B_CLONE_ADDRESS,B_READ_AREA|B_WRITE_AREA,area_postmaster);
		}
	} 

	/* remapping done release semaphore to allow other backend to startup */

	release_sem(beos_shm_sem);
}
