/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "prepareas.pgc"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


#line 1 "regression.h"






#line 5 "prepareas.pgc"

/* exec sql whenever sqlerror  sqlprint ; */
#line 6 "prepareas.pgc"


static void
check_result_of_insert(void)
{
	/* exec sql begin declare section */
	      
	
#line 12 "prepareas.pgc"
 int ivar1 = 0 , ivar2 = 0 ;
/* exec sql end declare section */
#line 13 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select c1 , c2 from test", ECPGt_EOIT, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 15 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 15 "prepareas.pgc"

	printf("%d %d\n", ivar1, ivar2);
}

int main(void)
{
	/* exec sql begin declare section */
	      
	    
	
#line 22 "prepareas.pgc"
 int ivar1 = 1 , ivar2 = 2 ;
 
#line 23 "prepareas.pgc"
 char v_include_dq_name [ 16 ] , v_include_ws_name [ 16 ] , v_normal_name [ 16 ] , v_query [ 64 ] ;
/* exec sql end declare section */
#line 24 "prepareas.pgc"


	strcpy(v_normal_name, "normal_name");
	strcpy(v_include_dq_name, "include_\"_name");
	strcpy(v_include_ws_name, "include_ _name");
	strcpy(v_query, "insert into test values(?,?)");

	/*
	 * preparing for test
	 */
	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 34 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 34 "prepareas.pgc"

	{ ECPGtrans(__LINE__, NULL, "begin");
#line 35 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 35 "prepareas.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table test ( c1 int , c2 int )", ECPGt_EOIT, ECPGt_EORT);
#line 36 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "prepareas.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit work");
#line 37 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 37 "prepareas.pgc"

	{ ECPGtrans(__LINE__, NULL, "begin");
#line 38 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 38 "prepareas.pgc"


	/*
	 * Non dynamic statement
	 */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 43 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 43 "prepareas.pgc"

	printf("+++++ Test for prepnormal +++++\n");
	printf("insert into test values(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test values ( $1  , $2  )", 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 46 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 46 "prepareas.pgc"

	check_result_of_insert();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 49 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 49 "prepareas.pgc"

	printf("+++++ Test for execute immediate +++++\n");
	printf("execute immediate \"insert into test values(1,2)\"\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_immediate, "insert into test values(1,2)", ECPGt_EOIT, ECPGt_EORT);
#line 52 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 52 "prepareas.pgc"

	check_result_of_insert();

	/*
	 * PREPARE FROM
	 */
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 58 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 58 "prepareas.pgc"

	printf("+++++ Test for PREPARE ident FROM CString +++++\n");
	printf("prepare ident_name from \"insert into test values(?,?)\"\n");
	{ ECPGprepare(__LINE__, NULL, 0, "ident_name", "insert into test values(?,?)");
#line 61 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 61 "prepareas.pgc"

	printf("execute ident_name using :ivar1,:ivar2\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "ident_name", 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 63 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 63 "prepareas.pgc"

	check_result_of_insert();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 66 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 66 "prepareas.pgc"

	printf("+++++ Test for PREPARE char_variable_normal_name FROM char_variable +++++\n");
	printf("prepare :v_normal_name from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, v_normal_name, v_query);
#line 69 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 69 "prepareas.pgc"

	printf("execute :v_normal_name using :ivar1,:ivar2\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, v_normal_name, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 71 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 71 "prepareas.pgc"

	check_result_of_insert();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 74 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 74 "prepareas.pgc"

	printf("+++++ Test for PREPARE char_variable_inc_dq_name FROM char_variable +++++\n");
	printf("prepare :v_include_dq_name from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, v_include_dq_name, v_query);
#line 77 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 77 "prepareas.pgc"

	printf("execute :v_include_dq_name using :ivar1,:ivar2\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, v_include_dq_name, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 79 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 79 "prepareas.pgc"

	check_result_of_insert();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 82 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 82 "prepareas.pgc"

	printf("+++++ Test for PREPARE char_variable_inc_ws_name FROM char_variable +++++\n");
	printf("prepare :v_include_ws_name from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, v_include_ws_name, v_query);
#line 85 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 85 "prepareas.pgc"

	printf("execute :v_include_ws_name using :ivar1,:ivar2\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, v_include_ws_name, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 87 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 87 "prepareas.pgc"

	check_result_of_insert();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 90 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 90 "prepareas.pgc"

	printf("+++++ Test for PREPARE CString_inc_ws_name FROM char_variable +++++\n");
	printf("prepare \"include_ _name\" from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, "include_ _name", v_query);
#line 93 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 93 "prepareas.pgc"

	printf("exec sql execute \"include_ _name\" using :ivar1,:ivar2\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "include_ _name", 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 95 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 95 "prepareas.pgc"

	check_result_of_insert();

	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 98 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 98 "prepareas.pgc"

	printf("+++++ Test for PREPARE CString_normal_name FROM char_variable +++++\n");
	printf("prepare \"norma_name\" from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, "normal_name", v_query);
#line 101 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 101 "prepareas.pgc"

	printf("exec sql execute \"normal_name\" using :ivar1,:ivar2\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "normal_name", 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 103 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 103 "prepareas.pgc"

	check_result_of_insert();

	/*
	 * PREPARE AS
	 */
	{ ECPGdeallocate(__LINE__, 0, NULL, "ident_name");
#line 109 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 109 "prepareas.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "normal_name");
#line 110 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 110 "prepareas.pgc"

	{ ECPGdeallocate(__LINE__, 0, NULL, "include_ _name");
#line 111 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 111 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 113 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 113 "prepareas.pgc"

	printf("+++++ Test for PREPARE ident(typelist) AS +++++\n");
	printf("prepare ident_name(int,int) as insert into test values($1,$2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( int , int ) as insert into test values ( $1 , $2 )", 
	ECPGt_const,"ident_name",(long)10,(long)1,strlen("ident_name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 116 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 116 "prepareas.pgc"

	printf("execute ident_name(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"ident_name",(long)10,(long)1,strlen("ident_name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 118 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 118 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "ident_name");
#line 120 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 120 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 122 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 122 "prepareas.pgc"

	printf("+++++ Test for PREPARE CString_normal_name(typelist) AS +++++\n");
	printf("prepare \"normal_name\"(int,int) as insert into test values($1,$2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( int , int ) as insert into test values ( $1 , $2 )", 
	ECPGt_const,"normal_name",(long)11,(long)1,strlen("normal_name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 125 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 125 "prepareas.pgc"

	printf("execute \"normal_name\"(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"normal_name",(long)11,(long)1,strlen("normal_name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 127 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 127 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "normal_name");
#line 129 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 129 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 131 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 131 "prepareas.pgc"

	printf("+++++ Test for PREPARE CString_include_ws_name(typelist) AS +++++\n");
	printf("prepare \"include_ _name\"(int,int) as insert into test values($1,$2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( int , int ) as insert into test values ( $1 , $2 )", 
	ECPGt_const,"include_ _name",(long)14,(long)1,strlen("include_ _name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 134 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 134 "prepareas.pgc"

	printf("execute \"include_ _name\"(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"include_ _name",(long)14,(long)1,strlen("include_ _name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 136 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 136 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "include_ _name");
#line 138 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 138 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 140 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 140 "prepareas.pgc"

	printf("+++++ Test for PREPARE char_variable_normal_name(typelist) AS +++++\n");
	printf("prepare :v_normal_name(int,int) as insert into test values($1,$2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( int , int ) as insert into test values ( $1 , $2 )", 
	ECPGt_char,(v_normal_name),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 143 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 143 "prepareas.pgc"

	printf("execute :v_normal_name(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_char,(v_normal_name),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 145 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 145 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "normal_name");
#line 147 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 147 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 149 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 149 "prepareas.pgc"

	printf("+++++ Test for PREPARE char_variable_include_ws_name(typelist) AS +++++\n");
	printf("prepare :v_include_ws_name(int,int) as insert into test values($1,$2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_prepare, "prepare $0 ( int , int ) as insert into test values ( $1 , $2 )", 
	ECPGt_char,(v_include_ws_name),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 152 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 152 "prepareas.pgc"

	printf("execute :v_include_ws_name(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_char,(v_include_ws_name),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 154 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 154 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "include_ _name");
#line 156 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 156 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 158 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 158 "prepareas.pgc"

	printf("+++++ Test for EXECUTE :v_normal_name(const,const) +++++\n");
	printf("prepare :v_normal_name from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, v_normal_name, v_query);
#line 161 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 161 "prepareas.pgc"

	printf("execute :v_normal_name(1,2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( 1 , 2 )", 
	ECPGt_char,(v_normal_name),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 163 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 163 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "normal_name");
#line 165 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 165 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 167 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 167 "prepareas.pgc"

	printf("+++++ Test for EXECUTE :v_normal_name(expr,expr) +++++\n");
	printf("prepare :v_normal_name from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, v_normal_name, v_query);
#line 170 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 170 "prepareas.pgc"

	printf("execute :v_normal_name(0+1,1+1)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( 0 + 1 , 1 + 1 )", 
	ECPGt_char,(v_normal_name),(long)16,(long)1,(16)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 172 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 172 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "normal_name");
#line 174 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 174 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 176 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 176 "prepareas.pgc"

	printf("+++++ Test for combination PREPARE FROM and EXECUTE ident(typelist) +++++\n");
	printf("prepare ident_name from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, "ident_name", v_query);
#line 179 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 179 "prepareas.pgc"

	printf("execute ident_name(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"ident_name",(long)10,(long)1,strlen("ident_name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 181 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 181 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "ident_name");
#line 183 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 183 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "truncate test", ECPGt_EOIT, ECPGt_EORT);
#line 185 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 185 "prepareas.pgc"

	printf("+++++ Test for combination PREPARE FROM and EXECUTE CString_include_ws_name(typelist) +++++\n");
	printf("prepare \"include_ _name\" from :v_query\n");
	{ ECPGprepare(__LINE__, NULL, 0, "include_ _name", v_query);
#line 188 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 188 "prepareas.pgc"

	printf("execute \"include_ _name\"(:ivar1,:ivar2)\n");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_exec_with_exprlist, "execute $0 ( $1  , $2  )", 
	ECPGt_const,"include_ _name",(long)14,(long)1,strlen("include_ _name"), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar1),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(ivar2),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 190 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 190 "prepareas.pgc"

	check_result_of_insert();
	{ ECPGdeallocate(__LINE__, 0, NULL, "include_ _name");
#line 192 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 192 "prepareas.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table test", ECPGt_EOIT, ECPGt_EORT);
#line 194 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 194 "prepareas.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit work");
#line 195 "prepareas.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 195 "prepareas.pgc"


	return 0;
}
