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


static void test(void) {
  /* exec sql begin declare section */
  	     
	  
	   
  
#line 10 "autoprep.pgc"
 int item [ 4 ] , ind [ 4 ] , i = 1 ;
 
#line 11 "autoprep.pgc"
 int item1 , ind1 ;
 
#line 12 "autoprep.pgc"
 char sqlstr [ 64 ] = "SELECT item2 FROM T ORDER BY item2 NULLS LAST" ;
/* exec sql end declare section */
#line 13 "autoprep.pgc"


  ECPGdebug(1, stderr);
  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 16 "autoprep.pgc"


  /* exec sql whenever sql_warning  sqlprint ; */
#line 18 "autoprep.pgc"

  /* exec sql whenever sqlerror  sqlprint ; */
#line 19 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table T ( Item1 int , Item2 int )", ECPGt_EOIT, ECPGt_EORT);
#line 21 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 21 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 21 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "insert into T values ( 1 , null )", ECPGt_EOIT, ECPGt_EORT);
#line 23 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 23 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 23 "autoprep.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "insert into T values ( 1 , $1  )", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 24 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 24 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 24 "autoprep.pgc"

  i++;
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "insert into T values ( 1 , $1  )", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 26 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 26 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "autoprep.pgc"

  { ECPGprepare(__LINE__, NULL, 0, "i", " insert into T values ( 1 , 2 ) ");
#line 27 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 27 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 27 "autoprep.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "i", ECPGt_EOIT, ECPGt_EORT);
#line 28 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 28 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 28 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepnormal, "select Item2 from T order by Item2 nulls last", ECPGt_EOIT, 
	ECPGt_int,(item),(long)1,(long)4,sizeof(int), 
	ECPGt_int,(ind),(long)1,(long)4,sizeof(int), ECPGt_EORT);
#line 30 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 30 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 30 "autoprep.pgc"


  for (i=0; i<4; i++)
  	printf("item[%d] = %d\n", i, ind[i] ? -1 : item[i]);

  /* declare C cursor for select Item1 from T */
#line 35 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare C cursor for select Item1 from T", ECPGt_EOIT, ECPGt_EORT);
#line 37 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 37 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 37 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch 1 in C", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 39 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 39 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 39 "autoprep.pgc"

  printf("i = %d\n", i);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close C", ECPGt_EOIT, ECPGt_EORT);
#line 42 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 42 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 42 "autoprep.pgc"


  { ECPGprepare(__LINE__, NULL, 0, "stmt1", sqlstr);
#line 44 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 44 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 44 "autoprep.pgc"


  /* declare cur1 cursor for $1 */
#line 46 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cur1 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt1", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 48 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 48 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 48 "autoprep.pgc"


  /* exec sql whenever not found  break ; */
#line 50 "autoprep.pgc"


  i = 0;
  while (1)
  {
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cur1", ECPGt_EOIT, 
	ECPGt_int,&(item1),(long)1,(long)1,sizeof(int), 
	ECPGt_int,&(ind1),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 55 "autoprep.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 55 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 55 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 55 "autoprep.pgc"

	printf("item[%d] = %d\n", i, ind1 ? -1 : item1);
	i++;
  }

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cur1", ECPGt_EOIT, ECPGt_EORT);
#line 60 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 60 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 60 "autoprep.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table T", ECPGt_EOIT, ECPGt_EORT);
#line 62 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 62 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 62 "autoprep.pgc"


  { ECPGdisconnect(__LINE__, "ALL");
#line 64 "autoprep.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 64 "autoprep.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 64 "autoprep.pgc"

}

int main() {
  test();
  test();     /* retry */

  return 0;
}
