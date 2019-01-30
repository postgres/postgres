/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "whenever_do_continue.pgc"
#include <stdlib.h>


#line 1 "regression.h"






#line 3 "whenever_do_continue.pgc"


/* exec sql whenever sqlerror  stop ; */
#line 5 "whenever_do_continue.pgc"


int main(void)
{
	/* exec sql begin declare section */
	
	
		 
		 
		 
	 
	 
	 
	
#line 15 "whenever_do_continue.pgc"
 struct { 
#line 12 "whenever_do_continue.pgc"
 char ename [ 12 ] ;
 
#line 13 "whenever_do_continue.pgc"
 float sal ;
 
#line 14 "whenever_do_continue.pgc"
 float comm ;
 } emp ;
 
#line 16 "whenever_do_continue.pgc"
 int loopcount ;
 
#line 17 "whenever_do_continue.pgc"
 char msg [ 128 ] ;
/* exec sql end declare section */
#line 18 "whenever_do_continue.pgc"


	ECPGdebug(1, stderr);

	strcpy(msg, "connect");
	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 23 "whenever_do_continue.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 23 "whenever_do_continue.pgc"


	strcpy(msg, "create");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table emp ( ename varchar , sal double precision , comm double precision )", ECPGt_EOIT, ECPGt_EORT);
#line 26 "whenever_do_continue.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 26 "whenever_do_continue.pgc"


	strcpy(msg, "insert");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into emp values ( 'Ram' , 111100 , 21 )", ECPGt_EOIT, ECPGt_EORT);
#line 29 "whenever_do_continue.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 29 "whenever_do_continue.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into emp values ( 'aryan' , 11110 , null )", ECPGt_EOIT, ECPGt_EORT);
#line 30 "whenever_do_continue.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 30 "whenever_do_continue.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into emp values ( 'josh' , 10000 , 10 )", ECPGt_EOIT, ECPGt_EORT);
#line 31 "whenever_do_continue.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 31 "whenever_do_continue.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into emp values ( 'tom' , 20000 , null )", ECPGt_EOIT, ECPGt_EORT);
#line 32 "whenever_do_continue.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 32 "whenever_do_continue.pgc"


	/* declare c cursor for select ename , sal , comm from emp order by ename collate \"C\" asc */
#line 34 "whenever_do_continue.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare c cursor for select ename , sal , comm from emp order by ename collate \"C\" asc", ECPGt_EOIT, ECPGt_EORT);
#line 36 "whenever_do_continue.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 36 "whenever_do_continue.pgc"


	/* The 'BREAK' condition to exit the loop. */
	/* exec sql whenever not found  break ; */
#line 39 "whenever_do_continue.pgc"


	/* The DO CONTINUE makes the loop start at the next iteration when an error occurs.*/
	/* exec sql whenever sqlerror  continue ; */
#line 42 "whenever_do_continue.pgc"


	for (loopcount = 0; loopcount < 100; loopcount++)
	{
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch c", ECPGt_EOIT, 
	ECPGt_char,&(emp.ename),(long)12,(long)1,(12)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_float,&(emp.sal),(long)1,(long)1,sizeof(float), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_float,&(emp.comm),(long)1,(long)1,sizeof(float), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 46 "whenever_do_continue.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 46 "whenever_do_continue.pgc"

if (sqlca.sqlcode < 0) continue;}
#line 46 "whenever_do_continue.pgc"

		/* The employees with non-NULL commissions will be displayed. */
		printf("%s %7.2f %9.2f\n", emp.ename, emp.sal, emp.comm);
	}

	/*
	 * This 'CONTINUE' shuts off the 'DO CONTINUE' and allow the program to
	 * proceed if any further errors do occur.
	 */
	/* exec sql whenever sqlerror  continue ; */
#line 55 "whenever_do_continue.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close c", ECPGt_EOIT, ECPGt_EORT);}
#line 57 "whenever_do_continue.pgc"


	strcpy(msg, "drop");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table emp", ECPGt_EOIT, ECPGt_EORT);}
#line 60 "whenever_do_continue.pgc"


	exit(0);
}
