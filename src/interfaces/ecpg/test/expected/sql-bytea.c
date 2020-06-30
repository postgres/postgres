/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "bytea.pgc"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>


#line 1 "regression.h"






#line 6 "bytea.pgc"

/* exec sql whenever sqlerror  sqlprint ; */
#line 7 "bytea.pgc"


static void
dump_binary(char *buf, int len, int ind)
{
	int i;

	printf("len=%d, ind=%d, data=0x", len, ind);
	for (i = 0; i < len; ++i)
		printf("%02x", 0xff & buf[i]);
	printf("\n");
}

#define DATA_SIZE 0x200
#define LACK_SIZE 13
#
int
main(void)
{
/* exec sql begin declare section */
	 
	 
	 
	   
	 

#line 27 "bytea.pgc"
  struct bytea_1  { int len; char arr[ 512 ]; }  send_buf [ 2 ] ;
 
#line 28 "bytea.pgc"
  struct bytea_2  { int len; char arr[ DATA_SIZE ]; }  recv_buf [ 2 ] ;
 
#line 29 "bytea.pgc"
  struct bytea_3  { int len; char arr[ DATA_SIZE ]; } * recv_vlen_buf ;
 
#line 30 "bytea.pgc"
  struct bytea_4  { int len; char arr[ DATA_SIZE - LACK_SIZE ]; }  recv_short_buf ;
 
#line 31 "bytea.pgc"
 int ind [ 2 ] ;
/* exec sql end declare section */
#line 32 "bytea.pgc"

	int i, j, c;

#define init() { \
	for (i = 0; i < 2; ++i) \
	{ \
		memset(recv_buf[i].arr, 0x0, sizeof(recv_buf[i].arr)); \
		recv_buf[i].len = 0; \
		ind[i] = 0; \
	} \
	recv_vlen_buf = NULL, \
	memset(recv_short_buf.arr, 0x0, sizeof(recv_short_buf.arr)); \
} \
while (0)

	ECPGdebug(1, stderr);

	for (i = 0; i < 2; ++i)
	{
		for (j = 0, c = 0xff; (c == -1 ? c = 0xff : 1), j < DATA_SIZE; ++j, --c)
			send_buf[i].arr[j] = c;

		send_buf[i].len = DATA_SIZE;
	}

    { ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 57 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 57 "bytea.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table if not exists test ( data1 bytea , data2 bytea )", ECPGt_EOIT, ECPGt_EORT);
