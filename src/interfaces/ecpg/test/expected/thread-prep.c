/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "prep.pgc"
#include <stdlib.h>
#include "ecpg_config.h"

#ifndef ENABLE_THREAD_SAFETY
int
main(void)
{
	printf("No threading enabled.\n");
	return 0;
}
#else
#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif
#include <stdio.h>

#define THREADS		16
#define REPEATS		50


#line 1 "sqlca.h"
#ifndef POSTGRES_SQLCA_H
#define POSTGRES_SQLCA_H

#ifndef PGDLLIMPORT
#if  defined(WIN32) || defined(__CYGWIN__)
#define PGDLLIMPORT __declspec (dllimport)
#else
#define PGDLLIMPORT
#endif   /* __CYGWIN__ */
#endif   /* PGDLLIMPORT */

#define SQLERRMC_LEN	150

#ifdef __cplusplus
extern		"C"
{
#endif

struct sqlca_t
{
	char		sqlcaid[8];
	long		sqlabc;
	long		sqlcode;
	struct
	{
		int			sqlerrml;
		char		sqlerrmc[SQLERRMC_LEN];
	}			sqlerrm;
	char		sqlerrp[8];
	long		sqlerrd[6];
	/* Element 0: empty						*/
	/* 1: OID of processed tuple if applicable			*/
	/* 2: number of rows processed				*/
	/* after an INSERT, UPDATE or				*/
	/* DELETE statement					*/
	/* 3: empty						*/
	/* 4: empty						*/
	/* 5: empty						*/
	char		sqlwarn[8];
	/* Element 0: set to 'W' if at least one other is 'W'	*/
	/* 1: if 'W' at least one character string		*/
	/* value was truncated when it was			*/
	/* stored into a host variable.				*/

	/*
	 * 2: if 'W' a (hopefully) non-fatal notice occurred
	 */	/* 3: empty */
	/* 4: empty						*/
	/* 5: empty						*/
	/* 6: empty						*/
	/* 7: empty						*/

	char		sqlstate[5];
};

struct sqlca_t *ECPGget_sqlca(void);

#ifndef POSTGRES_ECPG_INTERNAL
#define sqlca (*ECPGget_sqlca())
#endif

#ifdef __cplusplus
}
#endif

#endif

#line 24 "prep.pgc"


#line 1 "regression.h"






#line 25 "prep.pgc"


/* exec sql whenever sqlerror  sqlprint ; */
#line 27 "prep.pgc"

/* exec sql whenever not found  sqlprint ; */
#line 28 "prep.pgc"


#ifdef WIN32
static unsigned __stdcall fn(void* arg)
#else
static void* fn(void* arg)
#endif
{
	int i;

	/* exec sql begin declare section */
	  
	 
	   
	
#line 39 "prep.pgc"
 int value ;
 
#line 40 "prep.pgc"
 char name [ 100 ] ;
 
#line 41 "prep.pgc"
 char query [ 256 ] = "INSERT INTO T VALUES ( ? )" ;
/* exec sql end declare section */
#line 42 "prep.pgc"


	value = (long)arg;
	sprintf(name, "Connection: %d", value);

	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , name, 0); 
#line 47 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 47 "prep.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 48 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 48 "prep.pgc"

	for (i = 1; i <= REPEATS; ++i)
	{
		{ ECPGprepare(__LINE__, NULL, 0, "i", query);
#line 51 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 51 "prep.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "i", 
	ECPGt_int,&(value),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 52 "prep.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) sqlprint();
#line 52 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 52 "prep.pgc"

	}
	{ ECPGdeallocate(__LINE__, 0, NULL, "i");
#line 54 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 54 "prep.pgc"

	{ ECPGdisconnect(__LINE__, name);
#line 55 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 55 "prep.pgc"


	return 0;
}

int main ()
{
	int i;
#ifdef WIN32
	HANDLE threads[THREADS];
#else
	pthread_t threads[THREADS];
#endif

	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); 
#line 69 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 69 "prep.pgc"

	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 70 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 70 "prep.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table if exists T", ECPGt_EOIT, ECPGt_EORT);
#line 71 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 71 "prep.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table T ( i int )", ECPGt_EOIT, ECPGt_EORT);
#line 72 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 72 "prep.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 73 "prep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 73 "prep.pgc"


#ifdef WIN32
	for (i = 0; i < THREADS; ++i)
	{
		unsigned id;
		threads[i] = (HANDLE)_beginthreadex(NULL, 0, fn, (void*)i, 0, &id);
	}

	WaitForMultipleObjects(THREADS, threads, TRUE, INFINITE);
	for (i = 0; i < THREADS; ++i)
		CloseHandle(threads[i]);
#else
	for (i = 0; i < THREADS; ++i)
		pthread_create(&threads[i], NULL, fn, (void *) (long) i);
	for (i = 0; i < THREADS; ++i)
		pthread_join(threads[i], NULL);
#endif

	return 0;
}
#endif

