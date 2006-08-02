/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* Needed for informix compatibility */
#include <ecpg_informix.h>
/* End of automatic include section */

#line 1 "test_informix.pgc"
#include "sqltypes.h"


#line 1 "./../regression.h"






#line 3 "test_informix.pgc"



static void openit(void);
static void dosqlprint(void) {
	printf("doSQLprint: Error: %s\n", sqlca.sqlerrm.sqlerrmc);
}

int main(void)
{
	
#line 13 "test_informix.pgc"
 int  i   = 14 ;

#line 13 "test_informix.pgc"
 
	
#line 14 "test_informix.pgc"
 decimal  j    ,  m    ,  n    ;

#line 14 "test_informix.pgc"


	ECPGdebug(1, stderr);
	/* exec sql whenever sqlerror  do dosqlprint (  ) ; */
#line 17 "test_informix.pgc"


	{ ECPGconnect(__LINE__, 1, "regress1" , NULL,NULL , NULL, 0); 
#line 19 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 19 "test_informix.pgc"

	if (sqlca.sqlcode != 0) exit(1);

	{ ECPGdo(__LINE__, 1, 1, NULL, "create  table test ( i int   primary key   , j int   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 22 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 22 "test_informix.pgc"


	/* this INSERT works */
	rsetnull(CDECIMALTYPE, (char *)&j);
	{ ECPGdo(__LINE__, 1, 1, NULL, "insert into test ( i  , j  ) values( 7 ,  ? )", 
	ECPGt_decimal,&(j),(long)1,(long)1,sizeof(decimal), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 26 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 26 "test_informix.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 27 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 27 "test_informix.pgc"


	/* this INSERT should fail because i is a unique column */
	{ ECPGdo(__LINE__, 1, 1, NULL, "insert into test ( i  , j  ) values( 7 , 12 )", ECPGt_EOIT, ECPGt_EORT);
#line 30 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 30 "test_informix.pgc"

	printf("INSERT: %ld=%s\n", sqlca.sqlcode, sqlca.sqlerrm.sqlerrmc);
	if (sqlca.sqlcode != 0) { ECPGtrans(__LINE__, NULL, "rollback");
#line 32 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 32 "test_informix.pgc"


	{ ECPGdo(__LINE__, 1, 1, NULL, "insert into test ( i  , j  ) values(  ? , 1 )", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 34 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 34 "test_informix.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 35 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 35 "test_informix.pgc"


	/* this will fail (more than one row in subquery) */
	{ ECPGdo(__LINE__, 1, 1, NULL, "select  i  from test where j = ( select  j  from test    )  ", ECPGt_EOIT, ECPGt_EORT);
#line 38 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 38 "test_informix.pgc"


	/* this however should be ok */
	{ ECPGdo(__LINE__, 1, 1, NULL, "select  i  from test where j = ( select  j  from test     limit 1  )  ", ECPGt_EOIT, ECPGt_EORT);
#line 41 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 41 "test_informix.pgc"

	printf("SELECT: %ld=%s\n", sqlca.sqlcode, sqlca.sqlerrm.sqlerrmc);
	if (sqlca.sqlcode != 0) { ECPGtrans(__LINE__, NULL, "rollback");
#line 43 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 43 "test_informix.pgc"


	 ECPG_informix_set_var( 0, &( i ), __LINE__);\
  /* declare c  cursor  for select  *  from test where i <=  ?   */
#line 45 "test_informix.pgc"

	openit();

	deccvint(0, &j);

	while (1)
	{
		{ ECPGdo(__LINE__, 1, 1, NULL, "fetch forward from c", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_decimal,&(j),(long)1,(long)1,sizeof(decimal), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 52 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 52 "test_informix.pgc"

		if (sqlca.sqlcode == 100) break;
		else if (sqlca.sqlcode != 0) printf ("Error: %ld\n", sqlca.sqlcode);

		if (risnull(CDECIMALTYPE, (char *)&j))
			printf("%d NULL\n", i);
		else
		{
			int a;

			dectoint(&j, &a);
			printf("%d %d\n", i, a);
		}
	}

	deccvint(7, &j);
	deccvint(14, &m);
	decadd(&j, &m, &n);
	{ ECPGdo(__LINE__, 1, 1, NULL, "delete from test  where i =  ?", 
	ECPGt_decimal,&(n),(long)1,(long)1,sizeof(decimal), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 70 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 70 "test_informix.pgc"

	printf("DELETE: %ld\n", sqlca.sqlcode);

	{ ECPGdo(__LINE__, 1, 1, NULL, "select  1  from test where i = 14  ", ECPGt_EOIT, ECPGt_EORT);
#line 73 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 73 "test_informix.pgc"

	printf("Exists: %ld\n", sqlca.sqlcode);

	{ ECPGdo(__LINE__, 1, 1, NULL, "select  1  from test where i = 147  ", ECPGt_EOIT, ECPGt_EORT);
#line 76 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 76 "test_informix.pgc"

	printf("Does not exist: %ld\n", sqlca.sqlcode);

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 79 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 79 "test_informix.pgc"

	{ ECPGdo(__LINE__, 1, 1, NULL, "drop table test ", ECPGt_EOIT, ECPGt_EORT);
#line 80 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 80 "test_informix.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 81 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 81 "test_informix.pgc"


	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 83 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 83 "test_informix.pgc"


	return 0;
}

static void openit(void)
{
	{ ECPGdo(__LINE__, 1, 1, NULL, "declare c  cursor  for select  *  from test where i <=  ?  ", 
	ECPGt_int,&(*( int  *)(ECPG_informix_get_var( 0))),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 90 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 90 "test_informix.pgc"

}

