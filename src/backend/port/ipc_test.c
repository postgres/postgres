/*-------------------------------------------------------------------------
 *
 * ipc_test.c
 *	   Simplistic testbed for shared memory and semaphore code.
 *
 * This file allows for quick "smoke testing" of a PG semaphore or shared
 * memory implementation, with less overhead than compiling up a whole
 * installation.  To use:
 *	1. Run configure, then edit src/include/pg_config.h to select the
 *	   USE_xxx_SEMAPHORES and USE_xxx_SHARED_MEMORY settings you want.
 *	   Also, adjust the pg_sema.c and pg_shmem.c symlinks in
 *	   src/backend/port/ if needed.
 *	2. In src/backend/port/, do "gmake ipc_test".
 *	3. Run ipc_test and see if it works.
 *	4. If it seems to work, try building the whole system and running
 *	   the parallel regression tests for a more complete test.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/ipc_test.c,v 1.23 2008/01/01 19:45:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/pg_sema.h"
#include "storage/pg_shmem.h"


/********* stuff needed to satisfy references in shmem/sema code *********/


volatile bool InterruptPending = false;
volatile bool QueryCancelPending = false;
volatile bool ProcDiePending = false;
volatile bool ImmediateInterruptOK = false;
volatile uint32 InterruptHoldoffCount = 0;
volatile uint32 CritSectionCount = 0;

bool		IsUnderPostmaster = false;
bool		assert_enabled = true;

int			MaxBackends = 32;
int			NBuffers = 64;

char	   *DataDir = ".";


#define MAX_ON_EXITS 20

static struct ONEXIT
{
	void		(*function) (int code, Datum arg);
	Datum		arg;
}	on_proc_exit_list[MAX_ON_EXITS], on_shmem_exit_list[MAX_ON_EXITS];

static int	on_proc_exit_index,
			on_shmem_exit_index;

void
proc_exit(int code)
{
	shmem_exit(code);
	while (--on_proc_exit_index >= 0)
		(*on_proc_exit_list[on_proc_exit_index].function) (code,
								  on_proc_exit_list[on_proc_exit_index].arg);
	exit(code);
}

void
shmem_exit(int code)
{
	while (--on_shmem_exit_index >= 0)
		(*on_shmem_exit_list[on_shmem_exit_index].function) (code,
								on_shmem_exit_list[on_shmem_exit_index].arg);
	on_shmem_exit_index = 0;
}

void
			on_shmem_exit(void (*function) (int code, Datum arg), Datum arg)
{
	if (on_shmem_exit_index >= MAX_ON_EXITS)
		elog(FATAL, "out of on_shmem_exit slots");

	on_shmem_exit_list[on_shmem_exit_index].function = function;
	on_shmem_exit_list[on_shmem_exit_index].arg = arg;

	++on_shmem_exit_index;
}

void
on_exit_reset(void)
{
	on_shmem_exit_index = 0;
	on_proc_exit_index = 0;
}

void
RecordSharedMemoryInLockFile(unsigned long id1, unsigned long id2)
{
}

void
ProcessInterrupts(void)
{
}

int
ExceptionalCondition(char *conditionName,
					 char *errorType,
					 char *fileName,
					 int lineNumber)
{
	fprintf(stderr, "TRAP: %s(\"%s\", File: \"%s\", Line: %d)\n",
			errorType, conditionName,
			fileName, lineNumber);
	abort();
	return 0;
}


int
errcode_for_file_access(void)
{
	return 0;
}

bool
errstart(int elevel, const char *filename, int lineno,
		 const char *funcname)
{
	return (elevel >= ERROR);
}

void
errfinish(int dummy,...)
{
	proc_exit(1);
}

void
elog_start(const char *filename, int lineno, const char *funcname)
{
}

void
elog_finish(int elevel, const char *fmt,...)
{
	fprintf(stderr, "ERROR: %s\n", fmt);
	proc_exit(1);
}

int
errcode(int sqlerrcode)
{
	return 0;					/* return value does not matter */
}

int
errmsg(const char *fmt,...)
{
	fprintf(stderr, "ERROR: %s\n", fmt);
	return 0;					/* return value does not matter */
}

int
errmsg_internal(const char *fmt,...)
{
	fprintf(stderr, "ERROR: %s\n", fmt);
	return 0;					/* return value does not matter */
}

int
errdetail(const char *fmt,...)
{
	fprintf(stderr, "DETAIL: %s\n", fmt);
	return 0;					/* return value does not matter */
}

int
errhint(const char *fmt,...)
{
	fprintf(stderr, "HINT: %s\n", fmt);
	return 0;					/* return value does not matter */
}


/********* here's the actual test *********/


typedef struct MyStorage
{
	PGShmemHeader header;
	int			flag;
	PGSemaphoreData sem;
}	MyStorage;


int
main(int argc, char **argv)
{
	MyStorage  *storage;
	int			cpid;

	printf("Creating shared memory ... ");
	fflush(stdout);

	storage = (MyStorage *) PGSharedMemoryCreate(8192, false, 5433);

	storage->flag = 1234;

	printf("OK\n");

	printf("Creating semaphores ... ");
	fflush(stdout);

	PGReserveSemaphores(2, 5433);

	PGSemaphoreCreate(&storage->sem);

	printf("OK\n");

	/* sema initial value is 1, so lock should work */

	printf("Testing Lock ... ");
	fflush(stdout);

	PGSemaphoreLock(&storage->sem, false);

	printf("OK\n");

	/* now sema value is 0, so trylock should fail */

	printf("Testing TryLock ... ");
	fflush(stdout);

	if (PGSemaphoreTryLock(&storage->sem))
		printf("unexpected result!\n");
	else
		printf("OK\n");

	/* unlocking twice and then locking twice should work... */

	printf("Testing Multiple Lock ... ");
	fflush(stdout);

	PGSemaphoreUnlock(&storage->sem);
	PGSemaphoreUnlock(&storage->sem);

	PGSemaphoreLock(&storage->sem, false);
	PGSemaphoreLock(&storage->sem, false);

	printf("OK\n");

	/* check Reset too */

	printf("Testing Reset ... ");
	fflush(stdout);

	PGSemaphoreUnlock(&storage->sem);

	PGSemaphoreReset(&storage->sem);

	if (PGSemaphoreTryLock(&storage->sem))
		printf("unexpected result!\n");
	else
		printf("OK\n");

	/* Fork a child process and see if it can communicate */

	printf("Forking child process ... ");
	fflush(stdout);

	cpid = fork();
	if (cpid == 0)
	{
		/* In child */
		on_exit_reset();
		sleep(3);
		storage->flag++;
		PGSemaphoreUnlock(&storage->sem);
		proc_exit(0);
	}
	if (cpid < 0)
	{
		/* Fork failed */
		printf("failed: %s\n", strerror(errno));
		proc_exit(1);
	}

	printf("forked child pid %d OK\n", cpid);

	if (storage->flag != 1234)
		printf("Wrong value found in shared memory!\n");

	printf("Waiting for child (should wait 3 sec here) ... ");
	fflush(stdout);

	PGSemaphoreLock(&storage->sem, false);

	printf("OK\n");

	if (storage->flag != 1235)
		printf("Wrong value found in shared memory!\n");

	/* Test shutdown */

	printf("Running shmem_exit processing ... ");
	fflush(stdout);

	shmem_exit(0);

	printf("OK\n");

	printf("Tests complete.\n");

	proc_exit(0);

	return 0;					/* not reached */
}
