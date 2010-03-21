/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* Needed for informix compatibility */
#include <ecpg_informix.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "rnull.pgc"
#include "sqltypes.h"
#include <stdlib.h>


#line 1 "regression.h"






#line 4 "rnull.pgc"



static void
test_null(int type, char *ptr)
{
	printf("null: %d\n", risnull(type, ptr));
}

int main(void)
{
	
#line 15 "rnull.pgc"
 char c [] = "abc" ;

#line 15 "rnull.pgc"

	
#line 16 "rnull.pgc"
 short s = 17 ;

#line 16 "rnull.pgc"

	
#line 17 "rnull.pgc"
 int i = - 74874 ;

#line 17 "rnull.pgc"

	
#line 18 "rnull.pgc"
 bool b = 1 ;

#line 18 "rnull.pgc"

	
#line 19 "rnull.pgc"
 float f = 3.71 ;

#line 19 "rnull.pgc"

	
#line 20 "rnull.pgc"
 long l = 487444 ;

#line 20 "rnull.pgc"

	
#line 21 "rnull.pgc"
 double dbl = 404.404 ;

#line 21 "rnull.pgc"

	
#line 22 "rnull.pgc"
 decimal dec ;

#line 22 "rnull.pgc"

	
#line 23 "rnull.pgc"
 date dat ;

#line 23 "rnull.pgc"

	
#line 24 "rnull.pgc"
 timestamp tmp ;

#line 24 "rnull.pgc"


	ECPGdebug(1, stderr);
	/* exec sql whenever sqlerror  do sqlprint ( ) ; */
#line 27 "rnull.pgc"


	{ ECPGconnect(__LINE__, 1, "regress1" , NULL, NULL , NULL, 0); 
#line 29 "rnull.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 29 "rnull.pgc"


	{ ECPGdo(__LINE__, 1, 0, NULL, 0, ECPGst_normal, "create table test ( id int , c char ( 10 ) , s smallint , i int , b bool , f float , l bigint , dbl double precision , dec decimal , dat date , tmp timestamptz )", ECPGt_EOIT, ECPGt_EORT);
#line 33 "rnull.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 33 "rnull.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 34 "rnull.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 34 "rnull.pgc"


	{ ECPGdo(__LINE__, 1, 0, NULL, 0, ECPGst_normal, "insert into test ( id , c , s , i , b , f , l , dbl ) values ( 1 , $1  , $2  , $3  , $4  , $5  , $6  , $7  )", 
	ECPGt_char,(c),(long)sizeof("abc"),(long)1,(sizeof("abc"))*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(s),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(b),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_float,&(f),(long)1,(long)1,sizeof(float), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_long,&(l),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_double,&(dbl),(long)1,(long)1,sizeof(double), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 38 "rnull.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 38 "rnull.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 39 "rnull.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 39 "rnull.pgc"


	rsetnull(CCHARTYPE, (char *) c);
	rsetnull(CSHORTTYPE, (char *) &s);
	rsetnull(CINTTYPE, (char *) &i);
	rsetnull(CBOOLTYPE, (char *) &b);
	rsetnull(CFLOATTYPE, (char *) &f);
	rsetnull(CLONGTYPE, (char *) &l);
	rsetnull(CDOUBLETYPE, (char *) &dbl);
	rsetnull(CDECIMALTYPE, (char *) &dec);
	rsetnull(CDATETYPE, (char *) &dat);
	rsetnull(CDTIMETYPE, (char *) &tmp);

	{ ECPGdo(__LINE__, 1, 0, NULL, 0, ECPGst_normal, "insert into test ( id , c , s , i , b , f , l , dbl , dec , dat , tmp ) values ( 2 , $1  , $2  , $3  , $4  , $5  , $6  , $7  , $8  , $9  , $10  )", 
	ECPGt_char,(c),(long)sizeof("abc"),(long)1,(sizeof("abc"))*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(s),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(b),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_float,&(f),(long)1,(long)1,sizeof(float), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_long,&(l),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_double,&(dbl),(long)1,(long)1,sizeof(double), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_decimal,&(dec),(long)1,(long)1,sizeof(decimal), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_date,&(dat),(long)1,(long)1,sizeof(date), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_timestamp,&(tmp),(long)1,(long)1,sizeof(timestamp), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 54 "rnull.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 54 "rnull.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 55 "rnull.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 55 "rnull.pgc"


	printf("first select\n");

	{ ECPGdo(__LINE__, 1, 0, NULL, 0, ECPGst_normal, "select c , s , i , b , f , l , dbl , dec , dat , tmp from test where id = 1", ECPGt_EOIT, 
	ECPGt_char,(c),(long)sizeof("abc"),(long)1,(sizeof("abc"))*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(s),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(b),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_float,&(f),(long)1,(long)1,sizeof(float), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_long,&(l),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_double,&(dbl),(long)1,(long)1,sizeof(double), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_decimal,&(dec),(long)1,(long)1,sizeof(decimal), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_date,&(dat),(long)1,(long)1,sizeof(date), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_timestamp,&(tmp),(long)1,(long)1,sizeof(timestamp), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 61 "rnull.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 61 "rnull.pgc"


	test_null(CCHARTYPE, (char *) c);
	test_null(CSHORTTYPE, (char *) &s);
	test_null(CINTTYPE, (char *) &i);
	test_null(CBOOLTYPE, (char *) &b);
	test_null(CFLOATTYPE, (char *) &f);
	test_null(CLONGTYPE, (char *) &l);
	test_null(CDOUBLETYPE, (char *) &dbl);
	test_null(CDECIMALTYPE, (char *) &dec);
	test_null(CDATETYPE, (char *) &dat);
	test_null(CDTIMETYPE, (char *) &tmp);

	printf("second select\n");

	{ ECPGdo(__LINE__, 1, 0, NULL, 0, ECPGst_normal, "select c , s , i , b , f , l , dbl , dec , dat , tmp from test where id = 2", ECPGt_EOIT, 
	ECPGt_char,(c),(long)sizeof("abc"),(long)1,(sizeof("abc"))*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(s),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(b),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_float,&(f),(long)1,(long)1,sizeof(float), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_long,&(l),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_double,&(dbl),(long)1,(long)1,sizeof(double), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_decimal,&(dec),(long)1,(long)1,sizeof(decimal), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_date,&(dat),(long)1,(long)1,sizeof(date), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_timestamp,&(tmp),(long)1,(long)1,sizeof(timestamp), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 78 "rnull.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 78 "rnull.pgc"


	test_null(CCHARTYPE, (char *) c);
	test_null(CSHORTTYPE, (char *) &s);
	test_null(CINTTYPE, (char *) &i);
	test_null(CBOOLTYPE, (char *) &b);
	test_null(CFLOATTYPE, (char *) &f);
	test_null(CLONGTYPE, (char *) &l);
	test_null(CDOUBLETYPE, (char *) &dbl);
	test_null(CDECIMALTYPE, (char *) &dec);
	test_null(CDATETYPE, (char *) &dat);
	test_null(CDTIMETYPE, (char *) &tmp);

	{ ECPGdo(__LINE__, 1, 0, NULL, 0, ECPGst_normal, "drop table test", ECPGt_EOIT, ECPGt_EORT);
#line 91 "rnull.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 91 "rnull.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 92 "rnull.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 92 "rnull.pgc"


	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 94 "rnull.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 94 "rnull.pgc"


	return 0;
}
