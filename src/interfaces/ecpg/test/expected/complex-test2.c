/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "test2.pgc"
#include <stdlib.h>
#include <string.h>


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


#line 4 "test2.pgc"


#line 1 "./../regression.h"






#line 5 "test2.pgc"


/* exec sql type c is char  reference */
#line 7 "test2.pgc"

typedef char* c;

/* exec sql type ind is union { 
#line 10 "test2.pgc"
 int  integer    ;
 
#line 10 "test2.pgc"
 short  smallint    ;
 }   */
#line 10 "test2.pgc"

typedef union { int integer; short smallint; } ind;

#define BUFFERSIZ 8
/* exec sql type str is  [ BUFFERSIZ ]   */
#line 14 "test2.pgc"


/* declare cur  cursor  for select  name , born , age , married , children  from meskes    */
#line 17 "test2.pgc"


int
main (void)
{
	struct birthinfo { 
#line 22 "test2.pgc"
 long  born    ;
 
#line 22 "test2.pgc"
 short  age    ;
 } ;
#line 22 "test2.pgc"

/* exec sql begin declare section */
	 		 
					  
				  
	  	 
					  
				    
	 
	 

#line 26 "test2.pgc"
 struct personal_struct { 
#line 24 "test2.pgc"
   struct varchar_name  { int len; char arr[ BUFFERSIZ ]; }  name    ;
 
#line 25 "test2.pgc"
 struct birthinfo  birth    ;
 }  personal    , * p    ;
 
#line 29 "test2.pgc"
 struct personal_indicator { 
#line 27 "test2.pgc"
 int  ind_name    ;
 
#line 28 "test2.pgc"
 struct birthinfo  ind_birth    ;
 }  ind_personal    , * i    ;
 
#line 30 "test2.pgc"
 ind  ind_children    ;
 
#line 31 "test2.pgc"
 char * query   = "select name, born, age, married, children from meskes where name = :var1" ;
/* exec sql end declare section */
#line 32 "test2.pgc"


	
#line 34 "test2.pgc"
 char * married   = NULL ;

#line 34 "test2.pgc"

	
#line 35 "test2.pgc"
 float  ind_married    ;

#line 35 "test2.pgc"

	
#line 36 "test2.pgc"
 ind  children    ;

#line 36 "test2.pgc"


	/* exec sql var ind_married is long   */
#line 38 "test2.pgc"


	char msg[128];

        ECPGdebug(1, stderr);

	strcpy(msg, "connect");
	{ ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); 
#line 45 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 45 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 45 "test2.pgc"


	strcpy(msg, "create");
	{ ECPGdo(__LINE__, 0, 1, NULL, "create  table meskes ( name char  ( 8 )    , born integer   , age smallint   , married date   , children integer   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 48 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 48 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 48 "test2.pgc"


	strcpy(msg, "insert");
	{ ECPGdo(__LINE__, 0, 1, NULL, "insert into meskes ( name  , married  , children  ) values( 'Petra' , '19900404' , 3 )", ECPGt_EOIT, ECPGt_EORT);
#line 51 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 51 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 51 "test2.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "insert into meskes ( name  , born  , age  , married  , children  ) values( 'Michael' , 19660117 , 35 , '19900404' , 3 )", ECPGt_EOIT, ECPGt_EORT);
#line 52 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 52 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 52 "test2.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "insert into meskes ( name  , born  , age  ) values( 'Carsten' , 19910103 , 10 )", ECPGt_EOIT, ECPGt_EORT);
#line 53 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 53 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 53 "test2.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "insert into meskes ( name  , born  , age  ) values( 'Marc' , 19930907 , 8 )", ECPGt_EOIT, ECPGt_EORT);
#line 54 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 54 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 54 "test2.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "insert into meskes ( name  , born  , age  ) values( 'Chris' , 19970923 , 4 )", ECPGt_EOIT, ECPGt_EORT);
#line 55 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 55 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 55 "test2.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 58 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 58 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 58 "test2.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, NULL, "declare cur  cursor  for select  name , born , age , married , children  from meskes   ", ECPGt_EOIT, ECPGt_EORT);
#line 61 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 61 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 61 "test2.pgc"


	/* exec sql whenever not found  break ; */
#line 63 "test2.pgc"


	p=&personal;
	i=&ind_personal;
	memset(i, 0, sizeof(ind_personal));
	while (1) {
		strcpy(msg, "fetch");
		{ ECPGdo(__LINE__, 0, 1, NULL, "fetch cur", ECPGt_EOIT, 
	ECPGt_varchar,&(p->name),(long)BUFFERSIZ,(long)1,sizeof(struct varchar_name), 
	ECPGt_int,&(i->ind_name),(long)1,(long)1,sizeof(int), 
	ECPGt_long,&(p->birth.born),(long)1,(long)1,sizeof(long), 
	ECPGt_long,&(i->ind_birth.born),(long)1,(long)1,sizeof(long), 
	ECPGt_short,&(p->birth.age),(long)1,(long)1,sizeof(short), 
	ECPGt_short,&(i->ind_birth.age),(long)1,(long)1,sizeof(short), 
	ECPGt_char,&(married),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_long,&(ind_married),(long)1,(long)1,sizeof(long), 
	ECPGt_int,&(children.integer),(long)1,(long)1,sizeof(int), 
	ECPGt_short,&(ind_children.smallint),(long)1,(long)1,sizeof(short), ECPGt_EORT);
#line 70 "test2.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 70 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 70 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 70 "test2.pgc"

		printf("%8.8s", personal.name.arr);
		if (i->ind_birth.born >= 0)
			printf(", born %ld", personal.birth.born);
		if (i->ind_birth.age >= 0)
			printf(", age = %d", personal.birth.age);
		if ((long)ind_married >= 0)
			printf(", married %s", married);
		if (ind_children.smallint >= 0)
			printf(", children = %d", children.integer);
		putchar('\n');

		free(married);
		married = NULL;
	}

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, NULL, "close cur", ECPGt_EOIT, ECPGt_EORT);
#line 87 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 87 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 87 "test2.pgc"


	/* and now a same query with prepare */
	{ ECPGprepare(__LINE__, "MM" , query);
#line 90 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 90 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 90 "test2.pgc"

	/* declare prep  cursor  for ? */
#line 91 "test2.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, NULL, "declare prep  cursor  for ?", 
	ECPGt_char_variable,(ECPGprepared_statement("MM")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"'Petra'",(long)7,(long)1,strlen("'Petra'"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 94 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 94 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 94 "test2.pgc"


	/* exec sql whenever not found  break ; */
#line 96 "test2.pgc"


	while (1) {
		strcpy(msg, "fetch");
		{ ECPGdo(__LINE__, 0, 1, NULL, "fetch in prep", ECPGt_EOIT, 
	ECPGt_varchar,&(personal.name),(long)BUFFERSIZ,(long)1,sizeof(struct varchar_name), 
	ECPGt_int,&(ind_personal.ind_name),(long)1,(long)1,sizeof(int), 
	ECPGt_long,&(personal.birth.born),(long)1,(long)1,sizeof(long), 
	ECPGt_long,&(ind_personal.ind_birth.born),(long)1,(long)1,sizeof(long), 
	ECPGt_short,&(personal.birth.age),(long)1,(long)1,sizeof(short), 
	ECPGt_short,&(ind_personal.ind_birth.age),(long)1,(long)1,sizeof(short), 
	ECPGt_char,&(married),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_long,&(ind_married),(long)1,(long)1,sizeof(long), 
	ECPGt_int,&(children.integer),(long)1,(long)1,sizeof(int), 
	ECPGt_short,&(ind_children.smallint),(long)1,(long)1,sizeof(short), ECPGt_EORT);
#line 100 "test2.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 100 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 100 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 100 "test2.pgc"

		printf("%8.8s", personal.name.arr);
		if (ind_personal.ind_birth.born >= 0)
			printf(", born %ld", personal.birth.born);
		if (ind_personal.ind_birth.age >= 0)
			printf(", age = %d", personal.birth.age);
		if ((long)ind_married >= 0)
			printf(", married %s", married);
		if (ind_children.smallint >= 0)
			printf(", children = %d", children.integer);
		putchar('\n');
	}

	free(married);

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, NULL, "close prep", ECPGt_EOIT, ECPGt_EORT);
#line 116 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 116 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 116 "test2.pgc"


	strcpy(msg, "drop");
	{ ECPGdo(__LINE__, 0, 1, NULL, "drop table meskes ", ECPGt_EOIT, ECPGt_EORT);
#line 119 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 119 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 119 "test2.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 122 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 122 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 122 "test2.pgc"


	strcpy(msg, "disconnect"); 
	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 125 "test2.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 125 "test2.pgc"

if (sqlca.sqlcode < 0) Finish ( msg );}
#line 125 "test2.pgc"


	return (0);
}
