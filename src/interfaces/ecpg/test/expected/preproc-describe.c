/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "describe.pgc"
#include <stdlib.h>
#include <string.h>


#line 1 "regression.h"






#line 4 "describe.pgc"


/* exec sql whenever sqlerror  stop ; */
#line 6 "describe.pgc"


int
main (void)
{
/* exec sql begin declare section */
		  
		  
		    
		  
		  
		  
		  

#line 12 "describe.pgc"
 char * stmt1 = "SELECT id, t FROM t1" ;
 
#line 13 "describe.pgc"
 char * stmt2 = "SELECT id, t FROM t1 WHERE id = -1" ;
 
#line 14 "describe.pgc"
 int i , count1 , count2 , count3 , count4 ;
 
#line 15 "describe.pgc"
 char field_name1 [ 30 ] = "not set" ;
 
#line 16 "describe.pgc"
 char field_name2 [ 30 ] = "not set" ;
 
#line 17 "describe.pgc"
 char field_name3 [ 30 ] = "not set" ;
 
#line 18 "describe.pgc"
 char field_name4 [ 30 ] = "not set" ;
/* exec sql end declare section */
#line 19 "describe.pgc"


	char msg[128];

	ECPGdebug(1, stderr);

	strcpy(msg, "connect");
	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 26 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 26 "describe.pgc"


	strcpy(msg, "set");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set datestyle to iso", ECPGt_EOIT, ECPGt_EORT);
#line 29 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 29 "describe.pgc"


	strcpy(msg, "create");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table t1 ( id serial primary key , t text )", ECPGt_EOIT, ECPGt_EORT);
#line 32 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 32 "describe.pgc"


	strcpy(msg, "insert");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'a' )", ECPGt_EOIT, ECPGt_EORT);
#line 35 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 35 "describe.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'b' )", ECPGt_EOIT, ECPGt_EORT);
#line 36 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 36 "describe.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'c' )", ECPGt_EOIT, ECPGt_EORT);
#line 37 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 37 "describe.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t1 ( id , t ) values ( default , 'd' )", ECPGt_EOIT, ECPGt_EORT);
#line 38 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 38 "describe.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 41 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 41 "describe.pgc"


	/*
	 * Test DESCRIBE with a query producing tuples.
	 * DESCRIPTOR and SQL DESCRIPTOR are the same in native mode.
	 */

	strcpy(msg, "allocate");
	ECPGallocate_desc(__LINE__, "desc1");
#line 49 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 49 "describe.pgc"

	ECPGallocate_desc(__LINE__, "desc2");
#line 50 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 50 "describe.pgc"

	ECPGallocate_desc(__LINE__, "desc3");
#line 51 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 51 "describe.pgc"

	ECPGallocate_desc(__LINE__, "desc4");
