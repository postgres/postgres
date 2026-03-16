/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "sqlpgq.pgc"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


#line 1 "regression.h"






#line 5 "sqlpgq.pgc"


/* exec sql whenever sqlerror  sqlprint ; */
#line 7 "sqlpgq.pgc"


int
main(void)
{
/* exec sql begin declare section */
	 
	 
	 
	 

#line 13 "sqlpgq.pgc"
 char command [ 512 ] ;
 
#line 14 "sqlpgq.pgc"
 char search_address [ 10 ] ;
 
#line 15 "sqlpgq.pgc"
 char cname [ 100 ] ;
 
#line 16 "sqlpgq.pgc"
 int reg ;
/* exec sql end declare section */
#line 17 "sqlpgq.pgc"


	ECPGdebug(1, stderr);

	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , "main", 0); 
#line 21 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 21 "sqlpgq.pgc"


	/* Create schema and tables for property graph testing */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create schema graph_ecpg_tests", ECPGt_EOIT, ECPGt_EORT);
#line 24 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 24 "sqlpgq.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set search_path = graph_ecpg_tests", ECPGt_EOIT, ECPGt_EORT);
#line 25 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 25 "sqlpgq.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table customers ( customer_id integer primary key , name varchar , address varchar )", ECPGt_EOIT, ECPGt_EORT);
#line 31 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 31 "sqlpgq.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table orders ( order_id integer primary key , register integer )", ECPGt_EOIT, ECPGt_EORT);
#line 36 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "sqlpgq.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table customer_orders ( customer_orders_id integer primary key , customer_id integer references customers ( customer_id ) , order_id integer references orders ( order_id ) )", ECPGt_EOIT, ECPGt_EORT);
#line 42 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 42 "sqlpgq.pgc"


	/* Insert test data */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into customers values ( 1 , 'customer1' , 'US' ) , ( 2 , 'customer2' , 'CA' ) , ( 3 , 'customer3' , 'GL' )", ECPGt_EOIT, ECPGt_EORT);
#line 45 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 45 "sqlpgq.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into orders values ( 1 , 100 ) , ( 2 , 200 ) , ( 3 , 500 )", ECPGt_EOIT, ECPGt_EORT);
#line 46 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 46 "sqlpgq.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into customer_orders ( customer_orders_id , customer_id , order_id ) values ( 1 , 1 , 1 ) , ( 2 , 2 , 2 ) , ( 3 , 3 , 3 )", ECPGt_EOIT, ECPGt_EORT);
#line 47 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 47 "sqlpgq.pgc"


	/* Create property graph */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create property graph shopgraph vertex tables ( customers , orders ) edge tables ( customer_orders key ( customer_orders_id ) source key ( customer_id ) references customers ( customer_id ) destination key ( order_id ) references orders ( order_id ) )", ECPGt_EOIT, ECPGt_EORT);
#line 59 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 59 "sqlpgq.pgc"


	{ ECPGtrans(__LINE__, NULL, "commit");
#line 61 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 61 "sqlpgq.pgc"


	/* direct sql - US customers */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select * from graph_table ( shopgraph match ( c is customers where c . address = 'US' ) - [ is customer_orders ] -> ( o is orders ) columns ( c . name , o . register ) )", ECPGt_EOIT, 
	ECPGt_char,(cname),(long)100,(long)1,(100)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(reg),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 64 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 64 "sqlpgq.pgc"

	printf("found %ld results (%s, %d)\n", sqlca.sqlerrd[2], cname, reg);

	/* direct sql with C variable - GL customers */
	strcpy(search_address, "GL");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select * from graph_table ( shopgraph match ( c is customers where c . address = $1  ) - [ is customer_orders ] -> ( o is orders ) columns ( c . name , o . register ) )", 
	ECPGt_char,(search_address),(long)10,(long)1,(10)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_char,(cname),(long)100,(long)1,(100)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(reg),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 69 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 69 "sqlpgq.pgc"

	printf("found %ld results (%s, %d)\n", sqlca.sqlerrd[2], cname, reg);

	/* prepared statement - CA customers */
	sprintf(command, "select * from graph_table (shopgraph match (c is customers where c.address = $1)-[is customer_orders]->(o is orders) columns (c.name, o.register))");
	{ ECPGprepare(__LINE__, NULL, 0, "graph_stmt", command);
#line 74 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 74 "sqlpgq.pgc"

	strcpy(search_address, "CA");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "graph_stmt", 
	ECPGt_char,(search_address),(long)10,(long)1,(10)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_char,(cname),(long)100,(long)1,(100)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(reg),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 76 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 76 "sqlpgq.pgc"

	printf("found %ld results (%s, %d)\n", sqlca.sqlerrd[2], cname, reg);
	{ ECPGdeallocate(__LINE__, 0, NULL, "graph_stmt");
#line 78 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 78 "sqlpgq.pgc"


	/* cursor test - all customers with orders */
	/* declare graph_cursor cursor for select * from graph_table ( shopgraph match ( c is customers ) - [ is customer_orders ] -> ( o is orders ) columns ( c . name , o . register ) ) order by name */
#line 81 "sqlpgq.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare graph_cursor cursor for select * from graph_table ( shopgraph match ( c is customers ) - [ is customer_orders ] -> ( o is orders ) columns ( c . name , o . register ) ) order by name", ECPGt_EOIT, ECPGt_EORT);
#line 82 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 82 "sqlpgq.pgc"

	/* exec sql whenever not found  break ; */
#line 83 "sqlpgq.pgc"

	while (1) {
		{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch graph_cursor", ECPGt_EOIT, 
	ECPGt_char,(cname),(long)100,(long)1,(100)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(reg),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 85 "sqlpgq.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 85 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 85 "sqlpgq.pgc"

		printf("cursor result: %s, %d\n", cname, reg);
	}
	/* exec sql whenever not found  continue ; */
#line 88 "sqlpgq.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close graph_cursor", ECPGt_EOIT, ECPGt_EORT);
#line 89 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 89 "sqlpgq.pgc"


	/* label disjunction syntax test */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select * from graph_table ( shopgraph match ( c is customers | customers where c . address = 'US' ) columns ( c . name ) )", ECPGt_EOIT, 
	ECPGt_char,(cname),(long)100,(long)1,(100)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 92 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 92 "sqlpgq.pgc"

	printf("found %ld results (%s)\n", sqlca.sqlerrd[2], cname);

	/* Clean up */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop property graph shopgraph", ECPGt_EOIT, ECPGt_EORT);
#line 96 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 96 "sqlpgq.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table customer_orders", ECPGt_EOIT, ECPGt_EORT);
#line 97 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 97 "sqlpgq.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table orders", ECPGt_EOIT, ECPGt_EORT);
#line 98 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 98 "sqlpgq.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table customers", ECPGt_EOIT, ECPGt_EORT);
#line 99 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 99 "sqlpgq.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop schema graph_ecpg_tests", ECPGt_EOIT, ECPGt_EORT);
#line 100 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 100 "sqlpgq.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 101 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 101 "sqlpgq.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 102 "sqlpgq.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 102 "sqlpgq.pgc"


	return 0;
}
