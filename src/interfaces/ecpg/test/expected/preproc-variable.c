/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "variable.pgc"
#include <stdlib.h>
#include <string.h>


#line 1 "regression.h"






#line 4 "variable.pgc"


/* exec sql whenever sqlerror  stop ; */
#line 6 "variable.pgc"


/* exec sql type c is char reference */
#line 8 "variable.pgc"

typedef char* c;

/* exec sql type ind is union { 
#line 11 "variable.pgc"
 int integer ;
 
#line 11 "variable.pgc"
 short smallint ;
 } */
#line 11 "variable.pgc"

typedef union { int integer; short smallint; } ind;

#define BUFFERSIZ 8
/* exec sql type str is [ BUFFERSIZ ] */
#line 15 "variable.pgc"


/* declare cur cursor for select name , born , age , married , children from family */
#line 18 "variable.pgc"


int
main (void)
{
	struct birthinfo { 
#line 23 "variable.pgc"
 long born ;
 
#line 23 "variable.pgc"
 short age ;
 } ;
#line 23 "variable.pgc"

/* exec sql begin declare section */
	 		 
					  
				  
	  	 
					  
				    
	 
	           
	    
	    

#line 27 "variable.pgc"
 struct personal_struct { 
#line 25 "variable.pgc"
  struct varchar_1  { int len; char arr[ BUFFERSIZ ]; }  name ;
 
#line 26 "variable.pgc"
 struct birthinfo birth ;
 } personal , * p ;
 
#line 30 "variable.pgc"
 struct personal_indicator { 
#line 28 "variable.pgc"
 int ind_name ;
 
#line 29 "variable.pgc"
 struct birthinfo ind_birth ;
 } ind_personal , * i ;
 
#line 31 "variable.pgc"
 ind ind_children ;
 struct t1 { 
#line 32 "variable.pgc"
  struct varchar_2  { int len; char arr[ BUFFERSIZ ]; }  name ;
 } ; struct t2 { 
#line 32 "variable.pgc"
  struct varchar_3  { int len; char arr[ BUFFERSIZ ]; }  name ;
 } ; 
#line 33 "variable.pgc"
 static  struct varchar_4  { int len; char arr[ 50 ]; }  vc1 ; static  struct varchar_5  { int len; char arr[ 50 ]; }  vc2 ; static  struct varchar_6  { int len; char arr[ 255 ]; }  vc3 ;
 
#line 34 "variable.pgc"
 static int i1 , i2 , i3 ;
/* exec sql end declare section */
#line 35 "variable.pgc"


	
#line 37 "variable.pgc"
 char * married = NULL ;

#line 37 "variable.pgc"

	
#line 38 "variable.pgc"
 long ind_married ;

#line 38 "variable.pgc"

	
#line 39 "variable.pgc"
 ind children ;

#line 39 "variable.pgc"

	int loopcount;
	char msg[128];

        ECPGdebug(1, stderr);

	strcpy(msg, "connect");
	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 46 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 46 "variable.pgc"


	strcpy(msg, "set");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set datestyle to iso", ECPGt_EOIT, ECPGt_EORT);
#line 49 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 49 "variable.pgc"


	strcpy(msg, "create");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table family ( name char ( 8 ) , born integer , age smallint , married date , children integer )", ECPGt_EOIT, ECPGt_EORT);
#line 52 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 52 "variable.pgc"


	strcpy(msg, "insert");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into family ( name , married , children ) values ( 'Mum' , '19870714' , 3 )", ECPGt_EOIT, ECPGt_EORT);
#line 55 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 55 "variable.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into family ( name , born , married , children ) values ( 'Dad' , '19610721' , '19870714' , 3 )", ECPGt_EOIT, ECPGt_EORT);
#line 56 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 56 "variable.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into family ( name , age ) values ( 'Child 1' , 16 )", ECPGt_EOIT, ECPGt_EORT);
#line 57 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 57 "variable.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into family ( name , age ) values ( 'Child 2' , 14 )", ECPGt_EOIT, ECPGt_EORT);
#line 58 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 58 "variable.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into family ( name , age ) values ( 'Child 3' , 9 )", ECPGt_EOIT, ECPGt_EORT);
#line 59 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 59 "variable.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 62 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 62 "variable.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cur cursor for select name , born , age , married , children from family", ECPGt_EOIT, ECPGt_EORT);
#line 65 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 65 "variable.pgc"


	/* exec sql whenever not found  break ; */
#line 67 "variable.pgc"


	p=&personal;
	i=&ind_personal;
	memset(i, 0, sizeof(ind_personal));
	for (loopcount = 0; loopcount < 100; loopcount++) {
		strcpy(msg, "fetch");
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cur", ECPGt_EOIT, 
	ECPGt_varchar,&(p->name),(long)BUFFERSIZ,(long)-1,sizeof( struct birthinfo ), 
	ECPGt_int,&(i->ind_name),(long)1,(long)-1,sizeof( struct birthinfo ), 
	ECPGt_long,&(p->birth.born),(long)1,(long)1,sizeof( struct birthinfo ), 
	ECPGt_long,&(i->ind_birth.born),(long)1,(long)1,sizeof( struct birthinfo ), 
	ECPGt_short,&(p->birth.age),(long)1,(long)1,sizeof( struct birthinfo ), 
	ECPGt_short,&(i->ind_birth.age),(long)1,(long)1,sizeof( struct birthinfo ), 
	ECPGt_char,&(married),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_long,&(ind_married),(long)1,(long)1,sizeof(long), 
	ECPGt_int,&(children.integer),(long)1,(long)1,sizeof(int), 
	ECPGt_short,&(ind_children.smallint),(long)1,(long)1,sizeof(short), ECPGt_EORT);
#line 74 "variable.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 74 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 74 "variable.pgc"

		printf("%8.8s", personal.name.arr);
		if (i->ind_birth.born >= 0)
			printf(", born %ld", personal.birth.born);
		if (i->ind_birth.age >= 0)
			printf(", age = %d", personal.birth.age);
		if (ind_married >= 0)
			printf(", married %s", married);
		if (ind_children.smallint >= 0)
			printf(", children = %d", children.integer);
		putchar('\n');

		free(married);
		married = NULL;
	}

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cur", ECPGt_EOIT, ECPGt_EORT);
#line 91 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 91 "variable.pgc"


	strcpy(msg, "drop");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table family", ECPGt_EOIT, ECPGt_EORT);
#line 94 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 94 "variable.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 97 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 97 "variable.pgc"


	strcpy(msg, "disconnect");
	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 100 "variable.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 100 "variable.pgc"


	/* this just to silence unused-variable warnings: */
	vc1.len = vc2.len = vc3.len = 0;
	i1 = i2 = i3 = 0;
	printf("%d %d %d %d %d %d\n",
	       vc1.len, vc2.len, vc3.len,
	       i1, i2, i3);

	return 0;
}
