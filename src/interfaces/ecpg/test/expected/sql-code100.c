/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "code100.pgc"

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

#line 1 "code100.pgc"

#include <stdio.h>


#line 1 "regression.h"






#line 4 "code100.pgc"



int main(int argc, char **argv)
{  /* exec sql begin declare section */
    
   
#line 9 "code100.pgc"
 int  index    ;
/* exec sql end declare section */
#line 10 "code100.pgc"



   ECPGdebug(1,stderr);
   
   { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 15 "code100.pgc"

   if (sqlca.sqlcode) printf("%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create  table test ( \"index\" numeric ( 3 )   primary key   , \"payload\" int4    not null )    ", ECPGt_EOIT, ECPGt_EORT);}
#line 20 "code100.pgc"

   if (sqlca.sqlcode) printf("%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);
   { ECPGtrans(__LINE__, NULL, "commit work");}
#line 22 "code100.pgc"

   if (sqlca.sqlcode) printf("%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);
   
   for (index=0;index<10;++index)
   {  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test ( payload  , index  ) values ( 0 ,  $1  ) ", 
	ECPGt_int,&(index),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);}
#line 28 "code100.pgc"

      if (sqlca.sqlcode) printf("%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);
   }
   { ECPGtrans(__LINE__, NULL, "commit work");}
#line 31 "code100.pgc"

   if (sqlca.sqlcode) printf("%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);
   
   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update test set payload  = payload + 1  where index = - 1 ", ECPGt_EOIT, ECPGt_EORT);}
#line 35 "code100.pgc"

   if (sqlca.sqlcode!=100) printf("%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);
   
   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "delete from test  where index = - 1 ", ECPGt_EOIT, ECPGt_EORT);}
#line 38 "code100.pgc"

   if (sqlca.sqlcode!=100) printf("%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test ( select  *  from test where index = - 1   ) ", ECPGt_EOIT, ECPGt_EORT);}
#line 41 "code100.pgc"

   if (sqlca.sqlcode!=100) printf("%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table test ", ECPGt_EOIT, ECPGt_EORT);}
#line 44 "code100.pgc"

   if (sqlca.sqlcode) printf("%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);
   { ECPGtrans(__LINE__, NULL, "commit work");}
#line 46 "code100.pgc"

   if (sqlca.sqlcode) printf("%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);
   
   { ECPGdisconnect(__LINE__, "CURRENT");}
#line 49 "code100.pgc"

   if (sqlca.sqlcode) printf("%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);
   return 0;
}
