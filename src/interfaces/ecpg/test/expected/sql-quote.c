/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "quote.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#line 1 "regression.h"






#line 5 "quote.pgc"


int main(int argc, char* argv[]) {
  /* exec sql begin declare section */
     
  
#line 9 "quote.pgc"
 char  var [ 25 ]    ;
/* exec sql end declare section */
#line 10 "quote.pgc"


  ECPGdebug(1, stderr);
  { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); }
#line 13 "quote.pgc"


  { ECPGsetcommit(__LINE__, "on", NULL);}
#line 15 "quote.pgc"

  /* exec sql whenever sql_warning  sqlprint ; */
#line 16 "quote.pgc"

  /* exec sql whenever sqlerror  sqlprint ; */
#line 17 "quote.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "create  table \"My_Table\" ( Item1 int   , Item2 text   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 19 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 19 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 19 "quote.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "show standard_conforming_strings", ECPGt_EOIT, 
	ECPGt_char,(var),(long)25,(long)1,(25)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 21 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 21 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 21 "quote.pgc"

  printf("Standard conforming strings: %s\n", var);

  /* this is a\\b actually */
  { ECPGdo(__LINE__, 0, 1, NULL, "insert into \"My_Table\" values ( 1 , 'a\\\\b' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 25 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 25 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 25 "quote.pgc"

  /* this is a\b */
  { ECPGdo(__LINE__, 0, 1, NULL, "insert into \"My_Table\" values ( 1 , 'a\\\\b' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 27 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 27 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 27 "quote.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "set standard_conforming_strings to on", ECPGt_EOIT, ECPGt_EORT);
#line 29 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 29 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 29 "quote.pgc"


  /* this is a\\b actually */
  { ECPGdo(__LINE__, 0, 1, NULL, "insert into \"My_Table\" values ( 1 , 'a\\\\b' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 32 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 32 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "quote.pgc"

  /* this is a\b */
  { ECPGdo(__LINE__, 0, 1, NULL, "insert into \"My_Table\" values ( 1 , 'a\\\\b' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 34 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 34 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 34 "quote.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "select  *  from \"My_Table\"   ", ECPGt_EOIT, ECPGt_EORT);
#line 36 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 36 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "quote.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "drop table \"My_Table\" ", ECPGt_EOIT, ECPGt_EORT);
#line 38 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 38 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 38 "quote.pgc"


  { ECPGdisconnect(__LINE__, "ALL");
#line 40 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 40 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 40 "quote.pgc"


  return 0;
}
