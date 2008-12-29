/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "test5.pgc"
/*
 * this file tests all sorts of connecting to one single database.
 */

#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* do not include regression.h */

int
main(void)
{
/* exec sql begin declare section */
	 
	 
	 

#line 16 "test5.pgc"
 char db [ 200 ] ;
 
#line 17 "test5.pgc"
 char id [ 200 ] ;
 
#line 18 "test5.pgc"
 char * user = "connectuser" ;
/* exec sql end declare section */
#line 19 "test5.pgc"


	ECPGdebug(1, stderr);

	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL, NULL , "main", 0); }
#line 23 "test5.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "alter user connectuser encrypted password 'connectpw'", ECPGt_EOIT, ECPGt_EORT);}
#line 24 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 25 "test5.pgc"
  /* <-- "main" not specified */

	strcpy(db, "connectdb");
	strcpy(id, "main");
	{ ECPGconnect(__LINE__, 0, db , NULL, NULL , id, 0); }
#line 29 "test5.pgc"

	{ ECPGdisconnect(__LINE__, id);}
#line 30 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL, NULL , "main", 0); }
#line 32 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 33 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL, NULL , "main", 0); }
#line 35 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 36 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL, NULL , "main", 0); }
#line 38 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 39 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "" , "connectdb" , NULL , "main", 0); }
#line 41 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 42 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb" , "connectuser" , "connectdb" , "main", 0); }
#line 44 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 45 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost/connectdb" , "connectuser" , NULL , "main", 0); }
#line 47 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 48 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost/connectdb" , "connectuser" , NULL , "main", 0); }
#line 50 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 51 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost/connectdb" , user , NULL , "main", 0); }
#line 53 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 54 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://200.46.204.71/connectdb" , "connectuser" , NULL , "main", 0); }
#line 56 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 57 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost/" , "connectdb" , NULL , "main", 0); }
#line 59 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 60 "test5.pgc"


	/* connect twice */
	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL, NULL , "main", 0); }
#line 63 "test5.pgc"

	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL, NULL , "main", 0); }
#line 64 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 65 "test5.pgc"


	/* not connected */
	{ ECPGdisconnect(__LINE__, "nonexistant");}
#line 68 "test5.pgc"


	return (0);
}
