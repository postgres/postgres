/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "cursor.pgc"
#include <stdlib.h>
#include <string.h>


#line 1 "regression.h"






#line 4 "cursor.pgc"


/* exec sql whenever sqlerror  stop ; */
#line 6 "cursor.pgc"


/* exec sql type c is char reference */
#line 8 "cursor.pgc"

typedef char* c;

/* exec sql type ind is union { 
#line 11 "cursor.pgc"
 int integer ;
 
#line 11 "cursor.pgc"
 short smallint ;
 } */
#line 11 "cursor.pgc"

typedef union { int integer; short smallint; } ind;

#define BUFFERSIZ 8
/* exec sql type str is [ BUFFERSIZ ] */
#line 15 "cursor.pgc"


#define CURNAME "mycur"

int
main (void)
{
/* exec sql begin declare section */
		  
		  
		  
		  
		
		  
		
		
		

#line 23 "cursor.pgc"
 char * stmt1 = "SELECT id, t FROM t1" ;
 
#line 24 "cursor.pgc"
 char * curname1 = CURNAME ;
 
#line 25 "cursor.pgc"
 char * curname2 = CURNAME ;
 
#line 26 "cursor.pgc"
 char * curname3 = CURNAME ;
 
#line 27 "cursor.pgc"
  struct varchar_1  { int len; char arr[ 50 ]; }  curname4 ;
 
#line 28 "cursor.pgc"
 char * curname5 = CURNAME ;
 
#line 29 "cursor.pgc"
 int count ;
 
#line 30 "cursor.pgc"
 int id ;
 
#line 31 "cursor.pgc"
 char t [ 64 ] ;
/* exec sql end declare section */
#line 32 "cursor.pgc"


	char msg[128];

	ECPGdebug(1, stderr);

	strcpy(msg, "connect");
	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , "test1", 0); 
#line 39 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 39 "cursor.pgc"

	{ ECPGconnect(__LINE__, 0, "connectdb" , NULL, NULL , "test2", 0); 
#line 40 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 40 "cursor.pgc"


	strcpy(msg, "set");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "set datestyle to iso", ECPGt_EOIT, ECPGt_EORT);
#line 43 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 43 "cursor.pgc"


	strcpy(msg, "create");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "create table t1 ( id serial primary key , t text )", ECPGt_EOIT, ECPGt_EORT);
#line 46 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 46 "cursor.pgc"

	{ ECPGdo(__LINE__, 0, 1, "test2", 0, ECPGst_normal, "create table t1 ( id serial primary key , t text )", ECPGt_EOIT, ECPGt_EORT);
#line 47 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 47 "cursor.pgc"


	strcpy(msg, "insert");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'a' )", ECPGt_EOIT, ECPGt_EORT);
#line 50 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 50 "cursor.pgc"

	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'b' )", ECPGt_EOIT, ECPGt_EORT);
#line 51 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 51 "cursor.pgc"

	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'c' )", ECPGt_EOIT, ECPGt_EORT);
#line 52 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 52 "cursor.pgc"

	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'd' )", ECPGt_EOIT, ECPGt_EORT);
