/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "num_test.pgc"
/* $PostgreSQL: pgsql/src/interfaces/ecpg/test/expected/pgtypeslib-num_test.c,v 1.1 2006/08/02 14:14:03 meskes Exp $ */

#include <stdio.h>
#include <stdlib.h>
#include <pgtypes_numeric.h>
#include <decimal.h>


#line 1 "./../regression.h"






#line 8 "num_test.pgc"


int
main(void)
{
	char *text="error\n";
	numeric *value1, *value2, *res;
	/* exec sql begin declare section */
		 
		/* = {0, 0, 0, 0, 0, NULL, NULL} ; */
	
#line 16 "num_test.pgc"
 numeric * des    ;
/* exec sql end declare section */
#line 18 "num_test.pgc"

	double d;

	ECPGdebug(1, stderr);
	/* exec sql whenever sqlerror  do sqlprint (  ) ; */
#line 22 "num_test.pgc"


	{ ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); 
#line 24 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 24 "num_test.pgc"


	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 26 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 26 "num_test.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "create  table test ( text char  ( 5 )    , num numeric ( 14 , 7 )   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 27 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 27 "num_test.pgc"


	value1 = PGTYPESnumeric_new();
	PGTYPESnumeric_from_int(1407, value1);
	text = PGTYPESnumeric_to_asc(value1, -1);
	printf("long = %s\n", text);
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
	{ ECPGdo(__LINE__, 0, 1, NULL, "insert into test ( text  , num  ) values( 'test' ,  ? )", 
	ECPGt_numeric,&(des),(long)1,(long)0,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 52 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 52 "num_test.pgc"


	value2 = PGTYPESnumeric_from_asc("2369.7", NULL);
	PGTYPESnumeric_mul(value1, value2, res);
	PGTYPESnumeric_free(value2);

	{ ECPGdo(__LINE__, 0, 1, NULL, "select  num  from test where text = 'test'  ", ECPGt_EOIT, 
	ECPGt_numeric,&(des),(long)1,(long)0,sizeof(numeric), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 58 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 58 "num_test.pgc"


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
	free(text);
	PGTYPESnumeric_free(value1);
	PGTYPESnumeric_free(value2);
	PGTYPESnumeric_free(res);

	{ ECPGtrans(__LINE__, NULL, "rollback");
#line 76 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 76 "num_test.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 77 "num_test.pgc"

if (sqlca.sqlcode < 0) sqlprint (  );}
#line 77 "num_test.pgc"


	return (0);
}

