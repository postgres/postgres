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
#include <stdlib.h>
#

#line 1 "regression.h"






#line 4 "test_informix.pgc"



static void openit(void);
static void dosqlprint(void) {
	printf("doSQLprint: Error: %s\n", sqlca.sqlerrm.sqlerrmc);
}

int main(void)
{
	
#line 14 "test_informix.pgc"
 int  i   = 14 ;

#line 14 "test_informix.pgc"
 
	
#line 15 "test_informix.pgc"
 decimal  j    ,  m    ,  n    ;

#line 15 "test_informix.pgc"


	ECPGdebug(1, stderr);
	/* exec sql whenever sqlerror  do dosqlprint (  ) ; */
#line 18 "test_informix.pgc"


	{ ECPGconnect(__LINE__, 1, "regress1" , NULL,NULL , NULL, 0); 
#line 20 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 20 "test_informix.pgc"

	if (sqlca.sqlcode != 0) exit(1);

	{ ECPGdo(__LINE__, 1, 1, NULL, "create  table test ( i int   primary key   , j int   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 23 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 23 "test_informix.pgc"


	/* this INSERT works */
	rsetnull(CDECIMALTYPE, (char *)&j);
	{ ECPGdo(__LINE__, 1, 1, NULL, "insert into test ( i  , j  ) values ( 7 ,  ? ) ", 
	ECPGt_decimal,&(j),(long)1,(long)1,sizeof(decimal), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 27 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 27 "test_informix.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 28 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 28 "test_informix.pgc"


	/* this INSERT should fail because i is a unique column */
	{ ECPGdo(__LINE__, 1, 1, NULL, "insert into test ( i  , j  ) values ( 7 , 12 ) ", ECPGt_EOIT, ECPGt_EORT);
#line 31 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 31 "test_informix.pgc"

	printf("INSERT: %ld=%s\n", sqlca.sqlcode, sqlca.sqlerrm.sqlerrmc);
	if (sqlca.sqlcode != 0) { ECPGtrans(__LINE__, NULL, "rollback");
#line 33 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 33 "test_informix.pgc"


	{ ECPGdo(__LINE__, 1, 1, NULL, "insert into test ( i  , j  ) values (  ? , 1 ) ", 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 35 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 35 "test_informix.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 36 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 36 "test_informix.pgc"


	/* this will fail (more than one row in subquery) */
	{ ECPGdo(__LINE__, 1, 1, NULL, "select  i  from test where j = ( select  j  from test    )  ", ECPGt_EOIT, ECPGt_EORT);
#line 39 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 39 "test_informix.pgc"

	{ ECPGtrans(__LINE__, NULL, "rollback");
#line 40 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 40 "test_informix.pgc"


	/* this however should be ok */
	{ ECPGdo(__LINE__, 1, 1, NULL, "select  i  from test where j = ( select  j  from test    order by i limit 1  )  ", ECPGt_EOIT, ECPGt_EORT);
#line 43 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 43 "test_informix.pgc"

	printf("SELECT: %ld=%s\n", sqlca.sqlcode, sqlca.sqlerrm.sqlerrmc);
	if (sqlca.sqlcode != 0) { ECPGtrans(__LINE__, NULL, "rollback");
#line 45 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 45 "test_informix.pgc"


	 ECPG_informix_set_var( 0, &( i ), __LINE__);\
  /* declare c  cursor  for select  *  from test where i <=  ?   */
#line 47 "test_informix.pgc"

	openit();

	deccvint(0, &j);

	while (1)
	{
		{ ECPGdo(__LINE__, 1, 1, NULL, "fetch forward from c", ECPGt_EOIT, 
	ECPGt_int,&(i),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_decimal,&(j),(long)1,(long)1,sizeof(decimal), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 54 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 54 "test_informix.pgc"

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
	{ ECPGdo(__LINE__, 1, 1, NULL, "delete from test  where i =  ? ", 
	ECPGt_decimal,&(n),(long)1,(long)1,sizeof(decimal), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 72 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 72 "test_informix.pgc"

	printf("DELETE: %ld\n", sqlca.sqlcode);

	{ ECPGdo(__LINE__, 1, 1, NULL, "select  1  from test where i = 14  ", ECPGt_EOIT, ECPGt_EORT);
#line 75 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 75 "test_informix.pgc"

	printf("Exists: %ld\n", sqlca.sqlcode);

	{ ECPGdo(__LINE__, 1, 1, NULL, "select  1  from test where i = 147  ", ECPGt_EOIT, ECPGt_EORT);
#line 78 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 78 "test_informix.pgc"

	printf("Does not exist: %ld\n", sqlca.sqlcode);

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 81 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 81 "test_informix.pgc"

	{ ECPGdo(__LINE__, 1, 1, NULL, "drop table test ", ECPGt_EOIT, ECPGt_EORT);
#line 82 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 82 "test_informix.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 83 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 83 "test_informix.pgc"


	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 85 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 85 "test_informix.pgc"


	return 0;
}

static void openit(void)
{
	{ ECPGdo(__LINE__, 1, 1, NULL, "declare c  cursor  for select  *  from test where i <=  ?  ", 
	ECPGt_int,&(*( int  *)(ECPG_informix_get_var( 0))),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 92 "test_informix.pgc"

if (sqlca.sqlcode < 0) dosqlprint (  );}
#line 92 "test_informix.pgc"

}

