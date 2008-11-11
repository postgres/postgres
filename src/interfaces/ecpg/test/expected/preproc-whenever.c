/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "whenever.pgc"
#include <stdlib.h>


#line 1 "regression.h"






#line 3 "whenever.pgc"


/* exec sql whenever sqlerror  sqlprint ; */
#line 5 "whenever.pgc"


static void print(char *msg)
{
        fprintf(stderr, "Error in statement '%s':\n", msg);
        sqlprint();
}

static void print2(void)
{
        fprintf(stderr, "Found another error\n");
        sqlprint();
}

static void warn(void)
{
        fprintf(stderr, "Warning: At least one column was truncated\n");
}

int main(void)
{
	
#line 26 "whenever.pgc"
 int  i    ;

#line 26 "whenever.pgc"

	
#line 27 "whenever.pgc"
 char  c  [ 6 ]   ;

#line 27 "whenever.pgc"


	ECPGdebug(1, stderr);

	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); 
#line 31 "whenever.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 31 "whenever.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create  table test ( i int   , c char  ( 10 )    )    ", ECPGt_EOIT, ECPGt_EORT);
#line 32 "whenever.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "whenever.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( 1 , 'abcdefghij' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 33 "whenever.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 33 "whenever.pgc"


	/* exec sql whenever sql_warning  do warn (  ) ; */
#line 35 "whenever.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  *  from test   ", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(c),(long)6,(long)1,(6)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 36 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 36 "whenever.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "whenever.pgc"

	{ ECPGtrans(__LINE__, NULL, "rollback ");
#line 37 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 37 "whenever.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 37 "whenever.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  *  from nonexistant   ", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 39 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 39 "whenever.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 39 "whenever.pgc"

	{ ECPGtrans(__LINE__, NULL, "rollback ");
#line 40 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 40 "whenever.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 40 "whenever.pgc"


	/* exec sql whenever sqlerror  do print ( \"select\" ) ; */
#line 42 "whenever.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  *  from nonexistant   ", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 43 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 43 "whenever.pgc"

if (sqlca.sqlcode < 0) print ( "select" );}
#line 43 "whenever.pgc"

	{ ECPGtrans(__LINE__, NULL, "rollback ");
#line 44 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 44 "whenever.pgc"

if (sqlca.sqlcode < 0) print ( "select" );}
#line 44 "whenever.pgc"


	/* exec sql whenever sqlerror  call print2 (  ) ; */
#line 46 "whenever.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  *  from nonexistant   ", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 47 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 47 "whenever.pgc"

if (sqlca.sqlcode < 0) print2 (  );}
#line 47 "whenever.pgc"

	{ ECPGtrans(__LINE__, NULL, "rollback ");
#line 48 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 48 "whenever.pgc"

if (sqlca.sqlcode < 0) print2 (  );}
#line 48 "whenever.pgc"


	/* exec sql whenever sqlerror  continue ; */
#line 50 "whenever.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  *  from nonexistant   ", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 51 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );}
#line 51 "whenever.pgc"

	{ ECPGtrans(__LINE__, NULL, "rollback ");
#line 52 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );}
#line 52 "whenever.pgc"


	/* exec sql whenever sqlerror  goto  error ; */
#line 54 "whenever.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  *  from nonexistant   ", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 55 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 55 "whenever.pgc"

if (sqlca.sqlcode < 0) goto error;}
#line 55 "whenever.pgc"

	printf("Should not be reachable\n");

	error:
	{ ECPGtrans(__LINE__, NULL, "rollback ");
#line 59 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 59 "whenever.pgc"

if (sqlca.sqlcode < 0) goto error;}
#line 59 "whenever.pgc"


	/* exec sql whenever sqlerror  stop ; */
#line 61 "whenever.pgc"

	/* This cannot fail, thus we don't get an exit value not equal 0. */
	/* However, it still test the precompiler output. */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select  1     ", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 64 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 64 "whenever.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 64 "whenever.pgc"

	{ ECPGtrans(__LINE__, NULL, "rollback ");
#line 65 "whenever.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 65 "whenever.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 65 "whenever.pgc"

	exit (0);
}	
