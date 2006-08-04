
THE PORT NUMBER MIGHT HAVE BEEN CHANGED BY THE REGRESSION SCRIPT

/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "test1.pgc"
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
	 
	 
	 

#line 16 "test1.pgc"
 char  db [ 200 ]    ;
 
#line 17 "test1.pgc"
 char  id [ 200 ]    ;
 
#line 18 "test1.pgc"
 char  pw [ 200 ]    ;
/* exec sql end declare section */
#line 19 "test1.pgc"


	ECPGdebug(1, stderr);

	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL,NULL , "main", 0); }
#line 23 "test1.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "alter user connectuser  encrypted password 'connectpw'", ECPGt_EOIT, ECPGt_EORT);}
#line 24 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 25 "test1.pgc"
  /* <-- "main" not specified */

	strcpy(db, "connectdb");
	strcpy(id, "main");
	{ ECPGconnect(__LINE__, 0, db , NULL,NULL , id, 0); }
#line 29 "test1.pgc"

	{ ECPGdisconnect(__LINE__, id);}
#line 30 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb@localhost" , NULL,NULL , "main", 0); }
#line 32 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 33 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb@localhost" , NULL,NULL , "main", 0); }
#line 35 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 36 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb@localhost" , "connectuser" , "connectdb" , "main", 0); }
#line 38 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 39 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "tcp:postgresql://localhost:55432/connectdb" , "connectuser" , "connectpw" , NULL, 0); }
#line 41 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "nonexistant");}
#line 42 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 43 "test1.pgc"


	strcpy(pw, "connectpw");
	strcpy(db, "tcp:postgresql://localhost:55432/connectdb");
	{ ECPGconnect(__LINE__, 0, db , "connectuser" , pw , NULL, 0); }
#line 47 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 48 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost:55432/connectdb" , "connectuser" , "connectpw" , NULL, 0); }
#line 50 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 51 "test1.pgc"


	/* wrong db */
	{ ECPGconnect(__LINE__, 0, "tcp:postgresql://localhost:55432/nonexistant" , "connectuser" , "connectpw" , NULL, 0); }
#line 54 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 55 "test1.pgc"


	/* wrong port */
	{ ECPGconnect(__LINE__, 0, "tcp:postgresql://localhost:0/connectdb" , "connectuser" , "connectpw" , NULL, 0); }
#line 58 "test1.pgc"

	/* no disconnect necessary */

	/* wrong password */
	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost:55432/connectdb" , "connectuser" , "wrongpw" , NULL, 0); }
#line 62 "test1.pgc"

	/* no disconnect necessary */

	/* connect twice */
	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL,NULL , "main", 0); }
#line 66 "test1.pgc"

	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL,NULL , "main", 0); }
#line 67 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 68 "test1.pgc"


	return (0);
}
