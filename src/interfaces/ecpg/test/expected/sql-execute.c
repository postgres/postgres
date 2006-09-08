/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "execute.pgc"
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


#line 1 "regression.h"






#line 6 "execute.pgc"


/* exec sql whenever sqlerror  sqlprint ; */
#line 8 "execute.pgc"


int
main(void)
{
/* exec sql begin declare section */
	 
	 
	 
	 
	 

#line 14 "execute.pgc"
 int  amount [ 8 ]    ;
 
#line 15 "execute.pgc"
 int  increment   = 100 ;
 
#line 16 "execute.pgc"
 char  name [ 8 ] [ 8 ]    ;
 
#line 17 "execute.pgc"
 char  letter [ 8 ] [ 1 ]    ;
 
#line 18 "execute.pgc"
 char  command [ 128 ]    ;
/* exec sql end declare section */
#line 19 "execute.pgc"

	int i,j;

	ECPGdebug(1, stderr);

	{ ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , "main", 0); 
#line 24 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 24 "execute.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "create  table test ( name char  ( 8 )    , amount int   , letter char  ( 1 )    )    ", ECPGt_EOIT, ECPGt_EORT);
#line 25 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 25 "execute.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 26 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "execute.pgc"


	sprintf(command, "insert into test (name, amount, letter) values ('db: ''r1''', 1, 'f')");
	{ ECPGdo(__LINE__, 0, 1, NULL, "?", 
	ECPGt_char_variable,(command),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 29 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 29 "execute.pgc"


	sprintf(command, "insert into test (name, amount, letter) values ('db: ''r1''', 2, 't')");
	{ ECPGdo(__LINE__, 0, 1, NULL, "?", 
	ECPGt_char_variable,(command),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 32 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "execute.pgc"


	sprintf(command, "insert into test (name, amount, letter) select name, amount+10, letter from test");
	{ ECPGdo(__LINE__, 0, 1, NULL, "?", 
	ECPGt_char_variable,(command),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 35 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 35 "execute.pgc"


	printf("Inserted %ld tuples via execute immediate\n", sqlca.sqlerrd[2]);

	sprintf(command, "insert into test (name, amount, letter) select name, amount+?, letter from test");
	{ ECPGprepare(__LINE__, "I" , command);
#line 40 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 40 "execute.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "?", 
	ECPGt_char_variable,(ECPGprepared_statement("I")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(increment),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 41 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 41 "execute.pgc"


	printf("Inserted %ld tuples via prepared execute\n", sqlca.sqlerrd[2]);

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 45 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 45 "execute.pgc"


	sprintf (command, "select * from test");

	{ ECPGprepare(__LINE__, "F" , command);
#line 49 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 49 "execute.pgc"

	/* declare CUR  cursor  for ? */
#line 50 "execute.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, "declare CUR  cursor  for ?", 
	ECPGt_char_variable,(ECPGprepared_statement("F")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 52 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 52 "execute.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "fetch 8 in CUR", ECPGt_EOIT, 
	ECPGt_char,(name),(long)8,(long)8,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(amount),(long)1,(long)8,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(letter),(long)1,(long)8,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 53 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 53 "execute.pgc"


	for (i=0, j=sqlca.sqlerrd[2]; i<j; i++)
	{
		/* exec sql begin declare section */
		    
		   
		
#line 58 "execute.pgc"
 char  n [ 8 ]    ,  l   = letter [ i ] [ 0 ] ;
 
#line 59 "execute.pgc"
 int  a   = amount [ i ] ;
/* exec sql end declare section */
#line 60 "execute.pgc"


		strncpy(n, name[i], 8);
		printf("name[%d]=%8.8s\tamount[%d]=%d\tletter[%d]=%c\n", i, n, i, a, i, l);
	}

	{ ECPGdo(__LINE__, 0, 1, NULL, "close CUR", ECPGt_EOIT, ECPGt_EORT);
#line 66 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 66 "execute.pgc"


	sprintf (command, "select * from test where amount = ?");

	{ ECPGprepare(__LINE__, "F" , command);
#line 70 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 70 "execute.pgc"

	/* declare CUR2  cursor  for ? */
#line 71 "execute.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, "declare CUR2  cursor  for ?", 
	ECPGt_char_variable,(ECPGprepared_statement("F")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_const,"1",(long)1,(long)1,strlen("1"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 73 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 73 "execute.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "fetch in CUR2", ECPGt_EOIT, 
	ECPGt_char,(name),(long)8,(long)8,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(amount),(long)1,(long)8,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(letter),(long)1,(long)8,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 74 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 74 "execute.pgc"


	for (i=0, j=sqlca.sqlerrd[2]; i<j; i++)
	{
		/* exec sql begin declare section */
		    
		   
		
#line 79 "execute.pgc"
 char  n [ 8 ]    ,  l   = letter [ i ] [ 0 ] ;
 
#line 80 "execute.pgc"
 int  a   = amount [ i ] ;
/* exec sql end declare section */
#line 81 "execute.pgc"


		strncpy(n, name[i], 8);
		printf("name[%d]=%8.8s\tamount[%d]=%d\tletter[%d]=%c\n", i, n, i, a, i, l);
	}

	{ ECPGdo(__LINE__, 0, 1, NULL, "close CUR2", ECPGt_EOIT, ECPGt_EORT);
#line 87 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 87 "execute.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "drop table test ", ECPGt_EOIT, ECPGt_EORT);
#line 88 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 88 "execute.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 89 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 89 "execute.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 90 "execute.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 90 "execute.pgc"


	return (0);
}
