
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
 char  pw [ 200 ]    ;
/* exec sql end declare section */
#line 18 "test1.pgc"


	ECPGdebug(1, stderr);

	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL,NULL , "main", 0); }
#line 22 "test1.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "alter user connectuser  encrypted password 'connectpw'", ECPGt_EOIT, ECPGt_EORT);}
#line 23 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 24 "test1.pgc"
  /* <-- "main" not specified */

	{ ECPGconnect(__LINE__, 0, "connectdb@localhost" , NULL,NULL , "main", 0); }
#line 26 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 27 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "@localhost" , "connectdb" , NULL , "main", 0); }
#line 29 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 30 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb@localhost:55432" , NULL,NULL , "main", 0); }
#line 32 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 33 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "@localhost:55432" , "connectdb" , NULL , "main", 0); }
#line 35 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 36 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb:55432" , NULL,NULL , "main", 0); }
#line 38 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 39 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, ":55432" , "connectdb" , NULL , "main", 0); }
#line 41 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 42 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "tcp:postgresql://localhost:55432/connectdb" , "connectuser" , "connectpw" , NULL, 0); }
#line 44 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 45 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "tcp:postgresql://localhost:55432/" , "connectdb" , NULL , NULL, 0); }
#line 47 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 48 "test1.pgc"


	strcpy(pw, "connectpw");
	strcpy(db, "tcp:postgresql://localhost:55432/connectdb");
	{ ECPGconnect(__LINE__, 0, db , "connectuser" , pw , NULL, 0); }
#line 52 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 53 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost:55432/connectdb" , "connectuser" , "connectpw" , NULL, 0); }
#line 55 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 56 "test1.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost:55432/connectdb" , "connectuser" , NULL , NULL, 0); }
#line 58 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 59 "test1.pgc"


	/* wrong db */
	{ ECPGconnect(__LINE__, 0, "tcp:postgresql://localhost:55432/nonexistant" , "connectuser" , "connectpw" , NULL, 0); }
#line 62 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 63 "test1.pgc"


	/* wrong port */
	{ ECPGconnect(__LINE__, 0, "tcp:postgresql://localhost:20/connectdb" , "connectuser" , "connectpw" , NULL, 0); }
#line 66 "test1.pgc"

	/* no disconnect necessary */

	/* wrong password */
	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost:55432/connectdb" , "connectuser" , "wrongpw" , NULL, 0); }
#line 70 "test1.pgc"

	/* no disconnect necessary */

	return (0);
}
