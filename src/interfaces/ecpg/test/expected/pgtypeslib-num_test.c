/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
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



/*

NOTE: This file has a different expect file for regression tests on MinGW32

*/


int
main(void)
{
	char *text="error\n";
	numeric *value1, *value2, *res;
	/* exec sql begin declare section */
		 
		/* = {0, 0, 0, 0, 0, NULL, NULL} ; */
	
#line 22 "num_test.pgc"
 numeric * des    ;
/* exec sql end declare section */
#line 24 "num_test.pgc"

	double d;
	long l1, l2;
	int i;

	ECPGdebug(1, stderr);
	/* exec sql whenever sqlerror  do sqlprint (  ) ; */
#line 30 "num_test.pgc"


	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); 
#line 32 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 32 "num_test.pgc"


	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 34 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 34 "num_test.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create  table test ( text char  ( 5 )    , num numeric ( 14 , 7 )   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 35 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 35 "num_test.pgc"


	value1 = PGTYPESnumeric_new();
	PGTYPESnumeric_from_int(1407, value1);
	text = PGTYPESnumeric_to_asc(value1, -1);
	printf("from int = %s\n", text);
	free(text);
	PGTYPESnumeric_free(value1);

	value1 = PGTYPESnumeric_from_asc("2369.7", NULL);
	value2 = PGTYPESnumeric_from_asc("10.0", NULL);
	res = PGTYPESnumeric_new();
	PGTYPESnumeric_add(value1, value2, res);
	text = PGTYPESnumeric_to_asc(res, -1);
	printf("add = %s\n", text);
	free(text);

	PGTYPESnumeric_sub(res, value2, res);
	text = PGTYPESnumeric_to_asc(res, -1);
	printf("sub = %s\n", text);
	free(text);
	PGTYPESnumeric_free(value2);

	des = PGTYPESnumeric_new();
	PGTYPESnumeric_copy(res, des);
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test ( text  , num  ) values ( 'test' ,  $1  ) ", 
	ECPGt_numeric,&(des),(long)1,(long)0,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 60 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 60 "num_test.pgc"


	value2 = PGTYPESnumeric_from_asc("2369.7", NULL);
	PGTYPESnumeric_mul(value1, value2, res);
	PGTYPESnumeric_free(value2);

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  num  from test where text = 'test'  ", ECPGt_EOIT, 
	ECPGt_numeric,&(des),(long)1,(long)0,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 66 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 66 "num_test.pgc"


	PGTYPESnumeric_mul(res, des, res);
	text = PGTYPESnumeric_to_asc(res, -1);
	printf("mul = %s\n", text);
	free(text);
	PGTYPESnumeric_free(des);

	value2 = PGTYPESnumeric_from_asc("10000", NULL);
	PGTYPESnumeric_div(res, value2, res);
	text = PGTYPESnumeric_to_asc(res, -1);
	PGTYPESnumeric_to_double(res, &d);
	printf("div = %s %e\n", text, d);

	value1 = PGTYPESnumeric_from_asc("2E7", NULL);
	value2 = PGTYPESnumeric_from_asc("14", NULL);
	i = PGTYPESnumeric_to_long(value1, &l1) | PGTYPESnumeric_to_long(value2, &l2);
	printf("to long(%d) = %ld %ld\n", i, l1, l2);

	free(text);
	PGTYPESnumeric_free(value1);
	PGTYPESnumeric_free(value2);
	PGTYPESnumeric_free(res);

	{ ECPGtrans(__LINE__, NULL, "rollback");
#line 90 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 90 "num_test.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 91 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 91 "num_test.pgc"


	return (0);
}

