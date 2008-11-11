/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "indicators.pgc"
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

#line 3 "indicators.pgc"


#line 1 "regression.h"






#line 4 "indicators.pgc"



int main(int argc, char **argv)
{
	/* exec sql begin declare section */
		   
		   
	
#line 10 "indicators.pgc"
 int  intvar   = 5 ;
 
#line 11 "indicators.pgc"
 int  nullind   = - 1 ;
/* exec sql end declare section */
#line 12 "indicators.pgc"


	ECPGdebug(1,stderr);

	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 16 "indicators.pgc"

	{ ECPGsetcommit(__LINE__, "off", NULL);}
#line 17 "indicators.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create  table indicator_test ( \"id\" int   primary key   , \"str\" text    not null , val int   null )    ", ECPGt_EOIT, ECPGt_EORT);}
#line 22 "indicators.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit work");}
#line 23 "indicators.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into indicator_test ( id  , str  , val  ) values ( 1 , 'Hello' , 0 ) ", ECPGt_EOIT, ECPGt_EORT);}
#line 25 "indicators.pgc"


	/* use indicator in insert */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into indicator_test ( id  , str  , val  ) values ( 2 , 'Hi there' ,  $1  ) ", 
	ECPGt_int,&(intvar),(long)1,(long)1,sizeof(int), 
	ECPGt_int,&(nullind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);}
#line 28 "indicators.pgc"

	nullind = 0;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into indicator_test ( id  , str  , val  ) values ( 3 , 'Good evening' ,  $1  ) ", 
	ECPGt_int,&(intvar),(long)1,(long)1,sizeof(int), 
	ECPGt_int,&(nullind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);}
#line 30 "indicators.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit work");}
#line 31 "indicators.pgc"


	/* use indicators to get information about selects */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  val  from indicator_test where id = 1  ", ECPGt_EOIT, 
	ECPGt_int,&(intvar),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 34 "indicators.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  val  from indicator_test where id = 2  ", ECPGt_EOIT, 
	ECPGt_int,&(intvar),(long)1,(long)1,sizeof(int), 
	ECPGt_int,&(nullind),(long)1,(long)1,sizeof(int), ECPGt_EORT);}
#line 35 "indicators.pgc"

	printf("intvar: %d, nullind: %d\n", intvar, nullind);
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  val  from indicator_test where id = 3  ", ECPGt_EOIT, 
	ECPGt_int,&(intvar),(long)1,(long)1,sizeof(int), 
	ECPGt_int,&(nullind),(long)1,(long)1,sizeof(int), ECPGt_EORT);}
#line 37 "indicators.pgc"

	printf("intvar: %d, nullind: %d\n", intvar, nullind);

	/* use indicators for update */
	intvar = 5; nullind = -1;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update indicator_test set val  =  $1   where id = 1 ", 
	ECPGt_int,&(intvar),(long)1,(long)1,sizeof(int), 
	ECPGt_int,&(nullind),(long)1,(long)1,sizeof(int), ECPGt_EOIT, ECPGt_EORT);}
#line 42 "indicators.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  val  from indicator_test where id = 1  ", ECPGt_EOIT, 
	ECPGt_int,&(intvar),(long)1,(long)1,sizeof(int), 
	ECPGt_int,&(nullind),(long)1,(long)1,sizeof(int), ECPGt_EORT);}
#line 43 "indicators.pgc"

	printf("intvar: %d, nullind: %d\n", intvar, nullind);

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table indicator_test ", ECPGt_EOIT, ECPGt_EORT);}
#line 46 "indicators.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit work");}
#line 47 "indicators.pgc"


	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 49 "indicators.pgc"

	return 0;
}
