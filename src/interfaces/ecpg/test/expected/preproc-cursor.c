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
  struct varchar_curname4_1  { int len; char arr[ 50 ]; }  curname4 ;
 
#line 28 "cursor.pgc"
 int count ;
 
#line 29 "cursor.pgc"
 int id ;
 
#line 30 "cursor.pgc"
 char t [ 64 ] ;
/* exec sql end declare section */
#line 31 "cursor.pgc"


	char msg[128];

	ECPGdebug(1, stderr);

	strcpy(msg, "connect");
	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); 
#line 38 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 38 "cursor.pgc"


	strcpy(msg, "set");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set datestyle to iso", ECPGt_EOIT, ECPGt_EORT);
#line 41 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 41 "cursor.pgc"


	strcpy(msg, "create");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table t1 ( id serial primary key , t text )", ECPGt_EOIT, ECPGt_EORT);
#line 44 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 44 "cursor.pgc"


	strcpy(msg, "insert");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'a' )", ECPGt_EOIT, ECPGt_EORT);
#line 47 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 47 "cursor.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'b' )", ECPGt_EOIT, ECPGt_EORT);
#line 48 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 48 "cursor.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'c' )", ECPGt_EOIT, ECPGt_EORT);
#line 49 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 49 "cursor.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'd' )", ECPGt_EOIT, ECPGt_EORT);
#line 50 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 50 "cursor.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 53 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 53 "cursor.pgc"


	/* Dynamic cursorname test with INTO list in FETCH stmts */

	strcpy(msg, "declare");
	ECPGset_var( 0, &( curname1 ), __LINE__);\
 /* declare $0 cursor for select id , t from t1 */
#line 59 "cursor.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare $0 cursor for select id , t from t1", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 62 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 62 "cursor.pgc"


	strcpy(msg, "fetch from");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch forward from $0", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 65 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 65 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch forward $0", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 69 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 69 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch 1 from");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch 1 from $0", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 73 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 73 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count from");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch $0 from $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
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

	strcpy(msg, "move in");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "move absolute 0 in $0", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 82 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 82 "cursor.pgc"


	strcpy(msg, "fetch 1");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch 1 $0", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 85 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 85 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch $0 $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
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

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close $0", 
	ECPGt_char,&(curname1),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 94 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 94 "cursor.pgc"


	/* Dynamic cursorname test with INTO list in DECLARE stmt */

	strcpy(msg, "declare");
	ECPGset_var( 1, &( curname2 ), __LINE__);\
 ECPGset_var( 2, ( t ), __LINE__);\
 ECPGset_var( 3, &( id ), __LINE__);\
 /* declare $0 cursor for select id , t from t1 */
#line 100 "cursor.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare $0 cursor for select id , t from t1", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 103 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 103 "cursor.pgc"


	strcpy(msg, "fetch from");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch from $0", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 106 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 106 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch $0", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 110 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 110 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch 1 from");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch 1 from $0", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 114 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 114 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count from");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch $0 from $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
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

	strcpy(msg, "move");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "move absolute 0 $0", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 123 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 123 "cursor.pgc"


	strcpy(msg, "fetch 1");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch 1 $0", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 126 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 126 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch $0 $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
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

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close $0", 
	ECPGt_char,&(curname2),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 135 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 135 "cursor.pgc"


	/* Dynamic cursorname test with PREPARED stmt */

	strcpy(msg, "prepare");
	{ ECPGprepare(__LINE__, NULL, 0, "st_id1", stmt1);
#line 140 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 140 "cursor.pgc"


	strcpy(msg, "declare");
	ECPGset_var( 4, &( curname3 ), __LINE__);\
 /* declare $0 cursor for $1 */
#line 143 "cursor.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare $0 cursor for $1", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "st_id1", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 146 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 146 "cursor.pgc"


	strcpy(msg, "fetch from");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch from $0", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 149 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 149 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch $0", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 153 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 153 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch 1 from");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch 1 from $0", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 157 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 157 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count from");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch $0 from $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 162 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 162 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "move");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "move absolute 0 $0", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 166 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 166 "cursor.pgc"


	strcpy(msg, "fetch 1");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch 1 $0", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 169 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 169 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch $0 $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 174 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 174 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close $0", 
	ECPGt_char,&(curname3),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 178 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 178 "cursor.pgc"


	strcpy(msg, "deallocate prepare");
	{ ECPGdeallocate(__LINE__, 0, NULL, "st_id1");
#line 181 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 181 "cursor.pgc"


	/* Dynamic cursorname test with PREPARED stmt,
	   cursor name in varchar */

	curname4.len = strlen(CURNAME);
	strcpy(curname4.arr, CURNAME);

	strcpy(msg, "prepare");
	{ ECPGprepare(__LINE__, NULL, 0, "st_id2", stmt1);
#line 190 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 190 "cursor.pgc"


	strcpy(msg, "declare");
	ECPGset_var( 5, &( curname4 ), __LINE__);\
 /* declare $0 cursor for $1 */
#line 193 "cursor.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare $0 cursor for $1", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_curname4_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "st_id2", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 196 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 196 "cursor.pgc"


	strcpy(msg, "fetch from");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch from $0", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_curname4_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 199 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 199 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch $0", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_curname4_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 203 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 203 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch 1 from");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch 1 from $0", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_curname4_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 207 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 207 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count from");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch $0 from $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_curname4_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 212 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 212 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "move");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "move absolute 0 $0", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_curname4_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 216 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 216 "cursor.pgc"


	strcpy(msg, "fetch 1");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch 1 $0", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_curname4_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 219 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 219 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "fetch :count");
	count = 1;
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch $0 $0", 
	ECPGt_int,&(count),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_curname4_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_int,&(id),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(t),(long)64,(long)1,(64)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 224 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 224 "cursor.pgc"

	printf("%d %s\n", id, t);

	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close $0", 
	ECPGt_varchar,&(curname4),(long)50,(long)1,sizeof(struct varchar_curname4_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 228 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 228 "cursor.pgc"


	strcpy(msg, "deallocate prepare");
	{ ECPGdeallocate(__LINE__, 0, NULL, "st_id2");
#line 231 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 231 "cursor.pgc"


	/* End test */

	strcpy(msg, "drop");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table t1", ECPGt_EOIT, ECPGt_EORT);
#line 236 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 236 "cursor.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 239 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 239 "cursor.pgc"


	strcpy(msg, "disconnect"); 
	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 242 "cursor.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 242 "cursor.pgc"


	return (0);
}
