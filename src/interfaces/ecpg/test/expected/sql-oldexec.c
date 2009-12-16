/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "oldexec.pgc"
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


#line 1 "regression.h"






#line 6 "oldexec.pgc"


/* exec sql whenever sqlerror  sqlprint ; */
#line 8 "oldexec.pgc"


int
main(void)
{
/* exec sql begin declare section */
	 
	 
	 
	 
	 

#line 14 "oldexec.pgc"
 int amount [ 8 ] ;
 
#line 15 "oldexec.pgc"
 int increment = 100 ;
 
#line 16 "oldexec.pgc"
 char name [ 8 ] [ 8 ] ;
 
#line 17 "oldexec.pgc"
 char letter [ 8 ] [ 1 ] ;
 
#line 18 "oldexec.pgc"
 char command [ 128 ] ;
/* exec sql end declare section */
#line 19 "oldexec.pgc"

	int i,j;

	ECPGdebug(1, stderr);

	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , "main", 0); 
#line 24 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 24 "oldexec.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "create table test ( name char ( 8 ) , amount int , letter char ( 1 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 25 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 25 "oldexec.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 26 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "oldexec.pgc"


	sprintf(command, "insert into test (name, amount, letter) values ('db: ''r1''', 1, 'f')");
	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_exec_immediate, command, ECPGt_EOIT, ECPGt_EORT);
#line 29 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 29 "oldexec.pgc"


	sprintf(command, "insert into test (name, amount, letter) values ('db: ''r1''', 2, 't')");
	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_exec_immediate, command, ECPGt_EOIT, ECPGt_EORT);
#line 32 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "oldexec.pgc"


	sprintf(command, "insert into test (name, amount, letter) select name, amount+10, letter from test");
	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_exec_immediate, command, ECPGt_EOIT, ECPGt_EORT);
#line 35 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 35 "oldexec.pgc"


	printf("Inserted %ld tuples via execute immediate\n", sqlca.sqlerrd[2]);

	sprintf(command, "insert into test (name, amount, letter) select name, amount+$1, letter from test");
	{ ECPGprepare(__LINE__, NULL, 1, "i", command);
#line 40 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 40 "oldexec.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_execute, "i", 
	ECPGt_int,&(increment),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 41 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 41 "oldexec.pgc"


	printf("Inserted %ld tuples via prepared execute\n", sqlca.sqlerrd[2]);

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 45 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 45 "oldexec.pgc"


	sprintf (command, "select * from test");

	{ ECPGprepare(__LINE__, NULL, 1, "f", command);
#line 49 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 49 "oldexec.pgc"

	/* declare CUR cursor for $1 */
#line 50 "oldexec.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "declare CUR cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "f", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 52 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 52 "oldexec.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "fetch 8 in CUR", ECPGt_EOIT, 
	ECPGt_char,(name),(long)8,(long)8,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(amount),(long)1,(long)8,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(letter),(long)1,(long)8,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 53 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 53 "oldexec.pgc"


	for (i=0, j=sqlca.sqlerrd[2]; i<j; i++)
	{
		/* exec sql begin declare section */
		    
		   
		
#line 58 "oldexec.pgc"
 char n [ 8 ] , l = letter [ i ] [ 0 ] ;
 
#line 59 "oldexec.pgc"
 int a = amount [ i ] ;
/* exec sql end declare section */
#line 60 "oldexec.pgc"


		strncpy(n, name[i], 8);
		printf("name[%d]=%8.8s\tamount[%d]=%d\tletter[%d]=%c\n", i, n, i, a, i, l);
	}

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "close CUR", ECPGt_EOIT, ECPGt_EORT);
#line 66 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 66 "oldexec.pgc"


	sprintf (command, "select * from test where ? = amount");

	{ ECPGprepare(__LINE__, NULL, 1, "f", command);
#line 70 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 70 "oldexec.pgc"

	/* declare CUR3 cursor for $1 */
#line 71 "oldexec.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "declare CUR3 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "f", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 73 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 73 "oldexec.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "fetch in CUR3", ECPGt_EOIT, 
	ECPGt_char,(name),(long)8,(long)8,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(amount),(long)1,(long)8,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(letter),(long)1,(long)8,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 74 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 74 "oldexec.pgc"


	for (i=0, j=sqlca.sqlerrd[2]; i<j; i++)
	{
		/* exec sql begin declare section */
		    
		   
		
#line 79 "oldexec.pgc"
 char n [ 8 ] , l = letter [ i ] [ 0 ] ;
 
#line 80 "oldexec.pgc"
 int a = amount [ i ] ;
/* exec sql end declare section */
#line 81 "oldexec.pgc"


		strncpy(n, name[i], 8);
		printf("name[%d]=%8.8s\tamount[%d]=%d\tletter[%d]=%c\n", i, n, i, a, i, l);
	}

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "close CUR3", ECPGt_EOIT, ECPGt_EORT);
#line 87 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 87 "oldexec.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "drop table test", ECPGt_EOIT, ECPGt_EORT);
#line 88 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 88 "oldexec.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 89 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 89 "oldexec.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 90 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 90 "oldexec.pgc"


	return (0);
}
