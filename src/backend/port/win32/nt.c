#include <windows.h>
#include <time.h>
#include "postgres.h"
#include "storage/ipc.h"

/* The name of the Postgres 95 ipc file mapping object */
#define IPC_NAME	"PG95_IPC"

/* The name of the Postgres 95 ipc file mapping object semaphore */
#define IPC_SEM_NAME	"PG95_IPC_SEM"

/* The maximum length of a shared memory object name */
#define IPC_MAX_SHMEM_NAME	32

/* The maximum number of emulated Unix shared memory segments */
#define IPC_NMAXSHM	10

/* The Maximum number of elements in a semaphore set. Note that this
** is just a guess.
*/
#define IPC_NMAXSEMGRP	7

/* The various states of a semaphore */
#define SIGNALED	1
#define UNSIGNALED 	0
#define UNUSED		-1

/* The security attribute structure necessary for handles to be inhereted */
SECURITY_ATTRIBUTES sec_attrib = { sizeof (LPSECURITY_ATTRIBUTES),
	NULL, TRUE};

/*
Postgres95 uses semaphores and shared memory. Both are provided by
Unix and NT, although NT uses a different method for referencing
them. Rather than changing the function calls used by Postgres95
to use NT system services, we've written code to emulate the Unix
system calls. We deliberately don't do a complete emulation of the
Unix calls, partly because it doesn't appear possible, but also
because only a few options of the Unix calls are actually used by
Postgres95.

The most noticable difference between the way Unix and NT use semaphores
is that the central entity on Unix is a semaphore set consisting of
potientially many actual semaphores whereas on NT a semaphore handle
represents just one actual semaphore. Furthermore, a Unix semaphore set
is identified by one semaphore id no matter how many elements there
are in the set. Given a Unix semaphore id, the Unix API provides a way
to index into the set to reference a specific semaphore.

You might think that since both a semaphore id and a semaphore handle
is just an integer there won't be any changes necessary to the Postgres95
code to deal with NT semaphores. If it weren't for the existence of
multi-semaphore semaphore sets this would be true.

To handle semaphore sets a fixed-size table, whose size is partially
based on the sum of the maximum number of semaphores times the maximum
number of semaphores per semaphore set, is created and kept in shared
memory that is visable to every backend started by the Postmaster.

Each semaphore set entry consists of an arbitrary key value, which serves
to identify the semaphore set, and IPC_NMAXSEMGRP array elements to
store the NT semaphore handles representing the NT semaphore used for
the semaphore set. Semaphore IDs are just indices into this table.
In order to distinguish occupied entries in this table -1 is always
considered an invalid semaphore ID.

This table is also used to store information about shared memory
segments. Fortunately, there is a one-to-one mapping between Unix
shared memory IDs and NT shared memory handles so the code to emulate
Unix shared memory is simple.
*/

/* We need one of these for each emulated semaphore set */
struct Pg_sem
{
	key_t	Pg_sem_key;
	HANDLE	Pg_sem_handle[IPC_NMAXSEMGRP];
	int	Pg_sem_nsems;
};

/* We need one of these for each emulated shared memory segment */
struct Pg_shm
{
	key_t	Pg_shm_key;
	HANDLE  Pg_shm_handle;
};

/* This structure is what's stored in shared memory. Note that
** since both the shared memory and semaphore data is in the same
** table, and the table is protected by a single NT semaphore, there's
** a chance that semaphore manipulation could be slowed down by
** shared memory manipulation, and vice versa. But, since both are
** allocated primarily when the Postmaster starts up, which isn't time
** critical, I don't think this will prove to be a problem.
*/

static struct Pg_shared
{
	int Pg_next_sem;
	int Pg_next_shm;
	struct Pg_sem Pg_sem[IPC_NMAXSEM];
	struct Pg_shm Pg_shm[IPC_NMAXSHM];
} *Pg_shared_ptr;

/* The semaphore that protects the shared memory table */
HANDLE Pg_shared_hnd;

/*
** Perform a semaphore operation. We're passed a semaphore set id,
** a pointer to an array of sembuf structures, and the number
** of elements in the array. Each element in the sembuf structure
** describes a specific semaphore within the semaphore set and the
** operation to perform on it.
*/

