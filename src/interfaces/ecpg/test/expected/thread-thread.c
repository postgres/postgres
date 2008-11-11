/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "thread.pgc"
/*
 *	Thread test program
 *	by Philip Yarra & Lee Kindness.
 */
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
#ifndef WIN32
#include <pthread.h>
#else
#include <windows.h>
#endif


#line 1 "regression.h"






#line 22 "thread.pgc"


void *test_thread(void *arg);

int nthreads   = 10;
int iterations = 20;

int main(int argc, char *argv[])
{
#ifndef WIN32
  pthread_t *threads;
#else
  HANDLE *threads;
#endif
  int n;
  /* exec sql begin declare section */
   
  
#line 38 "thread.pgc"
 int  l_rows    ;
/* exec sql end declare section */
#line 39 "thread.pgc"


 /* Do not switch on debug output for regression tests. The threads get executed in
  * more or less random order */
 /* ECPGdebug(1, stderr); */

  /* setup test_thread table */
  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 46 "thread.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table test_thread ", ECPGt_EOIT, ECPGt_EORT);}
#line 47 "thread.pgc"
 /* DROP might fail */
  { ECPGtrans(__LINE__, NULL, "commit ");}
#line 48 "thread.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create  table test_thread ( tstamp timestamp    not null default cast ( timeofday ( ) as timestamp   ) , thread text    not null , iteration integer   not null , primary key ( thread , iteration )   )    ", ECPGt_EOIT, ECPGt_EORT);}
#line 53 "thread.pgc"

  { ECPGtrans(__LINE__, NULL, "commit ");}
#line 54 "thread.pgc"

  { ECPGdisconnect(__LINE__, "CURRENT");}
#line 55 "thread.pgc"


  /* create, and start, threads */
  threads = calloc(nthreads, sizeof(threads[0]));
  if( threads == NULL )
    {
      fprintf(stderr, "Cannot alloc memory\n");
      return( 1 );
    }
  for( n = 0; n < nthreads; n++ )
    {
#ifndef WIN32
      pthread_create(&threads[n], NULL, test_thread, (void *) (long) (n + 1));
#else
      threads[n] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)test_thread, (void *) (n + 1), 0, NULL);
#endif
    }

  /* wait for thread completion */
#ifndef WIN32
  for( n = 0; n < nthreads; n++ )
    {
      pthread_join(threads[n], NULL);
    }
#else
  WaitForMultipleObjects(nthreads, threads, TRUE, INFINITE);
#endif
  free(threads);

  /* and check results */
  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 85 "thread.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  count ( * )  from test_thread   ", ECPGt_EOIT, 
	ECPGt_int,&(l_rows),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 86 "thread.pgc"

  { ECPGtrans(__LINE__, NULL, "commit ");}
#line 87 "thread.pgc"

  { ECPGdisconnect(__LINE__, "CURRENT");}
#line 88 "thread.pgc"

  if( l_rows == (nthreads * iterations) )
    printf("Success.\n");
  else
    printf("ERROR: Failure - expecting %d rows, got %d.\n", nthreads * iterations, l_rows);

  return( 0 );
}

void *test_thread(void *arg)
{
  long threadnum = (long)arg;
  /* exec sql begin declare section */
    
   
  
#line 101 "thread.pgc"
 int  l_i    ;
 
#line 102 "thread.pgc"
 char  l_connection  [ 128 ]   ;
/* exec sql end declare section */
#line 103 "thread.pgc"


  /* build up connection name, and connect to database */
#ifndef WIN32_ONLY_COMPILER
  snprintf(l_connection, sizeof(l_connection), "thread_%03ld", threadnum);
#else
  _snprintf(l_connection, sizeof(l_connection), "thread_%03ld", threadnum);
#endif
  /* exec sql whenever sqlerror  sqlprint ; */
#line 111 "thread.pgc"

  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , l_connection, 0); 
#line 112 "thread.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 112 "thread.pgc"

  if( sqlca.sqlcode != 0 )
    {
      printf("%s: ERROR: cannot connect to database!\n", l_connection);
      return( NULL );
    }
  { ECPGtrans(__LINE__, l_connection, "begin  ");
#line 118 "thread.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 118 "thread.pgc"


  /* insert into test_thread table */
  for( l_i = 1; l_i <= iterations; l_i++ )
    {
      { ECPGdo(__LINE__, 0, 1, l_connection, 0, ECPGst_normal, "insert into test_thread ( thread  , iteration  ) values (  $1  ,  $2  ) ", 
	ECPGt_char,(l_connection),(long)128,(long)1,(128)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(l_i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 123 "thread.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 123 "thread.pgc"

      if( sqlca.sqlcode != 0 )
	printf("%s: ERROR: insert failed!\n", l_connection);
    }

  /* all done */
  { ECPGtrans(__LINE__, l_connection, "commit ");
#line 129 "thread.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 129 "thread.pgc"

  { ECPGdisconnect(__LINE__, l_connection);
#line 130 "thread.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 130 "thread.pgc"

  return( NULL );
}
#endif /* ENABLE_THREAD_SAFETY */
