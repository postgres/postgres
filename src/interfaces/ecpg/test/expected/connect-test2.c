/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "test2.pgc"
/*
 * this file tests multiple connections to databases and switches
 * between them.
 */

#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


#line 1 "regression.h"






#line 11 "test2.pgc"


int
main(void)
{
/* exec sql begin declare section */
	 
	 

#line 17 "test2.pgc"
 char id [ 200 ] ;
 
#line 18 "test2.pgc"
 char res [ 200 ] ;
/* exec sql end declare section */
#line 19 "test2.pgc"


	ECPGdebug(1, stderr);

	strcpy(id, "first");
	{ ECPGconnect(__LINE__, 0, "ecpg2_regression" , NULL, NULL , id, 0); }
#line 24 "test2.pgc"

	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , "second", 0); }
#line 25 "test2.pgc"


	/* this selects from "second" which was opened last */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select current_database ( )", ECPGt_EOIT, 
	ECPGt_char,(res),(long)200,(long)1,(200)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 28 "test2.pgc"

	{ ECPGdo(__LINE__, 0, 1, "first", 0, ECPGst_normal, "select current_database ( )", ECPGt_EOIT, 
	ECPGt_char,(res),(long)200,(long)1,(200)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 29 "test2.pgc"

	{ ECPGdo(__LINE__, 0, 1, "second", 0, ECPGst_normal, "select current_database ( )", ECPGt_EOIT, 
	ECPGt_char,(res),(long)200,(long)1,(200)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 30 "test2.pgc"


	{ ECPGsetconn(__LINE__, "first");}
#line 32 "test2.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select current_database ( )", ECPGt_EOIT, 
	ECPGt_char,(res),(long)200,(long)1,(200)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 33 "test2.pgc"


	/* this will disconnect from "first" */
	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 36 "test2.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select current_database ( )", ECPGt_EOIT, 
	ECPGt_char,(res),(long)200,(long)1,(200)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 37 "test2.pgc"


	/* error here since "first" is already disconnected */
	{ ECPGdisconnect(__LINE__, id);}
#line 40 "test2.pgc"


	/* disconnect from "second" */
	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 43 "test2.pgc"


	return 0;
}
