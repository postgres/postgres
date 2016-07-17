/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* Needed for informix compatibility */
#include <ecpg_informix.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "test_informix.pgc"
#include "sqltypes.h"
#include <stdlib.h>


#line 1 "regression.h"






#line 4 "test_informix.pgc"



static void openit(void);
static void dosqlprint(void) {
	printf("doSQLprint: Error: %s\n", sqlca.sqlerrm.sqlerrmc);
}

int main(void)
{
	
#line 14 "test_informix.pgc"
 int i = 14 ;

#line 14 "test_informix.pgc"

	
#line 15 "test_informix.pgc"
 decimal j , m , n ;

#line 15 "test_informix.pgc"

	
#line 16 "test_informix.pgc"
 char c [ 10 ] ;

#line 16 "test_informix.pgc"


	ECPGdebug(1, stderr);
	/* exec sql whenever sqlerror  do dosqlprint ( ) ; */
#line 19 "test_informix.pgc"


	{ ECPGconnect(__LINE__, 1, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 21 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 21 "test_informix.pgc"

	if (sqlca.sqlcode != 0) exit(1);

	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "create table test ( i int primary key , j int , c text )", ECPGt_EOIT, ECPGt_EORT);
#line 24 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 24 "test_informix.pgc"


	/* this INSERT works */
	rsetnull(CDECIMALTYPE, (char *)&j);
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "insert into test ( i , j , c ) values ( 7 , $1  , 'test   ' )", 
	ECPGt_decimal,&(j),(long)1,(long)1,sizeof(decimal), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 28 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 28 "test_informix.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 29 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 29 "test_informix.pgc"


	/* this INSERT should fail because i is a unique column */
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "insert into test ( i , j , c ) values ( 7 , 12 , 'a' )", ECPGt_EOIT, ECPGt_EORT);
#line 32 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 32 "test_informix.pgc"

	printf("INSERT: %ld=%s\n", sqlca.sqlcode, sqlca.sqlerrm.sqlerrmc);
	if (sqlca.sqlcode != 0) { ECPGtrans(__LINE__, NULL, "rollback");
#line 34 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 34 "test_informix.pgc"


	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "insert into test ( i , j , c ) values ( $1  , 1 , 'a      ' )", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 36 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 36 "test_informix.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 37 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 37 "test_informix.pgc"


	/* this will fail (more than one row in subquery) */
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "select i from test where j = ( select j from test )", ECPGt_EOIT, ECPGt_EORT);
#line 40 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 40 "test_informix.pgc"

	{ ECPGtrans(__LINE__, NULL, "rollback");
#line 41 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 41 "test_informix.pgc"


	/* this however should be ok */
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "select i from test where j = ( select j from test order by i limit 1 )", ECPGt_EOIT, ECPGt_EORT);
#line 44 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 44 "test_informix.pgc"

	printf("SELECT: %ld=%s\n", sqlca.sqlcode, sqlca.sqlerrm.sqlerrmc);
	if (sqlca.sqlcode != 0) { ECPGtrans(__LINE__, NULL, "rollback");
#line 46 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 46 "test_informix.pgc"


	sqlca.sqlcode = 100;
	ECPGset_var( 0, &( i ), __LINE__);\
 ECPG_informix_reset_sqlca(); /* declare c cursor for select * from test where i <= $1  */
#line 49 "test_informix.pgc"

	printf ("%ld\n", sqlca.sqlcode);
	openit();

	deccvint(0, &j);

	while (1)
	{
		{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "fetch forward c", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_decimal,&(j),(long)1,(long)1,sizeof(decimal), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_string,(c),(long)10,(long)1,(10)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 57 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 57 "test_informix.pgc"

		if (sqlca.sqlcode == 100) break;
		else if (sqlca.sqlcode != 0) printf ("Error: %ld\n", sqlca.sqlcode);

		if (risnull(CDECIMALTYPE, (char *)&j))
			printf("%d NULL\n", i);
		else
		{
			int a;

			dectoint(&j, &a);
			printf("%d %d \"%s\"\n", i, a, c);
		}
	}

	deccvint(7, &j);
	deccvint(14, &m);
	decadd(&j, &m, &n);
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "delete from test where i = $1  :: decimal", 
	ECPGt_decimal,&(n),(long)1,(long)1,sizeof(decimal), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 75 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 75 "test_informix.pgc"

	printf("DELETE: %ld\n", sqlca.sqlcode);

	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "select 1 from test where i = 14", ECPGt_EOIT, ECPGt_EORT);
#line 78 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 78 "test_informix.pgc"

	printf("Exists: %ld\n", sqlca.sqlcode);

	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "select 1 from test where i = 147", ECPGt_EOIT, ECPGt_EORT);
#line 81 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 81 "test_informix.pgc"

	printf("Does not exist: %ld\n", sqlca.sqlcode);

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 84 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 84 "test_informix.pgc"

	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "drop table test", ECPGt_EOIT, ECPGt_EORT);
#line 85 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 85 "test_informix.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 86 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 86 "test_informix.pgc"


	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 88 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 88 "test_informix.pgc"


	return 0;
}

static void openit(void)
{
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "declare c cursor for select * from test where i <= $1 ", 
	ECPGt_int,&(*( int  *)(ECPGget_var( 0))),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 95 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint ( );}
#line 95 "test_informix.pgc"

}
