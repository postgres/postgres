/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "thread.pgc"
/*
 *	Thread test program
 *	by Philip Yarra & Lee Kindness.
 */
#include <stdlib.h>
#ifndef ENABLE_THREAD_SAFETY
int
main(void)
{
	printf("Success.\n");
	return 0;
}
#else
#include <pthread.h>

#undef DEBUG



#line 1 "regression.h"






#line 19 "thread.pgc"


void *test_thread(void *arg);

int nthreads   = 10;
int iterations = 20;

int main(int argc, char *argv[])
{
  pthread_t *threads;
  int n;
  /* exec sql begin declare section */
   
  
#line 31 "thread.pgc"
 int  l_rows    ;
/* exec sql end declare section */
#line 32 "thread.pgc"



 /* Switch off debug output for regression tests. The threads get executed in
  * more or less random order */
  ECPGdebug(0, stderr);


  /* setup test_thread table */
  { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); }
#line 41 "thread.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "drop table test_thread ", ECPGt_EOIT, ECPGt_EORT);}
#line 42 "thread.pgc"
 /* DROP might fail */
  { ECPGtrans(__LINE__, NULL, "commit");}
#line 43 "thread.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "create  table test_thread ( tstamp timestamp    not null default cast( timeofday () as timestamp   ) , thread TEXT   not null , iteration integer   not null , primary key( thread , iteration )   )    ", ECPGt_EOIT, ECPGt_EORT);}
#line 48 "thread.pgc"

  { ECPGtrans(__LINE__, NULL, "commit");}
#line 49 "thread.pgc"

  { ECPGdisconnect(__LINE__, "CURRENT");}
#line 50 "thread.pgc"


  /* create, and start, threads */
  threads = calloc(nthreads, sizeof(pthread_t));
  if( threads == NULL )
    {
      fprintf(stderr, "Cannot alloc memory\n");
      return( 1 );
    }
  for( n = 0; n < nthreads; n++ )
    {
      pthread_create(&threads[n], NULL, test_thread, (void *) (n + 1));
    }

  /* wait for thread completion */
  for( n = 0; n < nthreads; n++ )
    {
      pthread_join(threads[n], NULL);
    }
  free(threads);

  /* and check results */
  { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); }
#line 72 "thread.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "select  count (*)  from test_thread   ", ECPGt_EOIT, 
	ECPGt_int,&(l_rows),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 73 "thread.pgc"

  { ECPGtrans(__LINE__, NULL, "commit");}
#line 74 "thread.pgc"

  { ECPGdisconnect(__LINE__, "CURRENT");}
#line 75 "thread.pgc"

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
    
   
  
#line 88 "thread.pgc"
 int  l_i    ;
 
#line 89 "thread.pgc"
 char  l_connection [ 128 ]    ;
/* exec sql end declare section */
#line 90 "thread.pgc"


  /* build up connection name, and connect to database */
  snprintf(l_connection, sizeof(l_connection), "thread_%03ld", threadnum);
  /* exec sql whenever sqlerror  sqlprint ; */
#line 94 "thread.pgc"

  { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , l_connection, 0); 
#line 95 "thread.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 95 "thread.pgc"

  if( sqlca.sqlcode != 0 )
    {
      printf("%s: ERROR: cannot connect to database!\n", l_connection);
      return( NULL );
    }
  { ECPGtrans(__LINE__, l_connection, "begin transaction ");
#line 101 "thread.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 101 "thread.pgc"


  /* insert into test_thread table */
  for( l_i = 1; l_i <= iterations; l_i++ )
    {
#ifdef DEBUG
      printf("%s: inserting %d\n", l_connection, l_i);
#endif
      { ECPGdo(__LINE__, 0, 1, l_connection, "insert into test_thread ( thread  , iteration  ) values (  ? ,  ? ) ", 
	ECPGt_char,(l_connection),(long)128,(long)1,(128)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(l_i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 109 "thread.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 109 "thread.pgc"

#ifdef DEBUG
      if( sqlca.sqlcode == 0 )
	printf("%s: insert done\n", l_connection);
      else
	printf("%s: ERROR: insert failed!\n", l_connection);
#endif
    }

  /* all done */
  { ECPGtrans(__LINE__, l_connection, "commit");
#line 119 "thread.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 119 "thread.pgc"

  { ECPGdisconnect(__LINE__, l_connection);
#line 120 "thread.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 120 "thread.pgc"

#ifdef DEBUG
  printf("%s: done!\n", l_connection);
#endif
  return( NULL );
}
#endif /* ENABLE_THREAD_SAFETY */
