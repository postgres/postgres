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

	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , "main", 0); 
#line 24 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 24 "oldexec.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "create table test ( name char ( 8 ) , amount int , letter char ( 1 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 26 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "oldexec.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 27 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 27 "oldexec.pgc"


	sprintf(command, "insert into test (name, amount, letter) values ('db: ''r1''', 1, 'f')");
	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_exec_immediate, command, ECPGt_EOIT, ECPGt_EORT);
#line 30 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 30 "oldexec.pgc"


	sprintf(command, "insert into test (name, amount, letter) values ('db: ''r1''', 2, 't')");
	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_exec_immediate, command, ECPGt_EOIT, ECPGt_EORT);
#line 33 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 33 "oldexec.pgc"


	sprintf(command, "insert into test (name, amount, letter) select name, amount+10, letter from test");
	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_exec_immediate, command, ECPGt_EOIT, ECPGt_EORT);
#line 36 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "oldexec.pgc"


	printf("Inserted %ld tuples via execute immediate\n", sqlca.sqlerrd[2]);

	sprintf(command, "insert into test (name, amount, letter) select name, amount+$1, letter from test");
	{ ECPGprepare(__LINE__, NULL, 1, "i", command);
#line 41 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 41 "oldexec.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_execute, "i", 
	ECPGt_int,&(increment),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 42 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 42 "oldexec.pgc"


	printf("Inserted %ld tuples via prepared execute\n", sqlca.sqlerrd[2]);

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 46 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 46 "oldexec.pgc"


	sprintf (command, "select * from test");

	{ ECPGprepare(__LINE__, NULL, 1, "f", command);
#line 50 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 50 "oldexec.pgc"

	/* declare CUR cursor for $1 */
#line 51 "oldexec.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "declare CUR cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "f", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 53 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 53 "oldexec.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "fetch 8 in CUR", ECPGt_EOIT, 
	ECPGt_char,(name),(long)8,(long)8,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(amount),(long)1,(long)8,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(letter),(long)1,(long)8,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 54 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 54 "oldexec.pgc"


	for (i=0, j=sqlca.sqlerrd[2]; i<j; i++)
	{
		char n[8], l = letter[i][0];
		int a = amount[i];

		strncpy(n, name[i], 8);
		printf("name[%d]=%8.8s\tamount[%d]=%d\tletter[%d]=%c\n", i, n, i, a, i, l);
	}

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "close CUR", ECPGt_EOIT, ECPGt_EORT);
#line 65 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 65 "oldexec.pgc"


	sprintf (command, "select * from test where ? = amount");

	{ ECPGprepare(__LINE__, NULL, 1, "f", command);
#line 69 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 69 "oldexec.pgc"

	/* declare CUR3 cursor for $1 */
#line 70 "oldexec.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "declare CUR3 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "f", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 72 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 72 "oldexec.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "fetch in CUR3", ECPGt_EOIT, 
	ECPGt_char,(name),(long)8,(long)8,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(amount),(long)1,(long)8,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(letter),(long)1,(long)8,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 73 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 73 "oldexec.pgc"


	for (i=0, j=sqlca.sqlerrd[2]; i<j; i++)
	{
		char n[8], l = letter[i][0];
		int a = amount[i];

		strncpy(n, name[i], 8);
		printf("name[%d]=%8.8s\tamount[%d]=%d\tletter[%d]=%c\n", i, n, i, a, i, l);
	}

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "close CUR3", ECPGt_EOIT, ECPGt_EORT);
#line 84 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 84 "oldexec.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 1, ECPGst_normal, "drop table test", ECPGt_EOIT, ECPGt_EORT);
#line 85 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 85 "oldexec.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 86 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 86 "oldexec.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 87 "oldexec.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 87 "oldexec.pgc"


	return (0);
}
