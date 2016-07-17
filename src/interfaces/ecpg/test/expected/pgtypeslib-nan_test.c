/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "nan_test.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <math.h>
#include <pgtypes_numeric.h>
#include <decimal.h>


#line 1 "regression.h"






#line 8 "nan_test.pgc"


#ifdef WIN32
#define isinf(x) ((_fpclass(x) == _FPCLASS_PINF) || (_fpclass(x) == _FPCLASS_NINF))
#define isnan(x) _isnan(x)
#endif   /* WIN32 */

int
main(void)
{
	/* exec sql begin declare section */
		
		
		
		
	
#line 19 "nan_test.pgc"
 int id ;
 
#line 20 "nan_test.pgc"
 double d ;
 
#line 21 "nan_test.pgc"
 numeric * num ;
 
#line 22 "nan_test.pgc"
 char val [ 16 ] ;
/* exec sql end declare section */
#line 23 "nan_test.pgc"


	ECPGdebug(1, stderr);
	/* exec sql whenever sqlerror  do sqlprint ( ) ; */
#line 26 "nan_test.pgc"


	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 28 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 28 "nan_test.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table nantest1 ( id int4 , d float8 )", ECPGt_EOIT, ECPGt_EORT);
#line 30 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 30 "nan_test.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into nantest1 ( id , d ) values ( 1 , 'nan' :: float8 ) , ( 2 , 'infinity' :: float8 ) , ( 3 , '-infinity' :: float8 )", ECPGt_EOIT, ECPGt_EORT);
#line 31 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 31 "nan_test.pgc"


	/* declare cur cursor for select id , d , d from nantest1 */
#line 33 "nan_test.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cur cursor for select id , d , d from nantest1", ECPGt_EOIT, ECPGt_EORT);
#line 34 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 34 "nan_test.pgc"

	while (1)
	{
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch from cur", ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_double,&(d),(long)1,(long)1,sizeof(double), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(val),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 37 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 37 "nan_test.pgc"

		if (sqlca.sqlcode)
			break;
		if (isnan(d))
			printf("%d  NaN '%s'\n", id, val);
		else if (isinf(d))
			printf("%d %sInf '%s'\n", id, (d < 0 ? "-" : "+"), val);

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into nantest1 ( id , d ) values ( $1  + 3 , $2  )", 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_double,&(d),(long)1,(long)1,sizeof(double), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 45 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 45 "nan_test.pgc"

		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into nantest1 ( id , d ) values ( $1  + 6 , $2  )", 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(val),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 46 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 46 "nan_test.pgc"

	}
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cur", ECPGt_EOIT, ECPGt_EORT);
#line 48 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 48 "nan_test.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cur cursor for select id , d , d from nantest1", ECPGt_EOIT, ECPGt_EORT);
#line 50 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 50 "nan_test.pgc"

	while (1)
	{
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch from cur", ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_double,&(d),(long)1,(long)1,sizeof(double), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(val),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 53 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 53 "nan_test.pgc"

		if (sqlca.sqlcode)
			break;
		if (isinf(d))
			printf("%d %sInf '%s'\n", id, (d < 0 ? "-" : "+"), val);
		if (isnan(d))
			printf("%d  NaN '%s'\n", id, val);
	}
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cur", ECPGt_EOIT, ECPGt_EORT);
#line 61 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 61 "nan_test.pgc"


	num = PGTYPESnumeric_new();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table nantest2 ( id int4 , d numeric )", ECPGt_EOIT, ECPGt_EORT);
#line 65 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 65 "nan_test.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into nantest2 ( id , d ) values ( 4 , 'nan' :: numeric )", ECPGt_EOIT, ECPGt_EORT);
#line 66 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 66 "nan_test.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select id , d , d from nantest2 where id = 4", ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_numeric,&(num),(long)1,(long)0,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(val),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 68 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 68 "nan_test.pgc"


	printf("%d %s '%s'\n", id, (num->sign == NUMERIC_NAN ? "NaN" : "not NaN"), val);

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into nantest2 ( id , d ) values ( 5 , $1  )", 
	ECPGt_numeric,&(num),(long)1,(long)0,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 72 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 72 "nan_test.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into nantest2 ( id , d ) values ( 6 , $1  )", 
	ECPGt_char,(val),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 73 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 73 "nan_test.pgc"


	/* declare cur1 cursor for select id , d , d from nantest2 */
#line 75 "nan_test.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cur1 cursor for select id , d , d from nantest2", ECPGt_EOIT, ECPGt_EORT);
#line 76 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 76 "nan_test.pgc"

	while (1)
	{
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch from cur1", ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_numeric,&(num),(long)1,(long)0,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(val),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 79 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 79 "nan_test.pgc"

		if (sqlca.sqlcode)
			break;
		printf("%d %s '%s'\n", id, (num->sign == NUMERIC_NAN ? "NaN" : "not NaN"), val);
	}
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cur1", ECPGt_EOIT, ECPGt_EORT);
#line 84 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 84 "nan_test.pgc"


	PGTYPESnumeric_free(num);

	{ ECPGtrans(__LINE__, NULL, "rollback");
#line 88 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 88 "nan_test.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 89 "nan_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 89 "nan_test.pgc"


	return (0);
}
