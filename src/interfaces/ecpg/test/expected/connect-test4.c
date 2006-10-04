/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "test4.pgc"
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


#line 1 "regression.h"






#line 6 "test4.pgc"


int
main(void)
{
	ECPGdebug(1, stderr);

	{ ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , "main", 0); }
#line 13 "test4.pgc"


	{ ECPGsetconn(__LINE__, "main");}
#line 15 "test4.pgc"


	{ ECPGdisconnect(__LINE__, "DEFAULT");}
#line 17 "test4.pgc"


	return (0);
}
