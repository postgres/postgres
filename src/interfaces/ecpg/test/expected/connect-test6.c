/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "test6.pgc"
/*
 * This test verifies that ecpg functions properly handle NULL connections
 * (i.e., when a connection name doesn't exist or has been disconnected).
 * Before the fix, these operations would cause a segmentation fault.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>


#line 1 "regression.h"






#line 11 "test6.pgc"


int
main(void)
{
/* exec sql begin declare section */
	   
	   
	   
	   

#line 17 "test6.pgc"
 int val1output = 2 ;
 
#line 18 "test6.pgc"
 int val1 = 1 ;
 
#line 19 "test6.pgc"
 char val2 [ 6 ] = "data1" ;
 
#line 20 "test6.pgc"
 char * stmt1 = "SELECT * from test1 where a = $1 and b = $2" ;
/* exec sql end declare section */
#line 21 "test6.pgc"


	ECPGdebug(1, stderr);

	/* Connect to the database */
	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , "myconn", 0); }
#line 26 "test6.pgc"


	/* Test 1: Try to get descriptor on a disconnected connection */
	printf("Test 1: Try to get descriptor on a disconnected connection\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table test1 ( a int , b text )", ECPGt_EOIT, ECPGt_EORT);}
#line 30 "test6.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test1 ( a , b ) values ( 1 , 'data1' )", ECPGt_EOIT, ECPGt_EORT);}
#line 31 "test6.pgc"


	ECPGallocate_desc(__LINE__, "indesc");
#line 33 "test6.pgc"

	ECPGallocate_desc(__LINE__, "outdesc");
#line 34 "test6.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "foo2", stmt1);}
#line 36 "test6.pgc"


	{ ECPGset_desc(__LINE__, "indesc", 1,ECPGd_data,
	ECPGt_int,&(val1),(long)1,(long)1,sizeof(int), ECPGd_EODT);
}
#line 38 "test6.pgc"

	{ ECPGset_desc(__LINE__, "indesc", 2,ECPGd_data,
	ECPGt_char,(val2),(long)6,(long)1,(6)*sizeof(char), ECPGd_EODT);
}
#line 39 "test6.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "foo2", 
	ECPGt_descriptor, "indesc", 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_descriptor, "outdesc", 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 41 "test6.pgc"


	{ ECPGtrans(__LINE__, NULL, "rollback");}
#line 43 "test6.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 44 "test6.pgc"

	{ ECPGget_desc(__LINE__, "outdesc", 1,ECPGd_data,
	ECPGt_int,&(val1output),(long)1,(long)1,sizeof(int), ECPGd_EODT);
}
#line 45 "test6.pgc"

	printf("sqlca.sqlcode = %ld\n", sqlca.sqlcode);

	/* Test 2: Try to deallocate all on a non-existent connection */
	printf("Test 2: deallocate all with non-existent connection\n");
	{ ECPGdeallocate_all(__LINE__, 0, "nonexistent");}
#line 50 "test6.pgc"

	printf("sqlca.sqlcode = %ld\n", sqlca.sqlcode);

	/* Test 3: deallocate on disconnected connection */
	printf("Test 3: deallocate all on disconnected connection\n");
	{ ECPGdeallocate_all(__LINE__, 0, NULL);}
#line 55 "test6.pgc"

	printf("sqlca.sqlcode = %ld\n", sqlca.sqlcode);

	/* Test 4: Use prepared statement from non-existent connection */
	printf("Test 4: Use prepared statement from non-existent connection\n");
	{ ECPGprepare(__LINE__, "nonexistent", 0, "stmt1", "SELECT 1");}
#line 60 "test6.pgc"

	/* declare cur1 cursor for $1 */
#line 61 "test6.pgc"

	{ ECPGdo(__LINE__, 0, 1, "nonexistent", 0, ECPGst_normal, "declare cur1 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement("nonexistent", "stmt1", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);}
#line 62 "test6.pgc"

	printf("sqlca.sqlcode = %ld\n", sqlca.sqlcode);

	printf("All tests completed !\n");

	return 0;
}
