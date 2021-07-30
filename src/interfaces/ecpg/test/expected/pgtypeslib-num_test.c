/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "num_test.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <pgtypes_numeric.h>
#include <decimal.h>


#line 1 "regression.h"






#line 6 "num_test.pgc"



#line 1 "printf_hack.h"
/*
 * print_double(x) has the same effect as printf("%g", x), but is intended
 * to produce the same formatting across all platforms.
 */
static void
print_double(double x)
{
#ifdef WIN32
	/* Change Windows' 3-digit exponents to look like everyone else's */
	char		convert[128];
	int			vallen;

	sprintf(convert, "%g", x);
	vallen = strlen(convert);

	if (vallen >= 6 &&
		convert[vallen - 5] == 'e' &&
		convert[vallen - 3] == '0')
	{
		convert[vallen - 3] = convert[vallen - 2];
		convert[vallen - 2] = convert[vallen - 1];
		convert[vallen - 1] = '\0';
	}

	printf("%s", convert);
#else
	printf("%g", x);
#endif
}

#line 8 "num_test.pgc"



int
main(void)
{
	char *text="error\n";
	numeric *value1, *value2, *res;
	/* exec sql begin declare section */
		 
		/* = {0, 0, 0, 0, 0, NULL, NULL} ; */
	
#line 17 "num_test.pgc"
 numeric * des ;
/* exec sql end declare section */
#line 19 "num_test.pgc"

	double d;
	long l1, l2;
	int i, min, max;

	ECPGdebug(1, stderr);
	/* exec sql whenever sqlerror  do sqlprint ( ) ; */
#line 25 "num_test.pgc"


	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 27 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 27 "num_test.pgc"


	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 29 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 29 "num_test.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table test ( text char ( 5 ) , num numeric ( 14 , 7 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 30 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 30 "num_test.pgc"


	value1 = PGTYPESnumeric_new();
	PGTYPESnumeric_from_int(1407, value1);
	text = PGTYPESnumeric_to_asc(value1, -1);
	printf("from int = %s\n", text);
	PGTYPESchar_free(text);
	PGTYPESnumeric_free(value1);

	value1 = PGTYPESnumeric_from_asc("2369.7", NULL);
	value2 = PGTYPESnumeric_from_asc("10.0", NULL);
	res = PGTYPESnumeric_new();
	PGTYPESnumeric_add(value1, value2, res);
	text = PGTYPESnumeric_to_asc(res, -1);
	printf("add = %s\n", text);
	PGTYPESchar_free(text);

	PGTYPESnumeric_sub(res, value2, res);
	text = PGTYPESnumeric_to_asc(res, -1);
	printf("sub = %s\n", text);
	PGTYPESchar_free(text);
	PGTYPESnumeric_free(value2);

	des = PGTYPESnumeric_new();
	PGTYPESnumeric_copy(res, des);
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test ( text , num ) values ( 'test' , $1  )", 
	ECPGt_numeric,&(des),(long)1,(long)0,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 55 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 55 "num_test.pgc"


	value2 = PGTYPESnumeric_from_asc("2369.7", NULL);
	PGTYPESnumeric_mul(value1, value2, res);
	PGTYPESnumeric_free(value2);

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select num from test where text = 'test'", ECPGt_EOIT, 
	ECPGt_numeric,&(des),(long)1,(long)0,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 61 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 61 "num_test.pgc"


	PGTYPESnumeric_mul(res, des, res);
	text = PGTYPESnumeric_to_asc(res, -1);
	printf("mul = %s\n", text);
	PGTYPESchar_free(text);
	PGTYPESnumeric_free(des);

	value2 = PGTYPESnumeric_from_asc("10000", NULL);
	PGTYPESnumeric_div(res, value2, res);
	text = PGTYPESnumeric_to_asc(res, -1);
	PGTYPESnumeric_to_double(res, &d);
	printf("div = %s ", text);
	print_double(d);
	printf("\n");

	PGTYPESnumeric_free(value1);
	PGTYPESnumeric_free(value2);

	value1 = PGTYPESnumeric_from_asc("2E7", NULL);
	value2 = PGTYPESnumeric_from_asc("14", NULL);
	i = PGTYPESnumeric_to_long(value1, &l1) | PGTYPESnumeric_to_long(value2, &l2);
	printf("to long(%d) = %ld %ld\n", i, l1, l2);

	PGTYPESchar_free(text);
	PGTYPESnumeric_free(value1);
	PGTYPESnumeric_free(value2);
	PGTYPESnumeric_free(res);

	/* check conversion of numeric to int */
	value1 = PGTYPESnumeric_from_asc("-2147483648", NULL);
	PGTYPESnumeric_to_int(value1, &min);
	printf("min int = %d\n", min);
	PGTYPESnumeric_free(value1);

	value2 = PGTYPESnumeric_from_asc("2147483647", NULL);
	PGTYPESnumeric_to_int(value2, &max);
	printf("max int = %d\n", max);
	PGTYPESnumeric_free(value2);

	{ ECPGtrans(__LINE__, NULL, "rollback");
#line 101 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 101 "num_test.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 102 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 102 "num_test.pgc"


	return 0;
}
