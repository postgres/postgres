/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

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
 
#line 10 "quote.pgc"
 int  i    ;
/* exec sql end declare section */
#line 11 "quote.pgc"


  ECPGdebug(1, stderr);
  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 14 "quote.pgc"


  { ECPGsetcommit(__LINE__, "on", NULL);}
#line 16 "quote.pgc"

  /* exec sql whenever sql_warning  sqlprint ; */
#line 17 "quote.pgc"

  /* exec sql whenever sqlerror  sqlprint ; */
#line 18 "quote.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create  table \"My_Table\" ( Item1 int   , Item2 text    )    ", ECPGt_EOIT, ECPGt_EORT);
#line 20 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 20 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 20 "quote.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "show standard_conforming_strings", ECPGt_EOIT, 
	ECPGt_char,(var),(long)25,(long)1,(25)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 22 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 22 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 22 "quote.pgc"

  printf("Standard conforming strings: %s\n", var);

  /* this is a\\b actually */
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into \"My_Table\" values ( 1 , 'a\\\\\\\\b' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 26 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 26 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "quote.pgc"

  /* this is a\\b */
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into \"My_Table\" values ( 1 , E'a\\\\\\\\b' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 28 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 28 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 28 "quote.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set standard_conforming_strings to on", ECPGt_EOIT, ECPGt_EORT);
#line 30 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 30 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 30 "quote.pgc"


  /* this is a\\\\b actually */
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into \"My_Table\" values ( 2 , 'a\\\\\\\\b' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 33 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 33 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 33 "quote.pgc"

  /* this is a\\b */
  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into \"My_Table\" values ( 2 , E'a\\\\\\\\b' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 35 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 35 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 35 "quote.pgc"


  { ECPGtrans(__LINE__, NULL, "begin transaction ");
#line 37 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 37 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 37 "quote.pgc"

  /* declare C  cursor  for select  *  from \"My_Table\"    */
#line 38 "quote.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare C  cursor  for select  *  from \"My_Table\"   ", ECPGt_EOIT, ECPGt_EORT);
#line 40 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 40 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 40 "quote.pgc"


  /* exec sql whenever not found  break ; */
#line 42 "quote.pgc"


  while (true)
  {
  	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch C", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(var),(long)25,(long)1,(25)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 46 "quote.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 46 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 46 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 46 "quote.pgc"

	printf("value: %d %s\n", i, var);
  }

  { ECPGtrans(__LINE__, NULL, "rollback");
#line 50 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 50 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 50 "quote.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table \"My_Table\" ", ECPGt_EOIT, ECPGt_EORT);
#line 51 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 51 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 51 "quote.pgc"


  { ECPGdisconnect(__LINE__, "ALL");
#line 53 "quote.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 53 "quote.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 53 "quote.pgc"


  return 0;
}
