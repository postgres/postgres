/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "parser.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* test parser addition that merges two tokens into one */

#line 1 "regression.h"






#line 6 "parser.pgc"


int main(int argc, char* argv[]) {
  /* exec sql begin declare section */
  	   
  
#line 10 "parser.pgc"
 int  item [ 3 ]    ,  ind [ 3 ]    ,  i    ;
/* exec sql end declare section */
#line 11 "parser.pgc"


  ECPGdebug(1, stderr);
  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 14 "parser.pgc"


  { ECPGsetcommit(__LINE__, "on", NULL);}
#line 16 "parser.pgc"

  /* exec sql whenever sql_warning  sqlprint ; */
#line 17 "parser.pgc"

  /* exec sql whenever sqlerror  sqlprint ; */
#line 18 "parser.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create  table T ( Item1 int   , Item2 int   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 20 "parser.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 20 "parser.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 20 "parser.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into T values ( 1 , null ) ", ECPGt_EOIT, ECPGt_EORT);
#line 22 "parser.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 22 "parser.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 22 "parser.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into T values ( 1 , 1 ) ", ECPGt_EOIT, ECPGt_EORT);
#line 23 "parser.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 23 "parser.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 23 "parser.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into T values ( 1 , 2 ) ", ECPGt_EOIT, ECPGt_EORT);
#line 24 "parser.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 24 "parser.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 24 "parser.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  Item2  from T    order by Item2  nulls last", ECPGt_EOIT, 
	ECPGt_int,(item),(long)1,(long)3,sizeof(int), 
	ECPGt_int,(ind),(long)1,(long)3,sizeof(int), ECPGt_EORT);
#line 26 "parser.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 26 "parser.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "parser.pgc"


  for (i=0; i<3; i++)
  	printf("item[%d] = %d\n", i, ind[i] ? -1 : item[i]);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "alter table T alter  Item1  type bigint  ", ECPGt_EOIT, ECPGt_EORT);
#line 31 "parser.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 31 "parser.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 31 "parser.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "alter table T alter column Item2 set data type smallint  ", ECPGt_EOIT, ECPGt_EORT);
#line 32 "parser.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 32 "parser.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "parser.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table T ", ECPGt_EOIT, ECPGt_EORT);
#line 34 "parser.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 34 "parser.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 34 "parser.pgc"


  { ECPGdisconnect(__LINE__, "ALL");
#line 36 "parser.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 36 "parser.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "parser.pgc"


  return 0;
}
