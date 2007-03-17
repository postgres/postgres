/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "comment.pgc"
#include <stdlib.h>


#line 1 "regression.h"






#line 3 "comment.pgc"


/* just a test comment */ int i;
/* just a test comment int j*/;

/****************************************************************************/
/* Test comment                                                             */
/*--------------------------------------------------------------------------*/

int main(void)
{
  ECPGdebug(1, stderr);

  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 17 "comment.pgc"


  { ECPGdisconnect(__LINE__, "CURRENT");}
#line 19 "comment.pgc"

  exit (0);
}