#line 59 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 59 "bytea.pgc"


	{ ECPGprepare(__LINE__, NULL, 0, "ins_stmt", "insert into test values(?,?)");
#line 61 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 61 "bytea.pgc"

	{ ECPGprepare(__LINE__, NULL, 0, "sel_stmt", "select data1,data2 from test");
#line 62 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 62 "bytea.pgc"

	ECPGallocate_desc(__LINE__, "idesc");
#line 63 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 63 "bytea.pgc"

	ECPGallocate_desc(__LINE__, "odesc");
#line 64 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 64 "bytea.pgc"


	/* Test for static sql statement with normal host variable, indicator */
	init();
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 68 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 68 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( $1  , $2  )", 
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bytea,&(send_buf[1]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 69 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 69 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select data1 , data2 from test", ECPGt_EOIT, 
	ECPGt_bytea,&(recv_buf[0]),(long)DATA_SIZE,(long)1,sizeof(struct bytea_2), 
	ECPGt_int,&(ind[0]),(long)1,(long)1,sizeof(int), 
	ECPGt_bytea,&(recv_short_buf),(long)DATA_SIZE - LACK_SIZE,(long)1,sizeof(struct bytea_4), 
	ECPGt_int,&(ind[1]),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 70 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 70 "bytea.pgc"

	dump_binary(recv_buf[0].arr, recv_buf[0].len, ind[0]);
	dump_binary(recv_short_buf.arr, recv_short_buf.len, ind[1]);

	/* Test for cursor */
	init();
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 76 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 76 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( $1  , $2  )", 
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bytea,&(send_buf[1]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 77 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 77 "bytea.pgc"

	ECPGset_var( 0, &( send_buf[0] ), __LINE__);\
 /* declare cursor1 cursor for select data1 from test where data1 = $1  */
#line 78 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 78 "bytea.pgc"

#line 78 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cursor1 cursor for select data1 from test where data1 = $1 ", 
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 79 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 79 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch from cursor1", ECPGt_EOIT, 
	ECPGt_bytea,&(recv_buf[0]),(long)DATA_SIZE,(long)1,sizeof(struct bytea_2), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 80 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 80 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cursor1", ECPGt_EOIT, ECPGt_EORT);
#line 81 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 81 "bytea.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "cursor1");
#line 82 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 82 "bytea.pgc"

	dump_binary(recv_buf[0].arr, recv_buf[0].len, 0);

	/* Test for variable length array */
	init();
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 87 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 87 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( $1  , $2  )", 
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bytea,&(send_buf[1]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 88 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 88 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( $1  , $2  )", 
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bytea,&(send_buf[1]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 89 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 89 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select data1 from test", ECPGt_EOIT, 
	ECPGt_bytea,&(recv_vlen_buf),(long)DATA_SIZE,(long)0,sizeof(struct bytea_3), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 90 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 90 "bytea.pgc"

	dump_binary(recv_vlen_buf[0].arr, recv_vlen_buf[0].len, 0);
	dump_binary(recv_vlen_buf[1].arr, recv_vlen_buf[1].len, 0);
	free(recv_vlen_buf);

	/* Test for dynamic sql statement with normal host variable, indicator */
	init();
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 97 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 97 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "ins_stmt", 
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bytea,&(send_buf[1]),(long)512,(long)1,sizeof(struct bytea_1), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 98 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 98 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "sel_stmt", ECPGt_EOIT, 
	ECPGt_bytea,&(recv_buf[0]),(long)DATA_SIZE,(long)1,sizeof(struct bytea_2), 
	ECPGt_int,&(ind[0]),(long)1,(long)1,sizeof(int), 
	ECPGt_bytea,&(recv_short_buf),(long)DATA_SIZE - LACK_SIZE,(long)1,sizeof(struct bytea_4), 
	ECPGt_int,&(ind[1]),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 99 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 99 "bytea.pgc"

	dump_binary(recv_buf[0].arr, recv_buf[0].len, ind[0]);
	dump_binary(recv_short_buf.arr, recv_short_buf.len, ind[1]);

	/* Test for dynamic sql statement with sql descriptor */
	init();
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 105 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 105 "bytea.pgc"

	{ ECPGset_desc(__LINE__, "idesc", 1,ECPGd_data,
	ECPGt_bytea,&(send_buf[0]),(long)512,(long)1,sizeof(struct bytea_1), ECPGd_EODT);

#line 106 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 106 "bytea.pgc"

	{ ECPGset_desc(__LINE__, "idesc", 2,ECPGd_data,
	ECPGt_bytea,&(send_buf[1]),(long)512,(long)1,sizeof(struct bytea_1), ECPGd_EODT);

#line 107 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 107 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "ins_stmt", 
	ECPGt_descriptor, "idesc", 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 108 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 108 "bytea.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "sel_stmt", ECPGt_EOIT, 
	ECPGt_descriptor, "odesc", 1L, 1L, 1L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 109 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 109 "bytea.pgc"

	{ ECPGget_desc(__LINE__, "odesc", 1,ECPGd_indicator,
	ECPGt_int,&(ind[0]),(long)1,(long)1,sizeof(int), ECPGd_data,
	ECPGt_bytea,&(recv_buf[0]),(long)DATA_SIZE,(long)1,sizeof(struct bytea_2), ECPGd_EODT);

#line 110 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 110 "bytea.pgc"

	{ ECPGget_desc(__LINE__, "odesc", 2,ECPGd_indicator,
	ECPGt_int,&(ind[1]),(long)1,(long)1,sizeof(int), ECPGd_data,
	ECPGt_bytea,&(recv_short_buf),(long)DATA_SIZE - LACK_SIZE,(long)1,sizeof(struct bytea_4), ECPGd_EODT);

#line 111 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 111 "bytea.pgc"

	dump_binary(recv_buf[0].arr, recv_buf[0].len, ind[0]);
	dump_binary(recv_short_buf.arr, recv_short_buf.len, ind[1]);

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table test", ECPGt_EOIT, ECPGt_EORT);
#line 115 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 115 "bytea.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 116 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 116 "bytea.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 117 "bytea.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 117 "bytea.pgc"


	return 0;
}