int
semop(int semid, struct sembuf *sops, u_int nsops)
{
	u_int i;
	int result;
	HANDLE hndl;

	/* Go through all the sops structures */
	for (i = 0; i < nsops; i++)
	{
		struct sembuf *sptr;
		int semval;
		int av_sem_op;

		sptr = &sops[i];
		/*
		printf("performing %d in sem # %d\n", sptr->sem_op, sptr->sem_num);
		*/

		/*
		** Postgres95 uses -255 to represent a lock request
		** and 255 to show a lock release. Changing these values
		** to -1 and 1 make it easier to keep track of the state
		** of the semaphore.
		*/
		if (sptr->sem_op == -255)
			sptr->sem_op = -1;
		else if (sptr->sem_op == 255)
			sptr->sem_op = 1;
		else
			printf("invalid sem_op %d\n", sptr->sem_op);

		_get_ipc_sem();
		hndl = Pg_shared_ptr->Pg_sem[semid].Pg_sem_handle[sptr->sem_num];
		_rel_ipc_sem();
		semval = _get_sem_val(hndl);

		if (sptr->sem_op == 0)
		{
			if (semval == UNSIGNALED)
				return(semval);
			else
			{
				if (sptr->sem_flg & IPC_NOWAIT)
					return(SIGNALED);
				else
					result = WaitForSingleObject(hndl, 5000);
			}
		}

		av_sem_op = abs(sptr->sem_op);

		/* If a lock is being attempted */
		if (sptr->sem_op < 0)
		{
			if (semval >= av_sem_op)
			{
				semval -= av_sem_op;
				if (semval <= UNSIGNALED)
					result = WaitForSingleObject(hndl, 5000);
			}
			else
			{
				if (sptr->sem_flg & IPC_NOWAIT)
					return(SIGNALED);
				else
					result = WaitForSingleObject(hndl, 5000);
			}
		}

		/* If a lock is being released */
		if (sptr->sem_op > 0)
		{
			semval += av_sem_op;
			if (semval > 0)
				ReleaseSemaphore(hndl, 1, NULL);
		}
	}
}

int
semget(key_t key, int nsems, int semflg)
{
	int id, new_sem, ret_val;

	/* If nmsems is 0 then assume that we're just checking whether
	** the semaphore identified by key exists. Assume that
	** if key is IPC_PRIVATE that this should always fail.
	*/
	if (nsems == 0)
	{
		if (key == IPC_PRIVATE)
			ret_val = -1;
		else
		{
			_get_ipc_sem();
			id = _get_sem_id(key);
			_rel_ipc_sem();
			ret_val = id;
		}
		return(ret_val);
	}

	/* See if there's already a semaphore with the key.
	** If not, record the key, allocate enough space for the
	** handles of the semaphores, and then create the semaphores.
	*/
	_get_ipc_sem();
	id = _get_sem_id(key);
	if (id == UNUSED)
	{
		register int i;
		struct Pg_sem *pg_ptr;

		new_sem = Pg_shared_ptr->Pg_next_sem++;

		pg_ptr = &(Pg_shared_ptr->Pg_sem[new_sem]);
		pg_ptr->Pg_sem_key = key;
		pg_ptr->Pg_sem_nsems = nsems;

		for (i = 0; i < nsems; i++)
			pg_ptr->Pg_sem_handle[i] = CreateSemaphore(&sec_attrib, 1, 255, NULL);
		ret_val = new_sem;
	}
	else
		ret_val = id;
	_rel_ipc_sem();
	return(ret_val);
}

/* These next two functions could be written as one function, although
** doing so would require some additional logic.
*/

/* Given a semaphore key, return the corresponding id.
** This function assumes that the shared memory table is being
** protected by the shared memory table semaphore.
*/
_get_sem_id(key_t key)
{
	register int i;

	/* Go through the shared memory table looking for a semaphore
	** whose key matches what we're looking for
	*/
	for (i = 0; i < Pg_shared_ptr->Pg_next_sem; i++)
		if (Pg_shared_ptr->Pg_sem[i].Pg_sem_key == key)
			return(i);

	/* Return UNUSED if we didn't find a match */
	return(UNUSED);
}