#line 53 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 53 "cursor.pgc"

	{ ECPGdo(__LINE__, 0, 1, "test2", 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'e' )", ECPGt_EOIT, ECPGt_EORT);
#line 54 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 54 "cursor.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, "test1", "commit");
#line 57 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 57 "cursor.pgc"

	{ ECPGtrans(__LINE__, "test2", "commit");
#line 58 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 58 "cursor.pgc"


	/* Dynamic cursorname test with INTO list in FETCH stmts */

	strcpy(msg, "declare");
	ECPGset_var( 0, &( curname1 ), __LINE__);\
 /* declare $0 cursor for select id , t from t1 */
#line 64 "cursor.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "declare $0 cursor for select id , t from t1", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 67 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 67 "cursor.pgc"


	strcpy(msg, "fetch from");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch forward from $0", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 70 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 70 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch forward $0", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 74 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 74 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch 1 from");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch 1 from $0", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 78 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 78 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count from");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch $0 from $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 83 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 83 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "move in");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "move absolute 0 in $0", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 87 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 87 "cursor.pgc"


	strcpy(msg, "fetch 1");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch 1 $0", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 90 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 90 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch $0 $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 95 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 95 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "close $0", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 99 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 99 "cursor.pgc"


	/* Dynamic cursorname test with INTO list in DECLARE stmt */

	strcpy(msg, "declare");
	ECPGset_var( 1, &( curname2 ), __LINE__);\
 ECPGset_var( 2, ( t ), __LINE__);\
 ECPGset_var( 3, &( id ), __LINE__);\
 /* declare $0 cursor for select id , t from t1 */
#line 105 "cursor.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "declare $0 cursor for select id , t from t1", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 108 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 108 "cursor.pgc"


	strcpy(msg, "fetch from");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch from $0", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 111 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 111 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch $0", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 115 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 115 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch 1 from");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch 1 from $0", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 119 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 119 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count from");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch $0 from $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 124 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 124 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "move");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "move absolute 0 $0", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 128 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 128 "cursor.pgc"


	strcpy(msg, "fetch 1");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch 1 $0", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 131 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 131 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch $0 $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 136 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 136 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "close $0", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 140 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 140 "cursor.pgc"


	/* Dynamic cursorname test with PREPARED stmt */

	strcpy(msg, "prepare");
	{ ECPGprepare(__LINE__, "test1", 0, "st_id1", stmt1);
#line 145 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 145 "cursor.pgc"

	{ ECPGprepare(__LINE__, "test2", 0, "st_id1", stmt1);
#line 146 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 146 "cursor.pgc"


	strcpy(msg, "declare");
	ECPGset_var( 4, &( curname3 ), __LINE__);\
 /* declare $0 cursor for $1 */
#line 149 "cursor.pgc"

	ECPGset_var( 5, &( curname5 ), __LINE__);\
 /* declare $0 cursor for $1 */
#line 150 "cursor.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "declare $0 cursor for $1", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char_variable,(ECPGprepared_statement("test1", "st_id1", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 153 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 153 "cursor.pgc"

	{ ECPGdo(__LINE__, 0, 1, "test2", 0, ECPGst_normal, "declare $0 cursor for $1", 
	ECPGt_char,&(curname5),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char_variable,(ECPGprepared_statement("test2", "st_id1", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 154 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 154 "cursor.pgc"


	strcpy(msg, "fetch");
	{ ECPGdo(__LINE__, 0, 1, "test2", 0, ECPGst_normal, "fetch $0", 
	ECPGt_char,&(curname5),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 157 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 157 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch from");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch from $0", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 161 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 161 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch 1 from");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch 1 from $0", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 165 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 165 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count from");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch $0 from $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 170 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 170 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "move");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "move absolute 0 $0", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 174 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 174 "cursor.pgc"


	strcpy(msg, "fetch 1");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch 1 $0", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 177 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 177 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch $0 $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 182 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 182 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "close $0", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 186 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 186 "cursor.pgc"

	{ ECPGdo(__LINE__, 0, 1, "test2", 0, ECPGst_normal, "close $0", 
	ECPGt_char,&(curname5),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 187 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 187 "cursor.pgc"


	strcpy(msg, "deallocate prepare");
	{ ECPGdeallocate(__LINE__, 0, "test1", "st_id1");
#line 190 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 190 "cursor.pgc"

	{ ECPGdeallocate(__LINE__, 0, "test2", "st_id1");
#line 191 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 191 "cursor.pgc"


	/* Dynamic cursorname test with PREPARED stmt,
	   cursor name in varchar */

	curname4.len = strlen(CURNAME);
	strcpy(curname4.arr, CURNAME);

	strcpy(msg, "prepare");
	{ ECPGprepare(__LINE__, "test1", 0, "st_id2", stmt1);
#line 200 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 200 "cursor.pgc"


	strcpy(msg, "declare");
	ECPGset_var( 6, &( curname4 ), __LINE__);\
 /* declare $0 cursor for $1 */
#line 203 "cursor.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "declare $0 cursor for $1", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char_variable,(ECPGprepared_statement("test1", "st_id2", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 206 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 206 "cursor.pgc"


	strcpy(msg, "fetch from");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch from $0", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 209 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 209 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch $0", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 213 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 213 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch 1 from");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch 1 from $0", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 217 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 217 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count from");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch $0 from $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 222 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 222 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "move");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "move absolute 0 $0", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 226 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 226 "cursor.pgc"


	strcpy(msg, "fetch 1");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch 1 $0", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 229 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 229 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "fetch $0 $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 234 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 234 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "close $0", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 238 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 238 "cursor.pgc"


	strcpy(msg, "deallocate prepare");
	{ ECPGdeallocate(__LINE__, 0, "test1", "st_id2");
#line 241 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 241 "cursor.pgc"


	/* End test */

	strcpy(msg, "drop");
	{ ECPGdo(__LINE__, 0, 1, "test1", 0, ECPGst_normal, "drop table t1", ECPGt_EOIT, ECPGt_EORT);
#line 246 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 246 "cursor.pgc"

	{ ECPGdo(__LINE__, 0, 1, "test2", 0, ECPGst_normal, "drop table t1", ECPGt_EOIT, ECPGt_EORT);
#line 247 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 247 "cursor.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, "test1", "commit");
#line 250 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 250 "cursor.pgc"


	strcpy(msg, "disconnect");
	{ ECPGdisconnect(__LINE__, "ALL");
#line 253 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 253 "cursor.pgc"


	return (0);
}
