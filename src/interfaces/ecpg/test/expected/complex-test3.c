/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "test3.pgc"
/****************************************************************************/
/* Test comment                                                             */
/*--------------------------------------------------------------------------*/

#line 1 "./header_test.h"
#include "stdlib.h"

static void
Finish(char *msg)
{
	fprintf(stderr, "Error in statement '%s':\n", msg);
	sqlprint();

	/* finish transaction */
	{ ECPGtrans(__LINE__, NULL, "rollback");}
#line 10 "./header_test.h"


	/* and remove test table */
	{ ECPGdo(__LINE__, 0, 1, NULL, "drop table meskes ", ECPGt_EOIT, ECPGt_EORT);}
#line 13 "./header_test.h"

	{ ECPGtrans(__LINE__, NULL, "commit");}
#line 14 "./header_test.h"


	{ ECPGdisconnect(__LINE__, "CURRENT");}
#line 16 "./header_test.h"


	exit(-1);
}

static void
warn(void)
{
	fprintf(stderr, "Warning: At least one column was truncated\n");
}

/* exec sql whenever sqlerror  do Finish ( msg ) ; */
#line 29 "./header_test.h"

/* exec sql whenever sql_warning  do warn (  ) ; */
#line 32 "./header_test.h"


#line 4 "test3.pgc"


#line 1 "./../regression.h"






#line 5 "test3.pgc"


/* exec sql type str is  [ 10 ]   */
#line 7 "test3.pgc"


#include <stdlib.h>
#include <string.h>

