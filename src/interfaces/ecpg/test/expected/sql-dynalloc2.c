/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "dynalloc2.pgc"
#include <stdio.h>

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

#line 2 "dynalloc2.pgc"

#include <stdlib.h>

#line 1 "regression.h"






#line 4 "dynalloc2.pgc"


int main(void)
{
   /* exec sql begin declare section */
    
    
    
    
    
   
#line 9 "dynalloc2.pgc"
 int * ip1 = 0 ;
 
#line 10 "dynalloc2.pgc"
 char ** cp2 = 0 ;
 
#line 11 "dynalloc2.pgc"
 int * ipointer1 = 0 ;
 
#line 12 "dynalloc2.pgc"
 int * ipointer2 = 0 ;
 
#line 13 "dynalloc2.pgc"
 int colnum ;
/* exec sql end declare section */
#line 14 "dynalloc2.pgc"

   int i;

   ECPGdebug(1, stderr);

   /* exec sql whenever sqlerror  do sqlprint ( ) ; */
#line 19 "dynalloc2.pgc"

   { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); 
#line 20 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 20 "dynalloc2.pgc"


   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set datestyle to postgres", ECPGt_EOIT, ECPGt_EORT);
#line 22 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 22 "dynalloc2.pgc"


   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table test ( a int , b text )", ECPGt_EOIT, ECPGt_EORT);
#line 24 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 24 "dynalloc2.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( 1 , 'one' )", ECPGt_EOIT, ECPGt_EORT);
#line 25 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 25 "dynalloc2.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( 2 , 'two' )", ECPGt_EOIT, ECPGt_EORT);
#line 26 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 26 "dynalloc2.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( null , 'three' )", ECPGt_EOIT, ECPGt_EORT);
#line 27 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 27 "dynalloc2.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( 4 , 'four' )", ECPGt_EOIT, ECPGt_EORT);
#line 28 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 28 "dynalloc2.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( 5 , null )", ECPGt_EOIT, ECPGt_EORT);
#line 29 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 29 "dynalloc2.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( null , null )", ECPGt_EOIT, ECPGt_EORT);
#line 30 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 30 "dynalloc2.pgc"


   ECPGallocate_desc(__LINE__, "mydesc");
#line 32 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );
#line 32 "dynalloc2.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select * from test", ECPGt_EOIT, 
	ECPGt_descriptor, "mydesc", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 33 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 33 "dynalloc2.pgc"

   { ECPGget_desc_header(__LINE__, "mydesc", &(colnum));

#line 34 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 34 "dynalloc2.pgc"

   { ECPGget_desc(__LINE__, "mydesc", 1,ECPGd_indicator,
	ECPGt_int,&(ipointer1),(long)1,(long)0,sizeof(int), ECPGd_data,
	ECPGt_int,&(ip1),(long)1,(long)0,sizeof(int), ECPGd_EODT);

#line 35 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 35 "dynalloc2.pgc"

   { ECPGget_desc(__LINE__, "mydesc", 2,ECPGd_indicator,
	ECPGt_int,&(ipointer2),(long)1,(long)0,sizeof(int), ECPGd_data,
	ECPGt_char,&(cp2),(long)0,(long)0,(1)*sizeof(char), ECPGd_EODT);

#line 36 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 36 "dynalloc2.pgc"


   printf("Result (%d columns):\n", colnum);
   for (i=0;i < sqlca.sqlerrd[2];++i)
   {
      if (ipointer1[i]) printf("NULL, ");
      else printf("%d, ",ip1[i]);

      if (ipointer2[i]) printf("NULL, ");
      else printf("'%s', ",cp2[i]);
      printf("\n");
   }
   ECPGfree_auto_mem();
   printf("\n");

   ECPGdeallocate_desc(__LINE__, "mydesc");
#line 51 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );
#line 51 "dynalloc2.pgc"

   { ECPGtrans(__LINE__, NULL, "rollback");
#line 52 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 52 "dynalloc2.pgc"

   { ECPGdisconnect(__LINE__, "CURRENT");
#line 53 "dynalloc2.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 53 "dynalloc2.pgc"

   return 0;
}
