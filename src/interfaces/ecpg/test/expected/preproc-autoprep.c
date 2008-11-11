/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "autoprep.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* test automatic prepare for all statements */

#line 1 "regression.h"






#line 6 "autoprep.pgc"


int main(int argc, char* argv[]) {
  /* exec sql begin declare section */
  	     
  
#line 10 "autoprep.pgc"
 int  item  [ 4 ]   ,  ind  [ 4 ]   ,  i   = 1 ;
/* exec sql end declare section */
#line 11 "autoprep.pgc"


  ECPGdebug(1, stderr);
  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 14 "autoprep.pgc"


  /* exec sql whenever sql_warning  sqlprint ; */
#line 16 "autoprep.pgc"

  /* exec sql whenever sqlerror  sqlprint ; */
#line 17 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "create  table T ( Item1 int   , Item2 int   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 19 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 19 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 19 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "insert into T values ( 1 , null ) ", ECPGt_EOIT, ECPGt_EORT);
#line 21 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 21 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 21 "autoprep.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "insert into T values ( 1 ,  $1  ) ", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 22 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 22 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 22 "autoprep.pgc"

  i++;
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "insert into T values ( 1 ,  $1  ) ", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 24 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 24 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 24 "autoprep.pgc"

  { ECPGprepare(__LINE__, NULL, 0, "i", " insert into T values ( 1 , 2 )  ");
#line 25 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 25 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 25 "autoprep.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, 1, "i", ECPGt_EOIT, ECPGt_EORT);
#line 26 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 26 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "select  Item2  from T    order by Item2  nulls last", ECPGt_EOIT, 
	ECPGt_int,(item),(long)1,(long)4,sizeof(int), 
	ECPGt_int,(ind),(long)1,(long)4,sizeof(int), ECPGt_EORT);
#line 28 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 28 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 28 "autoprep.pgc"


  for (i=0; i<4; i++)
  	printf("item[%d] = %d\n", i, ind[i] ? -1 : item[i]);

  /* declare C  cursor  for select  Item1  from T    */
#line 33 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "declare C  cursor  for select  Item1  from T   ", ECPGt_EOIT, ECPGt_EORT);
#line 35 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 35 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 35 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "fetch 1 in C", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 37 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 37 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 37 "autoprep.pgc"

  printf("i = %d\n", i);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "close C", ECPGt_EOIT, ECPGt_EORT);
#line 40 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 40 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 40 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "drop table T ", ECPGt_EOIT, ECPGt_EORT);
#line 42 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 42 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 42 "autoprep.pgc"


  { ECPGdisconnect(__LINE__, "ALL");
#line 44 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 44 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 44 "autoprep.pgc"


  return 0;
}
