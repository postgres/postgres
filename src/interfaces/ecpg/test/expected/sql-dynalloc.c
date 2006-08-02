/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "dynalloc.pgc"
#include <stdio.h>

#line 1 "./../../include/sqlca.h"
#ifndef POSTGRES_SQLCA_H
#define POSTGRES_SQLCA_H

#ifndef DLLIMPORT
#if  defined(WIN32) || defined(__CYGWIN__)
#define DLLIMPORT __declspec (dllimport)
#else
#define DLLIMPORT
#endif   /* __CYGWIN__ */
#endif   /* DLLIMPORT */

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

#line 2 "dynalloc.pgc"

#include <stdlib.h>

#line 1 "./../regression.h"






#line 4 "dynalloc.pgc"


int main(void)
{
   /* exec sql begin declare section */
    
    
   
#line 9 "dynalloc.pgc"
 char ** cpp   = 0 ;
 
#line 10 "dynalloc.pgc"
 int * ipointer   = 0 ;
/* exec sql end declare section */
#line 11 "dynalloc.pgc"

   int i;

   ECPGdebug(1, stderr);

   /* exec sql whenever sqlerror  do sqlprint (  ) ; */
#line 16 "dynalloc.pgc"

   { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); 
#line 17 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 17 "dynalloc.pgc"


   ECPGallocate_desc(__LINE__, "mydesc");
#line 19 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );
#line 19 "dynalloc.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, "select  tablename  from pg_tables   ", ECPGt_EOIT, 
	ECPGt_descriptor, "mydesc", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 20 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 20 "dynalloc.pgc"

   { ECPGget_desc(__LINE__, "mydesc", 1,ECPGd_indicator,
	ECPGt_int,&(ipointer),(long)1,(long)0,sizeof(int), ECPGd_data,
	ECPGt_char,&(cpp),(long)0,(long)0,(1)*sizeof(char), ECPGd_EODT);

#line 21 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 21 "dynalloc.pgc"


   printf("Result ");
   for (i=0;i<sqlca.sqlerrd[2];++i)
   {
      if (ipointer[i]) printf("NULL, ");
      else printf("'%s', ",cpp[i]); 
   }
   ECPGfree_auto_mem();
   printf("\n");

   ECPGdeallocate_desc(__LINE__, "mydesc");
#line 32 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );
#line 32 "dynalloc.pgc"

   { ECPGdisconnect(__LINE__, "CURRENT");
#line 33 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 33 "dynalloc.pgc"

   return 0;
}
