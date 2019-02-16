/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "declare.pgc"
#include <locale.h>
#include <string.h>
#include <stdlib.h>

/* exec sql whenever sqlerror  sqlprint ; */
#line 5 "declare.pgc"



#line 1 "sqlca.h"
#ifndef POSTGRES_SQLCA_H
#define POSTGRES_SQLCA_H

#ifndef PGDLLIMPORT
#if  defined(WIN32) || defined(__CYGWIN__)
#define PGDLLIMPORT __declspec (dllimport)
#else
#define PGDLLIMPORT
#endif							/* __CYGWIN__ */
#endif							/* PGDLLIMPORT */

#define SQLERRMC_LEN	150

#ifdef __cplusplus
extern "C"
{
#endif

struct sqlca_t
{
	char		sqlcaid[8];
	long		sqlabc;
	long		sqlcode;
	struct
	{
		int			sqlerrml;
		char		sqlerrmc[SQLERRMC_LEN];
	}			sqlerrm;
	char		sqlerrp[8];
	long		sqlerrd[6];
	/* Element 0: empty						*/
	/* 1: OID of processed tuple if applicable			*/
	/* 2: number of rows processed				*/
	/* after an INSERT, UPDATE or				*/
	/* DELETE statement					*/
	/* 3: empty						*/
	/* 4: empty						*/
	/* 5: empty						*/
	char		sqlwarn[8];
	/* Element 0: set to 'W' if at least one other is 'W'	*/
	/* 1: if 'W' at least one character string		*/
	/* value was truncated when it was			*/
	/* stored into a host variable.             */

	/*
	 * 2: if 'W' a (hopefully) non-fatal notice occurred
	 */	/* 3: empty */
	/* 4: empty						*/
	/* 5: empty						*/
	/* 6: empty						*/
	/* 7: empty						*/

	char		sqlstate[5];
};

struct sqlca_t *ECPGget_sqlca(void);

#ifndef POSTGRES_ECPG_INTERNAL
#define sqlca (*ECPGget_sqlca())
#endif

#ifdef __cplusplus
}
#endif

#endif

#line 7 "declare.pgc"


#line 1 "regression.h"






#line 8 "declare.pgc"


#define ARRAY_SZIE 20

void execute_test(void);
void commitTable(void);
void reset(void);
void printResult(char *tc_name, int loop);

/* exec sql begin declare section */
 
 
 

#line 18 "declare.pgc"
 int f1 [ ARRAY_SZIE ] ;
 
#line 19 "declare.pgc"
 int f2 [ ARRAY_SZIE ] ;
 
#line 20 "declare.pgc"
 char f3 [ ARRAY_SZIE ] [ 20 ] ;
/* exec sql end declare section */
#line 21 "declare.pgc"


