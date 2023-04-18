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

#include <pgtypes_numeric.h>


#line 1 "sqlda.h"
#ifndef ECPG_SQLDA_H
#define ECPG_SQLDA_H

#ifdef _ECPG_INFORMIX_H

#include "sqlda-compat.h"
typedef struct sqlvar_compat sqlvar_t;
typedef struct sqlda_compat sqlda_t;

#else

#include "sqlda-native.h"
typedef struct sqlvar_struct sqlvar_t;
typedef struct sqlda_struct sqlda_t;

#endif

#endif							/* ECPG_SQLDA_H */

#line 7 "char_array.pgc"



#line 1 "regression.h"






#line 9 "char_array.pgc"


static void warn(void)
{
  fprintf(stderr, "Warning: At least one column was truncated\n");
}

/* Compatible handling of char array to retrieve varchar field to char array
   should be fixed-length, blank-padded, then null-terminated.
   Conforms to the ANSI Fixed Character type. */

int main() {

  /* exec sql whenever sql_warning  do warn ( ) ; */
#line 22 "char_array.pgc"

  /* exec sql whenever sqlerror  stop ; */
#line 23 "char_array.pgc"


  const char *ppppp = "XXXXX";
  int loopcount;
  sqlda_t *sqlda = NULL;

  /* exec sql begin declare section */
   
   
     
     
  
#line 30 "char_array.pgc"
 char shortstr [ 5 ] ;
 
#line 31 "char_array.pgc"
 char bigstr [ 11 ] ;
 
#line 32 "char_array.pgc"
 short shstr_ind = 0 ;
 
#line 33 "char_array.pgc"
 short bigstr_ind = 0 ;
/* exec sql end declare section */
#line 34 "char_array.pgc"


  ECPGdebug(1, stderr);
  { ECPGconnect(__LINE__, 3, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 37 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 37 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 37 "char_array.pgc"


  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "create table strdbase ( strval varchar ( 10 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 39 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 39 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 39 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "insert into strdbase values ( '' )", ECPGt_EOIT, ECPGt_EORT);
#line 40 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 40 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 40 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "insert into strdbase values ( 'AB' )", ECPGt_EOIT, ECPGt_EORT);
#line 41 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 41 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 41 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "insert into strdbase values ( 'ABCD' )", ECPGt_EOIT, ECPGt_EORT);
#line 42 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 42 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 42 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "insert into strdbase values ( 'ABCDE' )", ECPGt_EOIT, ECPGt_EORT);
#line 43 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 43 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 43 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "insert into strdbase values ( 'ABCDEF' )", ECPGt_EOIT, ECPGt_EORT);
#line 44 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 44 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 44 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "insert into strdbase values ( 'ABCDEFGHIJ' )", ECPGt_EOIT, ECPGt_EORT);
#line 45 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 45 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 45 "char_array.pgc"


  /* declare C cursor for select strval , strval from strdbase */
#line 47 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "declare C cursor for select strval , strval from strdbase", ECPGt_EOIT, ECPGt_EORT);
#line 48 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 48 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 48 "char_array.pgc"


  /* exec sql whenever not found  break ; */
#line 50 "char_array.pgc"


  printf("Full Str.  :  Short  Ind.\n");
  for (loopcount = 0; loopcount < 100; loopcount++) {
    strncpy(shortstr, ppppp, sizeof shortstr);
    memset(bigstr, 0, sizeof bigstr);
    { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "fetch C", ECPGt_EOIT, 
	ECPGt_char,(bigstr),(long)11,(long)1,(11)*sizeof(char), 
	ECPGt_short,&(bigstr_ind),(long)1,(long)1,sizeof(short), 
	ECPGt_char,(shortstr),(long)5,(long)1,(5)*sizeof(char), 
	ECPGt_short,&(shstr_ind),(long)1,(long)1,sizeof(short), ECPGt_EORT);
#line 56 "char_array.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 56 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 56 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 56 "char_array.pgc"

    printf("\"%s\": \"%s\"  %d\n", bigstr, shortstr, shstr_ind);
  }

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "close C", ECPGt_EOIT, ECPGt_EORT);
#line 60 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 60 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 60 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "drop table strdbase", ECPGt_EOIT, ECPGt_EORT);
#line 61 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 61 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 61 "char_array.pgc"

  { ECPGtrans(__LINE__, NULL, "commit work");
#line 62 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') warn ( );
#line 62 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 62 "char_array.pgc"


  /* SQLDA handling */
  /* exec sql whenever sql_warning  sqlprint ; */
#line 65 "char_array.pgc"

  /* exec sql whenever not found  stop ; */
#line 66 "char_array.pgc"

  { ECPGprepare(__LINE__, NULL, 0, "stmt1", "SELECT 123::numeric(3,0), 't'::varchar(2)");
#line 67 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 67 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 67 "char_array.pgc"

  /* declare cur1 cursor for $1 */
#line 68 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "declare cur1 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt1", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 69 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 69 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 69 "char_array.pgc"

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "fetch next from cur1", ECPGt_EOIT, 
	ECPGt_sqlda, &sqlda, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 70 "char_array.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) exit (1);
#line 70 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 70 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 70 "char_array.pgc"


  printf("\n-----------------\ntype    : data\n");
  for (int i = 0 ; i < sqlda->sqld ; i++)
  {
	  sqlvar_t v = sqlda->sqlvar[i];
	  char *sqldata = v.sqldata;

	  if (v.sqltype == ECPGt_numeric)
		  sqldata =
			  PGTYPESnumeric_to_asc((numeric*) sqlda->sqlvar[i].sqldata, -1);

	  printf("%-8s: \"%s\"\n", v.sqlname.data, sqldata);
  }

  { ECPGdo(__LINE__, 3, 1, NULL, 0, ECPGst_normal, "close cur1", ECPGt_EOIT, ECPGt_EORT);
#line 85 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 85 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 85 "char_array.pgc"

  { ECPGtrans(__LINE__, NULL, "commit work");
#line 86 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 86 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 86 "char_array.pgc"


  printf("\nGOOD-BYE!!\n\n");

  { ECPGdisconnect(__LINE__, "ALL");
#line 90 "char_array.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 90 "char_array.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 90 "char_array.pgc"


  return 0;
}
