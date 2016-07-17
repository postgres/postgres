/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "func.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#line 1 "regression.h"






#line 5 "func.pgc"


int main() {
  
#line 8 "func.pgc"
 char text [ 25 ] ;

#line 8 "func.pgc"


  ECPGdebug(1, stderr);
  { ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); }
#line 11 "func.pgc"


  { ECPGsetcommit(__LINE__, "on", NULL);}
#line 13 "func.pgc"

  /* exec sql whenever sql_warning  sqlprint ; */
#line 14 "func.pgc"

  /* exec sql whenever sqlerror  sqlprint ; */
#line 15 "func.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table My_Table ( Item1 int , Item2 text )", ECPGt_EOIT, ECPGt_EORT);
#line 17 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 17 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 17 "func.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table Log ( name text , w text )", ECPGt_EOIT, ECPGt_EORT);
#line 18 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 18 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 18 "func.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create function My_Table_Check ( ) returns trigger as $test$\
    BEGIN\
	INSERT INTO Log VALUES(TG_NAME, TG_WHEN);\
	RETURN NEW;\
    END; $test$ language plpgsql", ECPGt_EOIT, ECPGt_EORT);
#line 26 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 26 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "func.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create trigger My_Table_Check_Trigger before insert on My_Table for each row execute procedure My_Table_Check ( )", ECPGt_EOIT, ECPGt_EORT);
#line 32 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 32 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "func.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into My_Table values ( 1234 , 'Some random text' )", ECPGt_EOIT, ECPGt_EORT);
#line 34 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 34 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 34 "func.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into My_Table values ( 5678 , 'The Quick Brown' )", ECPGt_EOIT, ECPGt_EORT);
#line 35 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 35 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 35 "func.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select name from Log limit 1", ECPGt_EOIT, 
	ECPGt_char,(text),(long)25,(long)1,(25)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 36 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 36 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "func.pgc"

  printf("Trigger %s fired.\n", text);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop trigger My_Table_Check_Trigger on My_Table", ECPGt_EOIT, ECPGt_EORT);
#line 39 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 39 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 39 "func.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop function My_Table_Check ( )", ECPGt_EOIT, ECPGt_EORT);
#line 40 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 40 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 40 "func.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table Log", ECPGt_EOIT, ECPGt_EORT);
#line 41 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 41 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 41 "func.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table My_Table", ECPGt_EOIT, ECPGt_EORT);
#line 42 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 42 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 42 "func.pgc"


  { ECPGdisconnect(__LINE__, "ALL");
#line 44 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 44 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 44 "func.pgc"


  return 0;
}
