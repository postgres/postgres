/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "insupd.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#line 1 "regression.h"






#line 5 "insupd.pgc"


int main(int argc, char* argv[]) {
  /* exec sql begin declare section */
  	  
  
#line 9 "insupd.pgc"
 int  i1 [ 3 ]    ,  i2 [ 3 ]    ;
/* exec sql end declare section */
#line 10 "insupd.pgc"


  ECPGdebug(1, stderr);
  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 13 "insupd.pgc"


  /* exec sql whenever sql_warning  sqlprint ; */
#line 15 "insupd.pgc"

  /* exec sql whenever sqlerror  sqlprint ; */
#line 16 "insupd.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create  table insupd_test ( a int   , b int   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 18 "insupd.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 18 "insupd.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 18 "insupd.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into insupd_test ( a  , b  ) values ( 1 , 1 ) ", ECPGt_EOIT, ECPGt_EORT);
#line 20 "insupd.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 20 "insupd.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 20 "insupd.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into insupd_test ( a  , b  ) values ( 2 , 2 ) ", ECPGt_EOIT, ECPGt_EORT);
#line 21 "insupd.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 21 "insupd.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 21 "insupd.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into insupd_test ( a  , b  ) values ( 3 , 3 ) ", ECPGt_EOIT, ECPGt_EORT);
#line 22 "insupd.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 22 "insupd.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 22 "insupd.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update insupd_test set a  = a + 1   ", ECPGt_EOIT, ECPGt_EORT);
#line 24 "insupd.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 24 "insupd.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 24 "insupd.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update insupd_test set ( a  , b  )= ( 5 , 5 )  where a = 4 ", ECPGt_EOIT, ECPGt_EORT);
#line 25 "insupd.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 25 "insupd.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 25 "insupd.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "update insupd_test set a  = 4  where a = 3 ", ECPGt_EOIT, ECPGt_EORT);
#line 26 "insupd.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 26 "insupd.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "insupd.pgc"
;

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  a , b  from insupd_test    order by a  ", ECPGt_EOIT, 
	ECPGt_int,(i1),(long)1,(long)3,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(i2),(long)1,(long)3,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 28 "insupd.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 28 "insupd.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 28 "insupd.pgc"


  printf("test\na b\n%d %d\n%d %d\n%d %d\n", i1[0], i2[0], i1[1], i2[1], i1[2], i2[2]);

  { ECPGdisconnect(__LINE__, "ALL");
#line 32 "insupd.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 32 "insupd.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "insupd.pgc"


  return 0;
}
