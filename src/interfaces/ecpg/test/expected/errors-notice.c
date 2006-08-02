/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "test_notice.pgc"

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

#line 1 "test_notice.pgc"


#line 1 "./../regression.h"






#line 2 "test_notice.pgc"


#include <stdio.h>

static void printwarning(void)
{
   if (sqlca.sqlwarn[0]) printf("sqlca.sqlwarn: %c",sqlca.sqlwarn[0]);
   else return;

   if (sqlca.sqlwarn[1]) putchar('1');
   if (sqlca.sqlwarn[2]) putchar('2');

   putchar('\n');
}

int main(int argc, char **argv)
{
   /* exec sql begin declare section */
      
   
#line 20 "test_notice.pgc"
 int  payload    ;
/* exec sql end declare section */
#line 21 "test_notice.pgc"


   /* actually this will print 'sql error' if a warning occurs */
   /* exec sql whenever sql_warning  do printwarning (  ) ; */
#line 24 "test_notice.pgc"


   ECPGdebug(1, stderr);

   { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); 
#line 28 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 28 "test_notice.pgc"

   if (sqlca.sqlcode) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGdo(__LINE__, 0, 1, NULL, "create  table test ( \"index\" numeric ( 3 )   primary key  , \"payload\" int4   not null )    ", ECPGt_EOIT, ECPGt_EORT);
#line 33 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 33 "test_notice.pgc"

   if (sqlca.sqlcode) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGtrans(__LINE__, NULL, "commit");
#line 36 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 36 "test_notice.pgc"

   if (sqlca.sqlcode) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);


   /* double BEGIN */
   { ECPGtrans(__LINE__, NULL, "begin transaction ");
#line 41 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 41 "test_notice.pgc"

   if (sqlca.sqlcode) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   /* BEGIN with already open transaction */
   { ECPGtrans(__LINE__, NULL, "begin transaction ");
#line 45 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 45 "test_notice.pgc"

   if (sqlca.sqlcode!=ECPG_WARNING_IN_TRANSACTION) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   /* double COMMIT */
   { ECPGtrans(__LINE__, NULL, "commit");
#line 49 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 49 "test_notice.pgc"

   if (sqlca.sqlcode) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   /* COMMIT without open transaction */
   { ECPGtrans(__LINE__, NULL, "commit");
#line 53 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 53 "test_notice.pgc"

   if (sqlca.sqlcode!=ECPG_WARNING_NO_TRANSACTION) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   /* ROLLBACK without open transaction */
   { ECPGtrans(__LINE__, NULL, "rollback");
#line 57 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 57 "test_notice.pgc"

   if (sqlca.sqlcode!=ECPG_WARNING_NO_TRANSACTION) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   sqlca.sqlcode=0;
   /* declare x  cursor  for select  *  from test    */
#line 61 "test_notice.pgc"

   if (sqlca.sqlcode) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGdo(__LINE__, 0, 1, NULL, "declare x  cursor  for select  *  from test   ", ECPGt_EOIT, ECPGt_EORT);
#line 64 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 64 "test_notice.pgc"

   if (sqlca.sqlcode) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGdo(__LINE__, 0, 1, NULL, "declare x  cursor  for select  *  from test   ", ECPGt_EOIT, ECPGt_EORT);
#line 67 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 67 "test_notice.pgc"

   if (sqlca.sqlcode!=ECPG_WARNING_PORTAL_EXISTS) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGdo(__LINE__, 0, 1, NULL, "close x", ECPGt_EOIT, ECPGt_EORT);
#line 70 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 70 "test_notice.pgc"

   if (sqlca.sqlcode) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGtrans(__LINE__, NULL, "rollback");
#line 73 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 73 "test_notice.pgc"


   { ECPGdo(__LINE__, 0, 1, NULL, "close x", ECPGt_EOIT, ECPGt_EORT);
#line 75 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 75 "test_notice.pgc"

   if (sqlca.sqlcode!=ECPG_WARNING_UNKNOWN_PORTAL) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGtrans(__LINE__, NULL, "rollback");
#line 78 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 78 "test_notice.pgc"


   { ECPGdo(__LINE__, 0, 1, NULL, "update test set nonexistent  = 2  ", ECPGt_EOIT, ECPGt_EORT);
#line 80 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 80 "test_notice.pgc"

   if (sqlca.sqlcode!=ECPG_PGSQL) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGdo(__LINE__, 0, 1, NULL, "select  payload  from test where index = 1  ", ECPGt_EOIT, 
	ECPGt_int,&(payload),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 83 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 83 "test_notice.pgc"

   if (sqlca.sqlcode!=ECPG_WARNING_QUERY_IGNORED) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGtrans(__LINE__, NULL, "rollback");
#line 86 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 86 "test_notice.pgc"

   if (sqlca.sqlcode) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   /* this will raise a warning */
   { ECPGdo(__LINE__, 0, 1, NULL, "drop table test ", ECPGt_EOIT, ECPGt_EORT);
#line 90 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 90 "test_notice.pgc"

   if (sqlca.sqlcode) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);
   { ECPGtrans(__LINE__, NULL, "commit");
#line 92 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 92 "test_notice.pgc"

   if (sqlca.sqlcode) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   { ECPGdisconnect(__LINE__, "CURRENT");
#line 95 "test_notice.pgc"

if (sqlca.sqlwarn[0] == 'W') printwarning (  );}
#line 95 "test_notice.pgc"

   if (sqlca.sqlcode) printf("%d %ld:%s\n",__LINE__,sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);

   return 0;
}
