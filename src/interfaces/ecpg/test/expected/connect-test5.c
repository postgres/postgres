/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

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
 char  db [ 200 ]    ;
 
#line 17 "test5.pgc"
 char  id [ 200 ]    ;
/* exec sql end declare section */
#line 18 "test5.pgc"


	ECPGdebug(1, stderr);

	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL,NULL , "main", 0); }
#line 22 "test5.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "alter user connectuser  encrypted password 'connectpw'", ECPGt_EOIT, ECPGt_EORT);}
#line 23 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 24 "test5.pgc"
  /* <-- "main" not specified */

	strcpy(db, "connectdb");
	strcpy(id, "main");
	{ ECPGconnect(__LINE__, 0, db , NULL,NULL , id, 0); }
#line 28 "test5.pgc"

	{ ECPGdisconnect(__LINE__, id);}
#line 29 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL,NULL , "main", 0); }
#line 31 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 32 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL,NULL , "main", 0); }
#line 34 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 35 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL,NULL , "main", 0); }
#line 37 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 38 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "" , "connectdb" , NULL , "main", 0); }
#line 40 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 41 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "connectdb" , "connectuser" , "connectdb" , "main", 0); }
#line 43 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 44 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost/connectdb" , "connectuser" , NULL , "main", 0); }
#line 46 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 47 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost/connectdb" , "connectuser" , NULL , "main", 0); }
#line 49 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 50 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost/connectdb" , "connectuser" , NULL , "main", 0); }
#line 52 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 53 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://200.46.204.71/connectdb" , "connectuser" , NULL , "main", 0); }
#line 55 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 56 "test5.pgc"


	{ ECPGconnect(__LINE__, 0, "unix:postgresql://localhost/" , "connectdb" , NULL , "main", 0); }
#line 58 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 59 "test5.pgc"


	/* connect twice */
	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL,NULL , "main", 0); }
#line 62 "test5.pgc"

	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL,NULL , "main", 0); }
#line 63 "test5.pgc"

	{ ECPGdisconnect(__LINE__, "main");}
#line 64 "test5.pgc"


	/* not connected */
	{ ECPGdisconnect(__LINE__, "nonexistant");}
#line 67 "test5.pgc"


	return (0);
}
