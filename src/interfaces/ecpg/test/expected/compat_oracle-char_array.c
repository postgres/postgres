/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "char_array.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#line 1 "regression.h"






#line 5 "char_array.pgc"


static void warn(void)
{
  fprintf(stderr, "Warning: At least one column was truncated\n");
}

/* Compatible handling of char array to retrieve varchar field to char array
   should be fixed-length, blank-padded, then null-terminated.
   Conforms to the ANSI Fixed Character type. */

int main() {

  /* exec sql whenever sql_warning  do warn ( ) ; */
#line 18 "char_array.pgc"

  /* exec sql whenever sqlerror  stop ; */
#line 19 "char_array.pgc"


  const char *ppppp = "XXXXX";
  int loopcount;
  /* exec sql begin declare section */
   
   
     
     
  
#line 24 "char_array.pgc"
 char shortstr [ 5 ] ;
 
#line 25 "char_array.pgc"
 char bigstr [ 11 ] ;
 
#line 26 "char_array.pgc"
 short shstr_ind = 0 ;
 
#line 27 "char_array.pgc"
 short bigstr_ind = 0 ;
/* exec sql end declare section */
#line 28 "char_array.pgc"


  ECPGdebug(1, stderr);
  { ECPGconnect(__LINE__, 3, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 31 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 31 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 31 "char_array.pgc"


  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "create table strdbase ( strval varchar ( 10 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 33 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 33 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 33 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "insert into strdbase values ( '' )", ECPGt_EOIT, ECPGt_EORT);
#line 34 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 34 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 34 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "insert into strdbase values ( 'AB' )", ECPGt_EOIT, ECPGt_EORT);
#line 35 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 35 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 35 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "insert into strdbase values ( 'ABCD' )", ECPGt_EOIT, ECPGt_EORT);
#line 36 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 36 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 36 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "insert into strdbase values ( 'ABCDE' )", ECPGt_EOIT, ECPGt_EORT);
#line 37 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 37 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 37 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "insert into strdbase values ( 'ABCDEF' )", ECPGt_EOIT, ECPGt_EORT);
#line 38 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 38 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 38 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "insert into strdbase values ( 'ABCDEFGHIJ' )", ECPGt_EOIT, ECPGt_EORT);
#line 39 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 39 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 39 "char_array.pgc"


  /* declare C cursor for select strval , strval from strdbase */
#line 41 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "declare C cursor for select strval , strval from strdbase", ECPGt_EOIT, ECPGt_EORT);
#line 42 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 42 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 42 "char_array.pgc"


  /* exec sql whenever not found  break ; */
#line 44 "char_array.pgc"


  printf("Full Str.  :  Short  Ind.\n");
  for (loopcount = 0; loopcount < 100; loopcount++) {
    strncpy(shortstr, ppppp, sizeof shortstr);
    memset(bigstr, 0, sizeof bigstr);
    { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "fetch C", ECPGt_EOIT, 
	ECPGt_char,(bigstr),(long)11,(long)1,(11)*sizeof(char), 
	ECPGt_short,&(bigstr_ind),(long)1,(long)1,sizeof(short), 
	ECPGt_char,(shortstr),(long)5,(long)1,(5)*sizeof(char), 
	ECPGt_short,&(shstr_ind),(long)1,(long)1,sizeof(short), ECPGt_EORT);
#line 50 "char_array.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 50 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 50 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 50 "char_array.pgc"

    printf("\"%s\": \"%s\"  %d\n", bigstr, shortstr, shstr_ind);
  }

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "close C", ECPGt_EOIT, ECPGt_EORT);
#line 54 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 54 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 54 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "drop table strdbase", ECPGt_EOIT, ECPGt_EORT);
#line 55 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 55 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 55 "char_array.pgc"


  printf("\nGOOD-BYE!!\n\n");

  { ECPGtrans(__LINE__, NULL, "commit work");
#line 59 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 59 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 59 "char_array.pgc"


  { ECPGdisconnect(__LINE__, "ALL");
#line 61 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 61 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 61 "char_array.pgc"


  return 0;
}
