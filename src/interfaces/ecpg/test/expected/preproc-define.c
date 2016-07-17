/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "define.pgc"
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


#line 1 "regression.h"






#line 6 "define.pgc"


/* exec sql whenever sqlerror  sqlprint ; */
#line 8 "define.pgc"





/* exec sql type intarray is int [ 6 ] */
#line 13 "define.pgc"

typedef int intarray[ 6];

int
main(void)
{
/* exec sql begin declare section */

	   typedef char  string [ 8 ];

#line 21 "define.pgc"

	 
	   
	  	 

#line 22 "define.pgc"
 intarray amount ;
 
#line 23 "define.pgc"
 char name [ 6 ] [ 8 ] ;
 
#line 24 "define.pgc"
 char letter [ 6 ] [ 1 ] ;
 
#if 0
 
#line 26 "define.pgc"
 int not_used ;
 
#endif
/* exec sql end declare section */
#line 29 "define.pgc"

	int i,j;

	ECPGdebug(1, stderr);

	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 34 "define.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 34 "define.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table test ( name char ( 8 ) , amount int , letter char ( 1 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 36 "define.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "define.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 37 "define.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 37 "define.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into Test ( name , amount , letter ) values ( 'false' , 1 , 'f' )", ECPGt_EOIT, ECPGt_EORT);
#line 39 "define.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 39 "define.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test ( name , amount , letter ) values ( 'true' , 2 , 't' )", ECPGt_EOIT, ECPGt_EORT);
#line 40 "define.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 40 "define.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 41 "define.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 41 "define.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select * from test", ECPGt_EOIT, 
	ECPGt_char,(name),(long)8,(long)6,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(amount),(long)1,(long)6,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(letter),(long)1,(long)6,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 43 "define.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 43 "define.pgc"


	for (i=0, j=sqlca.sqlerrd[2]; i<j; i++)
	{
		/* exec sql begin declare section */
		 
		   
		   
		
#line 48 "define.pgc"
 string n ;
 
#line 49 "define.pgc"
 char l = letter [ i ] [ 0 ] ;
 
#line 50 "define.pgc"
 int a = amount [ i ] ;
/* exec sql end declare section */
#line 51 "define.pgc"


		strncpy(n, name[i],  8);
		printf("name[%d]=%8.8s\tamount[%d]=%d\tletter[%d]=%c\n", i, n, i, a, i, l);
	}

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table test", ECPGt_EOIT, ECPGt_EORT);
#line 57 "define.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 57 "define.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 58 "define.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 58 "define.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 59 "define.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 59 "define.pgc"


	return (0);
}
