/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "update.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#line 1 "regression.h"






#line 5 "update.pgc"


int main(int argc, char* argv[]) {
  /* exec sql begin declare section */
  	  
  
#line 9 "update.pgc"
 int  i1 [ 3 ]    ,  i2 [ 3 ]    ;
/* exec sql end declare section */
#line 10 "update.pgc"


  ECPGdebug(1, stderr);
  { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); }
#line 13 "update.pgc"


  /* exec sql whenever sql_warning  sqlprint ; */
#line 15 "update.pgc"

  /* exec sql whenever sqlerror  sqlprint ; */
#line 16 "update.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "create  table test ( a int   , b int   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 18 "update.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 18 "update.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 18 "update.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "insert into test ( a  , b  ) values ( 1 , 1 ) ", ECPGt_EOIT, ECPGt_EORT);
#line 20 "update.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 20 "update.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 20 "update.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "insert into test ( a  , b  ) values ( 2 , 2 ) ", ECPGt_EOIT, ECPGt_EORT);
#line 21 "update.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 21 "update.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 21 "update.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "insert into test ( a  , b  ) values ( 3 , 3 ) ", ECPGt_EOIT, ECPGt_EORT);
#line 22 "update.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 22 "update.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 22 "update.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "update test set a  = a + 1   ", ECPGt_EOIT, ECPGt_EORT);
#line 24 "update.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 24 "update.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 24 "update.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "update test set ( a  , b  )= ( 5 , 5 )  where a = 4 ", ECPGt_EOIT, ECPGt_EORT);
#line 25 "update.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 25 "update.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 25 "update.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "update test set a  = 4  where a = 3 ", ECPGt_EOIT, ECPGt_EORT);
#line 26 "update.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 26 "update.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "update.pgc"
;

  { ECPGdo(__LINE__, 0, 1, NULL, "select  a , b  from test    order by a", ECPGt_EOIT, 
	ECPGt_int,(i1),(long)1,(long)3,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(i2),(long)1,(long)3,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 28 "update.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 28 "update.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 28 "update.pgc"


  printf("test\na b\n%d %d\n%d %d\n%d %d\n", i1[0], i2[0], i1[1], i2[1], i1[2], i2[2]);

  { ECPGdisconnect(__LINE__, "ALL");
#line 32 "update.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 32 "update.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "update.pgc"


  return 0;
}