/* Given a shared memory key, return the corresponding id
** This function assumes that the shared memory table is being
** protected by the shared memory table semaphore.
*/
_get_shm_id(key_t key)
{
	register int i;

	/* Go through the shared memory table looking for a semaphore
	** whose key matches what we're looking for
	*/
	for (i = 0; i < Pg_shared_ptr->Pg_next_shm; i++)
		if (Pg_shared_ptr->Pg_shm[i].Pg_shm_key == key)
			return(i);

	/* Return UNUSED if we didn't find a match */
	return(UNUSED);
}

int
semctl(int semid, int semnum, int cmd, void *y)
{
	int old_val;
	HANDLE hndl;

	switch (cmd)
	{
	case SETALL:
	case SETVAL:
		/* We can't change the value of a semaphore under
		** NT except by releasing it or waiting for it.
		*/
		return(0);

	case GETVAL:
		_get_ipc_sem();
		hndl = Pg_shared_ptr->Pg_sem[semid].Pg_sem_handle[semnum];
		_rel_ipc_sem();
		old_val = _get_sem_val(hndl);
		return(old_val);
	}
}

/* Get the current value of the semaphore whose handle is passed in hnd
** This function does NOT assume that the shared memory table is being
** protected by the shared memory table semaphore.
*/

int
_get_sem_val(HANDLE hnd)
{
	DWORD waitresult;

	/* Try to get the semaphore */
	waitresult = WaitForSingleObject(hnd, 0L);

	/* Check what the value of the semaphore was */
	switch(waitresult)
	{
	/* The semaphore was signaled so we just got it.
	** Since we don't really want to keep it, since we just
	** wanted to test its value, go ahead and release it.
	*/
	case WAIT_OBJECT_0:
		ReleaseSemaphore(hnd, 1, NULL);
		return(SIGNALED);

	/* The semaphore was non-signaled meaning someone else had it. */
	case WAIT_TIMEOUT:
		return(UNSIGNALED);
	}
}

int
shmget(key_t key, uint32 size, int flags)
{
	HANDLE hnd;
	char name[IPC_MAX_SHMEM_NAME];
	int id;

	/* Get the handle for the key, if any. */
	_get_ipc_sem();
	id = _get_shm_id(key);
	_rel_ipc_sem();

	/* If we're really going to create a new mapping */
	if (flags != 0)
	{
		/* if the key is already being used return an error */
		if (id != UNUSED)
			return(-1);

		/* convert the key to a character string */
		sprintf(name, "%d", key);
	
		hnd = CreateFileMapping((HANDLE)0xffffffff,
			&sec_attrib, PAGE_READWRITE,
			0, size, name);
	
		if (hnd == NULL)
			return(-1);
		else
		{
			int new_ipc;
			struct Pg_shm *pg_ptr;

			_get_ipc_sem();
			new_ipc = Pg_shared_ptr->Pg_next_shm++;

			pg_ptr = &(Pg_shared_ptr->Pg_shm[new_ipc]);
			pg_ptr->Pg_shm_key = key;
			pg_ptr->Pg_shm_handle = hnd;
			_rel_ipc_sem();
			return(new_ipc);
		}
	}

	/* flags is 0 so we just want the id for the existing mapping */
	else
		return(id);
}

shmdt(char *shmaddr)
{
	UnmapViewOfFile(shmaddr);
}

int
shmctl(IpcMemoryId shmid, int cmd, struct shmid_ds *buf)
{
	int x = 0;

	if (cmd == IPC_RMID)
	{
		_get_ipc_sem();
		CloseHandle(Pg_shared_ptr->Pg_shm[shmid].Pg_shm_handle);
		_rel_ipc_sem();
		return(0);
	}
	x = x / x;
}

/* Attach to the already created shared memory segment */
LPVOID *
shmat(int shmid, void *shmaddr, int shmflg)
{
	LPVOID *ret_addr;

	_get_ipc_sem();
	ret_addr = MapViewOfFile(Pg_shared_ptr->Pg_shm[shmid].Pg_shm_handle,
		FILE_MAP_ALL_ACCESS, 0, 0, 0);
	_rel_ipc_sem();
	if (ret_addr == NULL)
	{
		int jon;

		jon = GetLastError();
	}
	return(ret_addr);
}