#line 52 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 52 "describe.pgc"


	strcpy(msg, "prepare");
	{ ECPGprepare(__LINE__, NULL, 0, "st_id1", stmt1);
#line 55 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 55 "describe.pgc"


	strcpy(msg, "describe");
	{ ECPGdescribe(__LINE__, 0, NULL, "st_id1",
	ECPGt_descriptor, "desc1", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 58 "describe.pgc"

	{ ECPGdescribe(__LINE__, 0, NULL, "st_id1",
	ECPGt_descriptor, "desc2", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 59 "describe.pgc"

	{ ECPGdescribe(__LINE__, 0, NULL, "st_id1",
	ECPGt_descriptor, "desc3", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 60 "describe.pgc"

	{ ECPGdescribe(__LINE__, 0, NULL, "st_id1",
	ECPGt_descriptor, "desc4", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 61 "describe.pgc"


	strcpy(msg, "get descriptor");
	{ ECPGget_desc_header(__LINE__, "desc1", &(count1));

#line 64 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 64 "describe.pgc"

	{ ECPGget_desc_header(__LINE__, "desc2", &(count2));

#line 65 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 65 "describe.pgc"

	{ ECPGget_desc_header(__LINE__, "desc3", &(count3));

#line 66 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 66 "describe.pgc"

	{ ECPGget_desc_header(__LINE__, "desc4", &(count4));

#line 67 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 67 "describe.pgc"


	if (!(count1 == count2 && count1 == count3 && count1 == count4))
		exit(1);

	for (i = 1; i <= count1; i++)
	{
		{ ECPGget_desc(__LINE__, "desc1", i,ECPGd_name,
	ECPGt_char,(field_name1),(long)30,(long)1,(30)*sizeof(char), ECPGd_EODT);

#line 74 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 74 "describe.pgc"

		{ ECPGget_desc(__LINE__, "desc2", i,ECPGd_name,
	ECPGt_char,(field_name2),(long)30,(long)1,(30)*sizeof(char), ECPGd_EODT);

#line 75 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 75 "describe.pgc"

		{ ECPGget_desc(__LINE__, "desc3", i,ECPGd_name,
	ECPGt_char,(field_name3),(long)30,(long)1,(30)*sizeof(char), ECPGd_EODT);

#line 76 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 76 "describe.pgc"

		{ ECPGget_desc(__LINE__, "desc4", i,ECPGd_name,
	ECPGt_char,(field_name4),(long)30,(long)1,(30)*sizeof(char), ECPGd_EODT);

#line 77 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 77 "describe.pgc"

		printf("field_name 1 '%s' 2 '%s' 3 '%s' 4 '%s'\n",
			field_name1, field_name2, field_name3, field_name4);
	}

	strcpy(msg, "deallocate");
	ECPGdeallocate_desc(__LINE__, "desc1");
#line 83 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 83 "describe.pgc"

	ECPGdeallocate_desc(__LINE__, "desc2");
#line 84 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 84 "describe.pgc"

	ECPGdeallocate_desc(__LINE__, "desc3");
#line 85 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 85 "describe.pgc"

	ECPGdeallocate_desc(__LINE__, "desc4");
#line 86 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 86 "describe.pgc"


	{ ECPGdeallocate(__LINE__, 0, NULL, "st_id1");
#line 88 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 88 "describe.pgc"


	/* Test DESCRIBE with a query not producing tuples */

	strcpy(msg, "allocate");
	ECPGallocate_desc(__LINE__, "desc1");
#line 93 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 93 "describe.pgc"

	ECPGallocate_desc(__LINE__, "desc2");
#line 94 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 94 "describe.pgc"

	ECPGallocate_desc(__LINE__, "desc3");
#line 95 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 95 "describe.pgc"

	ECPGallocate_desc(__LINE__, "desc4");
#line 96 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 96 "describe.pgc"


	strcpy(msg, "prepare");
	{ ECPGprepare(__LINE__, NULL, 0, "st_id2", stmt2);
#line 99 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 99 "describe.pgc"


	strcpy(msg, "describe");
	{ ECPGdescribe(__LINE__, 0, NULL, "st_id2",
	ECPGt_descriptor, "desc1", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 102 "describe.pgc"

	{ ECPGdescribe(__LINE__, 0, NULL, "st_id2",
	ECPGt_descriptor, "desc2", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 103 "describe.pgc"

	{ ECPGdescribe(__LINE__, 0, NULL, "st_id2",
	ECPGt_descriptor, "desc3", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 104 "describe.pgc"

	{ ECPGdescribe(__LINE__, 0, NULL, "st_id2",
	ECPGt_descriptor, "desc4", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 105 "describe.pgc"


	strcpy(msg, "get descriptor");
	{ ECPGget_desc_header(__LINE__, "desc1", &(count1));

#line 108 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 108 "describe.pgc"

	{ ECPGget_desc_header(__LINE__, "desc2", &(count2));

#line 109 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 109 "describe.pgc"

	{ ECPGget_desc_header(__LINE__, "desc3", &(count3));

#line 110 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 110 "describe.pgc"

	{ ECPGget_desc_header(__LINE__, "desc4", &(count4));

#line 111 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 111 "describe.pgc"


	if (!(count1 == count2 && count1 == count3 && count1 == count4))
		exit(1);

	for (i = 1; i <= count1; i++)
	{
		{ ECPGget_desc(__LINE__, "desc1", i,ECPGd_name,
	ECPGt_char,(field_name1),(long)30,(long)1,(30)*sizeof(char), ECPGd_EODT);

#line 118 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 118 "describe.pgc"

		{ ECPGget_desc(__LINE__, "desc2", i,ECPGd_name,
	ECPGt_char,(field_name2),(long)30,(long)1,(30)*sizeof(char), ECPGd_EODT);

#line 119 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 119 "describe.pgc"

		{ ECPGget_desc(__LINE__, "desc3", i,ECPGd_name,
	ECPGt_char,(field_name3),(long)30,(long)1,(30)*sizeof(char), ECPGd_EODT);

#line 120 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 120 "describe.pgc"

		{ ECPGget_desc(__LINE__, "desc4", i,ECPGd_name,
	ECPGt_char,(field_name4),(long)30,(long)1,(30)*sizeof(char), ECPGd_EODT);

#line 121 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 121 "describe.pgc"

		printf("field_name 1 '%s' 2 '%s' 3 '%s' 4 '%s'\n",
			field_name1, field_name2, field_name3, field_name4);
	}

	strcpy(msg, "deallocate");
	ECPGdeallocate_desc(__LINE__, "desc1");
#line 127 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 127 "describe.pgc"

	ECPGdeallocate_desc(__LINE__, "desc2");
#line 128 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 128 "describe.pgc"

	ECPGdeallocate_desc(__LINE__, "desc3");
#line 129 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 129 "describe.pgc"

	ECPGdeallocate_desc(__LINE__, "desc4");
#line 130 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 130 "describe.pgc"


	{ ECPGdeallocate(__LINE__, 0, NULL, "st_id2");
#line 132 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 132 "describe.pgc"



	/* End test */

	strcpy(msg, "drop");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table t1", ECPGt_EOIT, ECPGt_EORT);
#line 138 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 138 "describe.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 141 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 141 "describe.pgc"


	strcpy(msg, "disconnect"); 
	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 144 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 144 "describe.pgc"


	return 0;
}
