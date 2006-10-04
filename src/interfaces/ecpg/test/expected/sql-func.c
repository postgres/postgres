/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "func.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#line 1 "regression.h"






#line 5 "func.pgc"


int main(int argc, char* argv[]) {

  ECPGdebug(1, stderr);
  { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); }
#line 10 "func.pgc"


  { ECPGsetcommit(__LINE__, "on", NULL);}
#line 12 "func.pgc"

  /* exec sql whenever sql_warning  sqlprint ; */
#line 13 "func.pgc"

  /* exec sql whenever sqlerror  sqlprint ; */
#line 14 "func.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "create  table My_Table ( Item1 int   , Item2 text   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 16 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 16 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 16 "func.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "create  function My_Table_Check () returns trigger  as $test$\
    BEGIN\
	RAISE WARNING 'Notice: TG_NAME=%, TG WHEN=%', TG_NAME, TG_WHEN;\
	RETURN NEW;\
    END; $test$ language plpgsql", ECPGt_EOIT, ECPGt_EORT);
#line 24 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 24 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 24 "func.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "create trigger My_Table_Check_Trigger before insert on My_Table for each row execute procedure My_Table_Check (  )", ECPGt_EOIT, ECPGt_EORT);
#line 30 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 30 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 30 "func.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "insert into My_Table values ( 1234 , 'Some random text' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 32 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 32 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "func.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "insert into My_Table values ( 5678 , 'The Quick Brown' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 33 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 33 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 33 "func.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "drop trigger My_Table_Check_Trigger on My_Table ", ECPGt_EOIT, ECPGt_EORT);
#line 35 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 35 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 35 "func.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "drop function My_Table_Check () ", ECPGt_EOIT, ECPGt_EORT);
#line 36 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 36 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "func.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "drop table My_Table ", ECPGt_EOIT, ECPGt_EORT);
#line 37 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 37 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 37 "func.pgc"


  { ECPGdisconnect(__LINE__, "ALL");
#line 39 "func.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 39 "func.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 39 "func.pgc"


  return 0;
}