/* This is the function that is called when the postmaster starts up.
** It is here that the shared memory table is created. Also, create
** the semaphore that will be used to protect the shared memory table.
** TODO - do something with the return value.
*/
_nt_init()
{
	HANDLE hnd;
	int size = sizeof (struct Pg_shared);

	/* Create the file mapping for the shared memory to be
	** used to store the ipc table.
	*/
	hnd = CreateFileMapping((HANDLE)0xffffffff,
		&sec_attrib, PAGE_READWRITE,
		0, size, IPC_NAME);

	if (hnd == NULL)
	{
		size = GetLastError();
		return(-1);
	}

	Pg_shared_hnd = CreateSemaphore(&sec_attrib, 1, 255, IPC_SEM_NAME);
	if (Pg_shared_hnd == NULL)
	{
		size = GetLastError();
		return(-1);
	}
}

/* This function gets called by every backend at startup time. Its
** main duty is to put the address of the shared memory table pointed
** to by Pg_shared_ptr. There's no need to get the IPC_SEM_NAME semaphore
** because this function is called before we start manipulating the
** shared memory table.
*/
void
_nt_attach()
{
	HANDLE hnd;

	/* Get a handle to the shared memory table */
	hnd = OpenFileMapping(FILE_MAP_ALL_ACCESS,
		FALSE, IPC_NAME);

	/* Map the ipc shared memory table into the address space
	** of this process at an address returned by MapViewOfFile
	*/
	Pg_shared_ptr = (struct Pg_shared *) MapViewOfFile(hnd,
		FILE_MAP_ALL_ACCESS, 0, 0, 0);

	if (Pg_shared_ptr == NULL)
	{
		hnd = GetLastError();
		return(-1);
	}
}

_get_ipc_sem()
{
	WaitForSingleObject(Pg_shared_hnd, 5000);
}

_rel_ipc_sem()
{
	ReleaseSemaphore(Pg_shared_hnd, 1, NULL);
}

pg_dlerror(void)
{
	int x = 0;
	x = x / x;
}

pg_dlclose(void *handle)
{
	FreeLibrary(handle);
}

void *
pg_dlopen(char *filename)
{
	HINSTANCE hinstlib;

	hinstlib = LoadLibrary(filename);
	return (hinstlib);
}

void *
pg_dlsym(void *handle, char *funcname)
{
	void *proc;

	proc = GetProcAddress(handle, funcname);
	return (proc);
}

void
ftruncate(int fd, int offset)
{
	HANDLE hnd;

	_lseek(fd, offset, SEEK_SET);
	hnd = _get_osfhandle(fd);
	SetEndOfFile(hnd);
}

/* The rest are just stubs that are intended to serve as place holders
** in case we want to set breakpoints to see what's happening when
** these routines are called. They'll eventually have to be filled
** in but they're not necessary to get Postgres95 going.
*/
setuid(int i)
{
	int x = 1;
	x = x / x;
}

setsid()
{
	int x = 1;
	x = x / x;
}

vfork(void)
{
	int x = 0;
	x = x / x;
}

ttyname(int y)
{
	int x = 0;
	x = x / x;
}

step(char *string, char *expbuf)
{
	int x = 0;
	x = x / x;
}

siglongjmp(int env, int value)
{
	int x = 0;
	x = x / x;
}

pause(void)
{
	int x = 0;
	x = x / x;
}

kill(int process, int signal)
{
	int x = 1;
	x = x / x;
}

getuid(void)
{
	int x = 1;
	x = x / x;
}

geteuid( void )
{
	int x = 1;
	x = x / x;
}

int
fsync(int filedes)
{
}

fork(void)
{
	int x = 0;
	x = x / x;
}

char *
compile(char *instring,char *expbuf,char *endbuf,int eof)
{
	int x = 0;
	x = x / x;
}

beginRecipe(char *s)
{
	int x = 0;
	x = x / x;
}
