/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* Needed for informix compatibility */
#include <ecpg_informix.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "test_informix2.pgc"
#include <stdio.h>
#include <stdlib.h>
#include "sqltypes.h"


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
	/* stored into a host variable.             */

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

#line 5 "test_informix2.pgc"


#line 1 "regression.h"






#line 6 "test_informix2.pgc"



/* Check SQLCODE, and produce a "standard error" if it's wrong! */
static void sql_check(char *fn, char *caller, int ignore)
{
  char errorstring[255];

  if (SQLCODE == ignore)
    return;
  else
  {
    if (SQLCODE != 0)
    {

      sprintf(errorstring, "**SQL error %ld doing '%s' in function '%s'. [%s]",
             SQLCODE, caller, fn, sqlca.sqlerrm.sqlerrmc);
      fprintf(stderr, "%s", errorstring);
      printf("%s\n", errorstring);

      /* attempt a ROLLBACK */
      { ECPGtrans(__LINE__, NULL, "rollback");}
#line 27 "test_informix2.pgc"


      if (SQLCODE == 0)
      {
        sprintf(errorstring, "Rollback successful.\n");
      } else {
        sprintf(errorstring, "Rollback failed with code %ld.\n", SQLCODE);
      }

      fprintf(stderr, "%s", errorstring);
      printf("%s\n", errorstring);

      exit(1);
    }
  }
}



int main(void)
{
	/* exec sql begin declare section */
		 
		 
		 
		 
		 
	
#line 49 "test_informix2.pgc"
 int c ;
 
#line 50 "test_informix2.pgc"
 timestamp d ;
 
#line 51 "test_informix2.pgc"
 timestamp e ;
 
#line 52 "test_informix2.pgc"
 timestamp maxd ;
 
#line 53 "test_informix2.pgc"
 char dbname [ 30 ] ;
/* exec sql end declare section */
#line 54 "test_informix2.pgc"


	interval *intvl;

	/* exec sql whenever sqlerror  sqlprint ; */
#line 58 "test_informix2.pgc"


	ECPGdebug(1, stderr);

	strcpy(dbname, "regress1");
	{ ECPGconnect(__LINE__, 1, dbname , NULL, NULL , NULL, 0); 
#line 63 "test_informix2.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 63 "test_informix2.pgc"

	sql_check("main", "connect", 0);

	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "set DateStyle to 'DMY'", ECPGt_EOIT, ECPGt_EORT);
#line 66 "test_informix2.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 66 "test_informix2.pgc"


	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "create table history ( customerid integer , timestamp timestamp without time zone , action_taken char ( 5 ) , narrative varchar ( 100 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 68 "test_informix2.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 68 "test_informix2.pgc"

	sql_check("main", "create", 0);

	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "insert into history ( customerid , timestamp , action_taken , narrative ) values ( 1 , '2003-05-07 13:28:34 CEST' , 'test' , 'test' )", ECPGt_EOIT, ECPGt_EORT);
#line 73 "test_informix2.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 73 "test_informix2.pgc"

	sql_check("main", "insert", 0);

	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "select max ( timestamp ) from history", ECPGt_EOIT, 
	ECPGt_timestamp,&(maxd),(long)1,(long)1,sizeof(timestamp), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 78 "test_informix2.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 78 "test_informix2.pgc"

	sql_check("main", "select max", 100);

	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "select customerid , timestamp from history where timestamp = $1  limit 1", 
	ECPGt_timestamp,&(maxd),(long)1,(long)1,sizeof(timestamp), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(c),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_timestamp,&(d),(long)1,(long)1,sizeof(timestamp), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 85 "test_informix2.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 85 "test_informix2.pgc"

	sql_check("main", "select", 0);

	printf("Read in customer %d\n", c);

	intvl = PGTYPESinterval_from_asc("1 day 2 hours 24 minutes 65 seconds", NULL);
	PGTYPEStimestamp_add_interval(&d, intvl, &e);
	free(intvl);
	c++;

	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "insert into history ( customerid , timestamp , action_taken , narrative ) values ( $1  , $2  , 'test' , 'test' )", 
	ECPGt_int,&(c),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_timestamp,&(e),(long)1,(long)1,sizeof(timestamp), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 97 "test_informix2.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 97 "test_informix2.pgc"

	sql_check("main", "update", 0);

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 100 "test_informix2.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 100 "test_informix2.pgc"


	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "drop table history", ECPGt_EOIT, ECPGt_EORT);
#line 102 "test_informix2.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 102 "test_informix2.pgc"

	sql_check("main", "drop", 0);

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 105 "test_informix2.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 105 "test_informix2.pgc"


	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 107 "test_informix2.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 107 "test_informix2.pgc"

	sql_check("main", "disconnect", 0);

	printf("All OK!\n");

	exit(0);

/*
                 Table "public.history"
    Column    |            Type             | Modifiers
--------------+-----------------------------+-----------
 customerid   | integer                     | not null
 timestamp    | timestamp without time zone | not null
 action_taken | character(5)                | not null
 narrative    | character varying(100)      |
*/

}