int main(void)
{
    setlocale(LC_ALL, "C");

    ECPGdebug(1, stderr);

    { ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , "con1", 0); 
#line 29 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 29 "declare.pgc"

    { ECPGconnect(__LINE__, 0, "ecpg2_regression" , NULL, NULL , "con2", 0); 
#line 30 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 30 "declare.pgc"


    { ECPGdo(__LINE__, 0, 1, "con1", 0, ECPGst_normal, "create table source ( f1 integer , f2 integer , f3 varchar ( 20 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 32 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "declare.pgc"

    { ECPGdo(__LINE__, 0, 1, "con2", 0, ECPGst_normal, "create table source ( f1 integer , f2 integer , f3 varchar ( 20 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 33 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 33 "declare.pgc"


    { ECPGdo(__LINE__, 0, 1, "con1", 0, ECPGst_normal, "insert into source values ( 1 , 10 , 'db on con1' )", ECPGt_EOIT, ECPGt_EORT);
#line 35 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 35 "declare.pgc"

    { ECPGdo(__LINE__, 0, 1, "con1", 0, ECPGst_normal, "insert into source values ( 2 , 20 , 'db on con1' )", ECPGt_EOIT, ECPGt_EORT);
#line 36 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "declare.pgc"


    { ECPGdo(__LINE__, 0, 1, "con2", 0, ECPGst_normal, "insert into source values ( 1 , 10 , 'db on con2' )", ECPGt_EOIT, ECPGt_EORT);
#line 38 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 38 "declare.pgc"

    { ECPGdo(__LINE__, 0, 1, "con2", 0, ECPGst_normal, "insert into source values ( 2 , 20 , 'db on con2' )", ECPGt_EOIT, ECPGt_EORT);
#line 39 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 39 "declare.pgc"


    commitTable();

    execute_test();

    { ECPGdo(__LINE__, 0, 1, "con1", 0, ECPGst_normal, "drop table if exists source", ECPGt_EOIT, ECPGt_EORT);
#line 45 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 45 "declare.pgc"

    { ECPGdo(__LINE__, 0, 1, "con2", 0, ECPGst_normal, "drop table if exists source", ECPGt_EOIT, ECPGt_EORT);
#line 46 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 46 "declare.pgc"


    commitTable();

    { ECPGdisconnect(__LINE__, "ALL");
#line 50 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 50 "declare.pgc"


    return 0;
}

/*
 * default connection: con2
 * Non-default connection: con1
 *
 */
void execute_test(void)
{
    /* exec sql begin declare section */
     
       
    
#line 63 "declare.pgc"
 int i ;
 
#line 64 "declare.pgc"
 char * selectString = "SELECT f1,f2,f3 FROM source" ;
/* exec sql end declare section */
#line 65 "declare.pgc"


    /*
     * testcase1. using DECLARE STATEMENT without using AT clause,
     * using PREPARE and CURSOR statement without using AT clause
     */
    reset();

    { ECPGdeclare(__LINE__, NULL, "stmt_1");
#line 73 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 73 "declare.pgc"

    { ECPGprepare(__LINE__, NULL, 0, "stmt_1", selectString);
#line 74 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 74 "declare.pgc"

    /* declare cur_1 cursor for $1 */
#line 75 "declare.pgc"

    { ECPGopen("cur_1", "stmt_1", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cur_1 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt_1", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 76 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 76 "declare.pgc"


    /* exec sql whenever not found  break ; */
#line 78 "declare.pgc"

    i = 0;
    while (1)
    {
        { ECPGfetch("cur_1", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cur_1", ECPGt_EOIT, 
	ECPGt_int,&(f1[i]),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(f2[i]),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(f3[i]),(long)20,(long)1,(20)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 82 "declare.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 82 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 82 "declare.pgc"

        i++;
    }
    { ECPGclose("cur_1", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cur_1", ECPGt_EOIT, ECPGt_EORT);
#line 85 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 85 "declare.pgc"

    { ECPGdeallocate(__LINE__, 0, NULL, "stmt_1");
#line 86 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 86 "declare.pgc"

    /* exec sql whenever not found  continue ; */
#line 87 "declare.pgc"


    printResult("testcase1", 2);


    /*
     * testcase2. using DECLARE STATEMENT at con1,
     * using PREPARE and CURSOR statement without using AT clause
     */
    reset();

    { ECPGdeclare(__LINE__, "con1", "stmt_2");
#line 98 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 98 "declare.pgc"

    { ECPGprepare(__LINE__, NULL, 0, "stmt_2", selectString);
#line 99 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 99 "declare.pgc"

    /* declare cur_2 cursor for $1 */
#line 100 "declare.pgc"

    { ECPGopen("cur_2", "stmt_2", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare cur_2 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "stmt_2", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 101 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 101 "declare.pgc"


    /* exec sql whenever not found  break ; */
#line 103 "declare.pgc"

    i = 0;
    while (1)
    {
        { ECPGfetch("cur_2", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch cur_2", ECPGt_EOIT, 
	ECPGt_int,&(f1[i]),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(f2[i]),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(f3[i]),(long)20,(long)1,(20)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 107 "declare.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 107 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 107 "declare.pgc"

        i++;
    }
    { ECPGclose("cur_2", __LINE__, 0, 1, NULL, 0, ECPGst_normal, "close cur_2", ECPGt_EOIT, ECPGt_EORT);
#line 110 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 110 "declare.pgc"

    { ECPGdeallocate(__LINE__, 0, NULL, "stmt_2");
#line 111 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 111 "declare.pgc"

    /* exec sql whenever not found  continue ; */
#line 112 "declare.pgc"


    printResult("testcase2", 2);

    /*
     * testcase3. using DECLARE STATEMENT at con1,
     * using PREPARE and CURSOR statement at con2
     */
    reset();

    { ECPGdeclare(__LINE__, "con1", "stmt_3");
#line 122 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 122 "declare.pgc"

    { ECPGprepare(__LINE__, "con2", 0, "stmt_3", selectString);
#line 123 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 123 "declare.pgc"

    /* declare cur_3 cursor for $1 */
#line 124 "declare.pgc"

    { ECPGopen("cur_3", "stmt_3", __LINE__, 0, 1, "con2", 0, ECPGst_normal, "declare cur_3 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement("con2", "stmt_3", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 125 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 125 "declare.pgc"


    /* exec sql whenever not found  break ; */
#line 127 "declare.pgc"

    i = 0;
    while (1)
    {
        { ECPGfetch("cur_3", __LINE__, 0, 1, "con2", 0, ECPGst_normal, "fetch cur_3", ECPGt_EOIT, 
	ECPGt_int,&(f1[i]),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(f2[i]),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(f3[i]),(long)20,(long)1,(20)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 131 "declare.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 131 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 131 "declare.pgc"

        i++;
    }
    { ECPGclose("cur_3", __LINE__, 0, 1, "con2", 0, ECPGst_normal, "close cur_3", ECPGt_EOIT, ECPGt_EORT);
#line 134 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 134 "declare.pgc"

    { ECPGdeallocate(__LINE__, 0, "con2", "stmt_3");
#line 135 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 135 "declare.pgc"

    /* exec sql whenever not found  continue ; */
#line 136 "declare.pgc"


    printResult("testcase3", 2);


    /*
     * testcase4. using DECLARE STATEMENT without using AT clause,
     * using PREPARE and CURSOR statement at con2
     */
    reset();

    { ECPGdeclare(__LINE__, NULL, "stmt_4");
#line 147 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 147 "declare.pgc"

    { ECPGprepare(__LINE__, "con2", 0, "stmt_4", selectString);
#line 148 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 148 "declare.pgc"

    /* declare cur_4 cursor for $1 */
#line 149 "declare.pgc"

    { ECPGopen("cur_4", "stmt_4", __LINE__, 0, 1, "con2", 0, ECPGst_normal, "declare cur_4 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement("con2", "stmt_4", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 150 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 150 "declare.pgc"


    /* exec sql whenever not found  break ; */
#line 152 "declare.pgc"

    i = 0;
    while (1)
    {
        { ECPGfetch("cur_4", __LINE__, 0, 1, "con2", 0, ECPGst_normal, "fetch cur_4", ECPGt_EOIT, 
	ECPGt_int,&(f1[i]),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(f2[i]),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(f3[i]),(long)20,(long)1,(20)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 156 "declare.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 156 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 156 "declare.pgc"

        i++;
    }
    { ECPGclose("cur_4", __LINE__, 0, 1, "con2", 0, ECPGst_normal, "close cur_4", ECPGt_EOIT, ECPGt_EORT);
#line 159 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 159 "declare.pgc"

    { ECPGdeallocate(__LINE__, 0, "con2", "stmt_4");
#line 160 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 160 "declare.pgc"

    /* exec sql whenever not found  continue ; */
#line 161 "declare.pgc"


    printResult("testcase4", 2);

    /*
     * testcase5. using DECLARE STATEMENT without using AT clause,
     * using PREPARE and EXECUTE statement without using AT clause
     */
    reset();

    { ECPGdeclare(__LINE__, NULL, "stmt_5");
#line 171 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 171 "declare.pgc"

    { ECPGprepare(__LINE__, NULL, 0, "stmt_5", selectString);
#line 172 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 172 "declare.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_execute, "stmt_5", ECPGt_EOIT, 
	ECPGt_int,(f1),(long)1,(long)ARRAY_SZIE,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(f2),(long)1,(long)ARRAY_SZIE,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(f3),(long)20,(long)ARRAY_SZIE,(20)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 173 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 173 "declare.pgc"


    { ECPGdeallocate(__LINE__, 0, NULL, "stmt_5");
#line 175 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 175 "declare.pgc"


    printResult("testcase5", 2);
}

void commitTable()
{
    { ECPGtrans(__LINE__, "con1", "commit");
#line 182 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 182 "declare.pgc"

    { ECPGtrans(__LINE__, "con2", "commit");
#line 183 "declare.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 183 "declare.pgc"

}

/*
 * reset all the output variables
 */
void reset()
{
    memset(f1, 0, sizeof(f1));
    memset(f2, 0, sizeof(f2));
    memset(f3, 0, sizeof(f3));
}

void printResult(char *tc_name, int loop)
{
    int i;

    if (tc_name)
        printf("****%s test results:****\n", tc_name);

    for (i = 0; i < loop; i++)
        printf("f1=%d, f2=%d, f3=%s\n", f1[i], f2[i], f3[i]);

    printf("\n");
}
