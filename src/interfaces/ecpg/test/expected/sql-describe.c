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


#line 1 "sqlda.h"
#ifndef ECPG_SQLDA_H
#define ECPG_SQLDA_H

#ifdef _ECPG_INFORMIX_H

#include "sqlda-compat.h"
typedef struct sqlvar_compat sqlvar_t;
typedef struct sqlda_compat sqlda_t;

#else

#include "sqlda-native.h"
typedef struct sqlvar_struct sqlvar_t;
typedef struct sqlda_struct sqlda_t;

#endif

#endif   /* ECPG_SQLDA_H */

#line 5 "describe.pgc"


/* exec sql whenever sqlerror  stop ; */
#line 7 "describe.pgc"


sqlda_t	*sqlda1, *sqlda2, *sqlda3;

int
main (void)
{
/* exec sql begin declare section */
		  
		  
		  
		  
		  

#line 15 "describe.pgc"
 char * stmt1 = "SELECT id, t FROM descr_t2" ;
 
#line 16 "describe.pgc"
 char * stmt2 = "SELECT id, t FROM descr_t2 WHERE id = -1" ;
 
#line 17 "describe.pgc"
 int i , count1 , count2 ;
 
#line 18 "describe.pgc"
 char field_name1 [ 30 ] = "not set" ;
 
#line 19 "describe.pgc"
 char field_name2 [ 30 ] = "not set" ;
/* exec sql end declare section */
#line 20 "describe.pgc"


	char msg[128];

	ECPGdebug(1, stderr);

	strcpy(msg, "connect");
	{ ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); 
#line 27 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 27 "describe.pgc"


	strcpy(msg, "set");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set datestyle to iso", ECPGt_EOIT, ECPGt_EORT);
#line 30 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 30 "describe.pgc"


	strcpy(msg, "create");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table descr_t2 ( id serial primary key , t text )", ECPGt_EOIT, ECPGt_EORT);
#line 33 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 33 "describe.pgc"


	strcpy(msg, "insert");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into descr_t2 ( id , t ) values ( default , 'a' )", ECPGt_EOIT, ECPGt_EORT);
#line 36 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 36 "describe.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into descr_t2 ( id , t ) values ( default , 'b' )", ECPGt_EOIT, ECPGt_EORT);
#line 37 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 37 "describe.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into descr_t2 ( id , t ) values ( default , 'c' )", ECPGt_EOIT, ECPGt_EORT);
#line 38 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 38 "describe.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into descr_t2 ( id , t ) values ( default , 'd' )", ECPGt_EOIT, ECPGt_EORT);
#line 39 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 39 "describe.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 42 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 42 "describe.pgc"


	/*
	 * Test DESCRIBE with a query producing tuples.
	 * DESCRIPTOR and SQL DESCRIPTOR are NOT the same in
	 * Informix-compat mode.
	 */

	strcpy(msg, "allocate");
	ECPGallocate_desc(__LINE__, "desc1");
#line 51 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 51 "describe.pgc"

	ECPGallocate_desc(__LINE__, "desc2");
#line 52 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 52 "describe.pgc"


	strcpy(msg, "prepare");
	{ ECPGprepare(__LINE__, NULL, 0, "st_id1", stmt1);
#line 55 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 55 "describe.pgc"


	sqlda1 = sqlda2 = sqlda3 = NULL;

	strcpy(msg, "describe");
	{ ECPGdescribe(__LINE__, 0, 0, NULL, "st_id1",
	ECPGt_descriptor, "desc1", 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 60 "describe.pgc"

	{ ECPGdescribe(__LINE__, 0, 0, NULL, "st_id1",
	ECPGt_descriptor, "desc2", 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 61 "describe.pgc"


	{ ECPGdescribe(__LINE__, 0, 0, NULL, "st_id1",
	ECPGt_sqlda, &sqlda1, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 63 "describe.pgc"

	{ ECPGdescribe(__LINE__, 0, 0, NULL, "st_id1",
	ECPGt_sqlda, &sqlda2, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 64 "describe.pgc"

	{ ECPGdescribe(__LINE__, 0, 0, NULL, "st_id1",
	ECPGt_sqlda, &sqlda3, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 65 "describe.pgc"


	if (sqlda1 == NULL)
	{
		printf("sqlda1 NULL\n");
		exit(1);
	}

	if (sqlda2 == NULL)
	{
		printf("sqlda2 NULL\n");
		exit(1);
	}

	if (sqlda3 == NULL)
	{
		printf("sqlda3 NULL\n");
		exit(1);
	}

	strcpy(msg, "get descriptor");
	{ ECPGget_desc_header(__LINE__, "desc1", &(count1));

#line 86 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 86 "describe.pgc"

	{ ECPGget_desc_header(__LINE__, "desc1", &(count2));

#line 87 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 87 "describe.pgc"


	if (count1 != count2)
	{
		printf("count1 (%d) != count2 (%d)\n", count1, count2);
		exit(1);
	}

	if (count1 != sqlda1->sqld)
	{
		printf("count1 (%d) != sqlda1->sqld (%d)\n", count1, sqlda1->sqld);
		exit(1);
	}

	if (count1 != sqlda2->sqld)
	{
		printf("count1 (%d) != sqlda2->sqld (%d)\n", count1, sqlda2->sqld);
		exit(1);
	}

	if (count1 != sqlda3->sqld)
	{
		printf("count1 (%d) != sqlda3->sqld (%d)\n", count1, sqlda3->sqld);
		exit(1);
	}

	for (i = 1; i <= count1; i++)
	{
		{ ECPGget_desc(__LINE__, "desc1", i,ECPGd_name,
	ECPGt_char,(field_name1),(long)30,(long)1,(30)*sizeof(char), ECPGd_EODT);

#line 115 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 115 "describe.pgc"

		{ ECPGget_desc(__LINE__, "desc2", i,ECPGd_name,
	ECPGt_char,(field_name2),(long)30,(long)1,(30)*sizeof(char), ECPGd_EODT);

#line 116 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 116 "describe.pgc"

		printf("%d\n\tfield_name1 '%s'\n\tfield_name2 '%s'\n\t"
			"sqlda1 '%s'\n\tsqlda2 '%s'\n\tsqlda3 '%s'\n",
			i, field_name1, field_name2,
			sqlda1->sqlvar[i-1].sqlname.data,
			sqlda2->sqlvar[i-1].sqlname.data,
			sqlda3->sqlvar[i-1].sqlname.data);
	}

	strcpy(msg, "deallocate");
	ECPGdeallocate_desc(__LINE__, "desc1");
#line 126 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 126 "describe.pgc"

	ECPGdeallocate_desc(__LINE__, "desc2");
#line 127 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 127 "describe.pgc"

	free(sqlda1);
	free(sqlda2);
	free(sqlda3);

	{ ECPGdeallocate(__LINE__, 0, NULL, "st_id1");
#line 132 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 132 "describe.pgc"


	/* Test DESCRIBE with a query not producing tuples */

	strcpy(msg, "allocate");
	ECPGallocate_desc(__LINE__, "desc1");
#line 137 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 137 "describe.pgc"

	ECPGallocate_desc(__LINE__, "desc2");
#line 138 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 138 "describe.pgc"


	strcpy(msg, "prepare");
	{ ECPGprepare(__LINE__, NULL, 0, "st_id2", stmt2);
#line 141 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 141 "describe.pgc"


	sqlda1 = sqlda2 = sqlda3 = NULL;

	strcpy(msg, "describe");
	{ ECPGdescribe(__LINE__, 0, 0, NULL, "st_id2",
	ECPGt_descriptor, "desc1", 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 146 "describe.pgc"

	{ ECPGdescribe(__LINE__, 0, 0, NULL, "st_id2",
	ECPGt_descriptor, "desc2", 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 147 "describe.pgc"


	{ ECPGdescribe(__LINE__, 0, 0, NULL, "st_id2",
	ECPGt_sqlda, &sqlda1, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 149 "describe.pgc"

	{ ECPGdescribe(__LINE__, 0, 0, NULL, "st_id2",
	ECPGt_sqlda, &sqlda2, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 150 "describe.pgc"

	{ ECPGdescribe(__LINE__, 0, 0, NULL, "st_id2",
	ECPGt_sqlda, &sqlda3, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 151 "describe.pgc"


	if (sqlda1 == NULL || sqlda2 == NULL || sqlda3 == NULL)
		exit(1);

	strcpy(msg, "get descriptor");
	{ ECPGget_desc_header(__LINE__, "desc1", &(count1));

#line 157 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 157 "describe.pgc"

	{ ECPGget_desc_header(__LINE__, "desc1", &(count2));

#line 158 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 158 "describe.pgc"


	if (!(	count1 == count2 &&
		count1 == sqlda1->sqld &&
		count1 == sqlda2->sqld &&
		count1 == sqlda3->sqld))
		exit(1);

	for (i = 1; i <= count1; i++)
	{
		{ ECPGget_desc(__LINE__, "desc1", i,ECPGd_name,
	ECPGt_char,(field_name1),(long)30,(long)1,(30)*sizeof(char), ECPGd_EODT);

#line 168 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 168 "describe.pgc"

		{ ECPGget_desc(__LINE__, "desc2", i,ECPGd_name,
	ECPGt_char,(field_name2),(long)30,(long)1,(30)*sizeof(char), ECPGd_EODT);

#line 169 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 169 "describe.pgc"

		printf("%d\n\tfield_name1 '%s'\n\tfield_name2 '%s'\n\t"
			"sqlda1 '%s'\n\tsqlda2 '%s'\n\tsqlda3 '%s'\n",
			i, field_name1, field_name2,
			sqlda1->sqlvar[i-1].sqlname.data,
			sqlda2->sqlvar[i-1].sqlname.data,
			sqlda3->sqlvar[i-1].sqlname.data);
	}

	strcpy(msg, "deallocate");
	ECPGdeallocate_desc(__LINE__, "desc1");
#line 179 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 179 "describe.pgc"

	ECPGdeallocate_desc(__LINE__, "desc2");
#line 180 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 180 "describe.pgc"

	free(sqlda1);
	free(sqlda2);
	free(sqlda3);

	{ ECPGdeallocate(__LINE__, 0, NULL, "st_id2");
#line 185 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 185 "describe.pgc"


	/* End test */

	strcpy(msg, "drop");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table descr_t2", ECPGt_EOIT, ECPGt_EORT);
#line 190 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 190 "describe.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 193 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 193 "describe.pgc"


	strcpy(msg, "disconnect");
	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 196 "describe.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 196 "describe.pgc"


	return (0);
}