int
main (void)
{
/* exec sql begin declare section */
	        typedef struct { 
#line 16 "test3.pgc"
 long  born    ;
 
#line 16 "test3.pgc"
 short  age    ;
 }  birthinfo ;

#line 16 "test3.pgc"

	 		 
					 
				 
	  	 
					 
				   
	   
	    
	 
	   
	 
	 
 
#line 19 "test3.pgc"
 struct personal_struct { 
#line 17 "test3.pgc"
   struct varchar_name  { int len; char arr[ 10 ]; }  name    ;
 
#line 18 "test3.pgc"
 birthinfo  birth    ;
 }  personal    ;
 
#line 22 "test3.pgc"
 struct personal_indicator { 
#line 20 "test3.pgc"
 int  ind_name    ;
 
#line 21 "test3.pgc"
 birthinfo  ind_birth    ;
 }  ind_personal    ;
 
#line 23 "test3.pgc"
 int * ind_married   = NULL ;
 
#line 24 "test3.pgc"
 int  children    ,  movevalue   = 2 ;
 
#line 25 "test3.pgc"
 int  ind_children    ;
 
#line 26 "test3.pgc"
   struct varchar_married  { int len; char arr[ 10 ]; } * married  = NULL ;
 
#line 27 "test3.pgc"
 char * wifesname   = "Petra" ;
 
#line 28 "test3.pgc"
 char * query   = "select * from meskes where name = ?" ;
/* exec sql end declare section */
#line 29 "test3.pgc"


	/* declare cur  cursor  for select  name , born , age , married , children  from meskes    */
#line 32 "test3.pgc"


	char msg[128];

	ECPGdebug(1, stderr);

	strcpy(msg, "connect");
	{ ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); 
#line 39 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 39 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 39 "test3.pgc"


	strcpy(msg, "create");
	{ ECPGdo(__LINE__, 0, 1, NULL, "create  table meskes ( name char  ( 8 )    , born integer   , age smallint   , married date   , children integer   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 42 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 42 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 42 "test3.pgc"


	strcpy(msg, "insert");
	{ ECPGdo(__LINE__, 0, 1, NULL, "insert into meskes ( name  , married  , children  ) values(  ? , '19900404' , 3 )", 
	ECPGt_char,&(wifesname),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 45 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 45 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 45 "test3.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "insert into meskes ( name  , born  , age  , married  , children  ) values( 'Michael' , 19660117 , 35 , '19900404' , 3 )", ECPGt_EOIT, ECPGt_EORT);
#line 46 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 46 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 46 "test3.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "insert into meskes ( name  , born  , age  ) values( 'Carsten' , 19910103 , 10 )", ECPGt_EOIT, ECPGt_EORT);
#line 47 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 47 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 47 "test3.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "insert into meskes ( name  , born  , age  ) values( 'Marc' , 19930907 , 8 )", ECPGt_EOIT, ECPGt_EORT);
#line 48 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 48 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 48 "test3.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "insert into meskes ( name  , born  , age  ) values( 'Chris' , 19970923 , 4 )", ECPGt_EOIT, ECPGt_EORT);
#line 49 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 49 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 49 "test3.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 52 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 52 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 52 "test3.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, NULL, "declare cur  cursor  for select  name , born , age , married , children  from meskes   ", ECPGt_EOIT, ECPGt_EORT);
#line 55 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 55 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 55 "test3.pgc"


	strcpy(msg, "move");
	{ ECPGdo(__LINE__, 0, 1, NULL, "move  ? in cur", 
	ECPGt_int,&(movevalue),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 58 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 58 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 58 "test3.pgc"


	/* exec sql whenever not found  break ; */
#line 60 "test3.pgc"


	while (1) {
		strcpy(msg, "fetch");
		{ ECPGdo(__LINE__, 0, 1, NULL, "fetch from cur", ECPGt_EOIT, 
	ECPGt_varchar,&(personal.name),(long)10,(long)1,sizeof(struct varchar_name), 
	ECPGt_int,&(ind_personal.ind_name),(long)1,(long)1,sizeof(int), 
	ECPGt_long,&(personal.birth.born),(long)1,(long)1,sizeof(long), 
	ECPGt_long,&(ind_personal.ind_birth.born),(long)1,(long)1,sizeof(long), 
	ECPGt_short,&(personal.birth.age),(long)1,(long)1,sizeof(short), 
	ECPGt_short,&(ind_personal.ind_birth.age),(long)1,(long)1,sizeof(short), 
	ECPGt_varchar,&(married),(long)10,(long)0,sizeof(struct varchar_married), 
	ECPGt_int,&(ind_married),(long)1,(long)0,sizeof(int), 
	ECPGt_int,&(children),(long)1,(long)1,sizeof(int), 
	ECPGt_int,&(ind_children),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 64 "test3.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 64 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 64 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 64 "test3.pgc"

		printf("%8.8s", personal.name.arr);
		if (ind_personal.ind_birth.born >= 0)
			printf(", born %ld", personal.birth.born);
		if (ind_personal.ind_birth.age >= 0)
			printf(", age = %d", personal.birth.age);
		if (*ind_married >= 0)
			printf(", married %10.10s", married->arr);
		if (ind_children >= 0)
			printf(", children = %d", children);
		putchar('\n');

		free(married);
		married = NULL;
	}

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, NULL, "close cur", ECPGt_EOIT, ECPGt_EORT);
#line 81 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 81 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 81 "test3.pgc"


	/* and now a query with prepare */
	{ ECPGprepare(__LINE__, "MM" , query);
#line 84 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 84 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 84 "test3.pgc"

	/* declare prep  cursor  for ? */
#line 85 "test3.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, NULL, "declare prep  cursor  for ?", 
	ECPGt_char_variable,(ECPGprepared_statement("MM")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(wifesname),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 88 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 88 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 88 "test3.pgc"


	/* exec sql whenever not found  break ; */
#line 90 "test3.pgc"


	while (1) {
		strcpy(msg, "fetch");
		{ ECPGdo(__LINE__, 0, 1, NULL, "fetch in prep", ECPGt_EOIT, 
	ECPGt_varchar,&(personal.name),(long)10,(long)1,sizeof(struct varchar_name), 
	ECPGt_int,&(ind_personal.ind_name),(long)1,(long)1,sizeof(int), 
	ECPGt_long,&(personal.birth.born),(long)1,(long)1,sizeof(long), 
	ECPGt_long,&(ind_personal.ind_birth.born),(long)1,(long)1,sizeof(long), 
	ECPGt_short,&(personal.birth.age),(long)1,(long)1,sizeof(short), 
	ECPGt_short,&(ind_personal.ind_birth.age),(long)1,(long)1,sizeof(short), 
	ECPGt_varchar,&(married),(long)10,(long)0,sizeof(struct varchar_married), 
	ECPGt_int,&(ind_married),(long)1,(long)0,sizeof(int), 
	ECPGt_int,&(children),(long)1,(long)1,sizeof(int), 
	ECPGt_int,&(ind_children),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 94 "test3.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 94 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 94 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 94 "test3.pgc"

		printf("%8.8s", personal.name.arr);
		if (ind_personal.ind_birth.born >= 0)
			printf(", born %ld", personal.birth.born);
		if (ind_personal.ind_birth.age >= 0)
			printf(", age = %d", personal.birth.age);
		if (*ind_married >= 0)
			printf(", married %10.10s", married->arr);
		if (ind_children >= 0)
			printf(", children = %d", children);
		putchar('\n');
	}

	free(married);

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, NULL, "close prep", ECPGt_EOIT, ECPGt_EORT);
#line 110 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 110 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 110 "test3.pgc"


	strcpy(msg, "drop");
	{ ECPGdo(__LINE__, 0, 1, NULL, "drop table meskes ", ECPGt_EOIT, ECPGt_EORT);
#line 113 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 113 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 113 "test3.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 116 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 116 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 116 "test3.pgc"


	strcpy(msg, "disconnect"); 
	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 119 "test3.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 119 "test3.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 119 "test3.pgc"


	return (0);
}
