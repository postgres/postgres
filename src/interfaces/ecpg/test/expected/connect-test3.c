/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "test3.pgc"
/*
 * this file just tests the several possibilities you have for a disconnect
 */

#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


#line 1 "regression.h"






#line 10 "test3.pgc"


int
main(void)
{
/* exec sql begin declare section */
	 
	 

#line 16 "test3.pgc"
 char  id  [ 200 ]   ;
 
#line 17 "test3.pgc"
 char  res  [ 200 ]   ;
/* exec sql end declare section */
#line 18 "test3.pgc"


	ECPGdebug(1, stderr);

	strcpy(id, "first");
	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL, NULL , id, 0); }
#line 23 "test3.pgc"

	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , "second", 0); }
#line 24 "test3.pgc"


	/* this selects from "second" which was opened last */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  current_database ( )     ", ECPGt_EOIT, 
	ECPGt_char,(res),(long)200,(long)1,(200)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 27 "test3.pgc"


	/* will close "second" */
	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 30 "test3.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  current_database ( )     ", ECPGt_EOIT, 
	ECPGt_char,(res),(long)200,(long)1,(200)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 31 "test3.pgc"


	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , "second", 0); }
#line 33 "test3.pgc"

	/* will close "second" */
	{ ECPGdisconnect(__LINE__, "DEFAULT");}
#line 35 "test3.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL, NULL , "second", 0); }
#line 37 "test3.pgc"

	{ ECPGdisconnect(__LINE__, "ALL");}
#line 38 "test3.pgc"


	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 40 "test3.pgc"

	{ ECPGdisconnect(__LINE__, "DEFAULT");}
#line 41 "test3.pgc"

	{ ECPGdisconnect(__LINE__, "ALL");}
#line 42 "test3.pgc"


	/*
	 * exec sql disconnect;
	 * exec sql disconnect name;
	 *
	 *     are used in other tests
	 */

	return (0);
}
