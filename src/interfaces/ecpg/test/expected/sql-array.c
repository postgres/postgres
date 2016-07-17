/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "array.pgc"
#include <locale.h>
#include <string.h>
#include <stdlib.h>

#include <pgtypes_date.h>
#include <pgtypes_interval.h>
#include <pgtypes_numeric.h>
#include <pgtypes_timestamp.h>

/* exec sql whenever sqlerror  sqlprint ; */
#line 10 "array.pgc"



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

#line 12 "array.pgc"


#line 1 "regression.h"






#line 13 "array.pgc"


int
main (void)
{
/* exec sql begin declare section */
	    
	   
	   
	 
	 
	 
	 
	   
	    
	 

#line 19 "array.pgc"
 int i = 1 , j ;
 
#line 20 "array.pgc"
 int * did = & i ;
 
#line 21 "array.pgc"
 short a [ 10 ] = { 9 , 8 , 7 , 6 , 5 , 4 , 3 , 2 , 1 , 0 } ;
 
#line 22 "array.pgc"
 timestamp ts [ 10 ] ;
 
#line 23 "array.pgc"
 date d [ 10 ] ;
 
#line 24 "array.pgc"
 interval in [ 10 ] ;
 
#line 25 "array.pgc"
 numeric n [ 10 ] ;
 
#line 26 "array.pgc"
 char text [ 25 ] = "klmnopqrst" ;
 
#line 27 "array.pgc"
 char * t = ( char * ) malloc ( 11 ) ;
 
#line 28 "array.pgc"
 double f ;
/* exec sql end declare section */
#line 29 "array.pgc"


	strcpy(t, "0123456789");
	setlocale(LC_ALL, "C");

	ECPGdebug(1, stderr);

	for (j = 0; j < 10; j++) {
		char str[20];
		numeric *value;
		interval *inter;

		sprintf(str, "2000-1-1 0%d:00:00", j);
		ts[j] = PGTYPEStimestamp_from_asc(str, NULL);
		sprintf(str, "2000-1-1%d\n", j);
		d[j] = PGTYPESdate_from_asc(str, NULL);
		sprintf(str, "%d hours", j+10);
		inter = PGTYPESinterval_from_asc(str, NULL);
		in[j] = *inter;
		value = PGTYPESnumeric_new();
		PGTYPESnumeric_from_int(j, value);
		n[j] = *value;
	}

        { ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 53 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 53 "array.pgc"


	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 55 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 55 "array.pgc"


	{ ECPGtrans(__LINE__, NULL, "begin work");
#line 57 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 57 "array.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table test ( f float , i int , a int [ 10 ] , text char ( 10 ) , ts timestamp [ 10 ] , n numeric [ 10 ] , d date [ 10 ] , inter interval [ 10 ] )", ECPGt_EOIT, ECPGt_EORT);
#line 59 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 59 "array.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test ( f , i , a , text , ts , n , d , inter ) values ( 404.90 , 3 , '{0,1,2,3,4,5,6,7,8,9}' , 'abcdefghij' , $1  , $2  , $3  , $4  )", 
	ECPGt_timestamp,&(ts),(long)1,(long)10,sizeof(timestamp), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_numeric,&(n),(long)1,(long)10,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_date,&(d),(long)1,(long)10,sizeof(date), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_interval,&(in),(long)1,(long)10,sizeof(interval), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 61 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 61 "array.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test ( f , i , a , text , ts , n , d , inter ) values ( 140787.0 , 2 , $1  , $2  , $3  , $4  , $5  , $6  )", 
	ECPGt_short,(a),(long)1,(long)10,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(text),(long)25,(long)1,(25)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_timestamp,&(ts),(long)1,(long)10,sizeof(timestamp), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_numeric,&(n),(long)1,(long)10,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_date,&(d),(long)1,(long)10,sizeof(date), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_interval,&(in),(long)1,(long)10,sizeof(interval), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 63 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 63 "array.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test ( f , i , a , text , ts , n , d , inter ) values ( 14.07 , $1  , $2  , $3  , $4  , $5  , $6  , $7  )", 
	ECPGt_int,&(did),(long)1,(long)0,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,(a),(long)1,(long)10,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(t),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_timestamp,&(ts),(long)1,(long)10,sizeof(timestamp), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_numeric,&(n),(long)1,(long)10,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_date,&(d),(long)1,(long)10,sizeof(date), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_interval,&(in),(long)1,(long)10,sizeof(interval), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 65 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 65 "array.pgc"


	{ ECPGtrans(__LINE__, NULL, "commit");
#line 67 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 67 "array.pgc"


	for (j = 0; j < 10; j++) {
		ts[j] = PGTYPEStimestamp_from_asc("1900-01-01 00:00:00", NULL);
		d[j] = PGTYPESdate_from_asc("1900-01-01", NULL);
		in[j] = *PGTYPESinterval_new();
		n[j] = *PGTYPESnumeric_new();
	}
	{ ECPGtrans(__LINE__, NULL, "begin work");
#line 75 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 75 "array.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select f , text from test where i = 1", ECPGt_EOIT, 
	ECPGt_double,&(f),(long)1,(long)1,sizeof(double), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(text),(long)25,(long)1,(25)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 80 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 80 "array.pgc"


	printf("Found f=%f text=%10.10s\n", f, text);

	f=140787;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select a , text , ts , n , d , inter from test where f = $1 ", 
	ECPGt_double,&(f),(long)1,(long)1,sizeof(double), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_short,(a),(long)1,(long)10,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(t),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_timestamp,&(ts),(long)1,(long)10,sizeof(timestamp), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_numeric,&(n),(long)1,(long)10,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_date,&(d),(long)1,(long)10,sizeof(date), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_interval,&(in),(long)1,(long)10,sizeof(interval), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 88 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 88 "array.pgc"


	for (i = 0; i < 10; i++)
		printf("Found a[%d] = %d ts[%d] = %s n[%d] = %s d[%d] = %s in[%d] = %s\n", i, a[i], i, PGTYPEStimestamp_to_asc(ts[i]), i, PGTYPESnumeric_to_asc(&(n[i]), -1), i, PGTYPESdate_to_asc(d[i]), i, PGTYPESinterval_to_asc(&(in[i])));

	printf("Found text=%10.10s\n", t);

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select a from test where f = $1 ", 
	ECPGt_double,&(f),(long)1,(long)1,sizeof(double), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_char,(text),(long)25,(long)1,(25)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 98 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 98 "array.pgc"


	printf("Found text=%s\n", text);

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table test", ECPGt_EOIT, ECPGt_EORT);
#line 102 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 102 "array.pgc"


	{ ECPGtrans(__LINE__, NULL, "commit");
#line 104 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 104 "array.pgc"


	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 106 "array.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 106 "array.pgc"


	free(t);

	return (0);
}
