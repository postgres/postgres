/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "sqljson.pgc"
#include <stdio.h>


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

#line 3 "sqljson.pgc"


#line 1 "regression.h"






#line 4 "sqljson.pgc"


/* exec sql whenever sqlerror  sqlprint ; */
#line 6 "sqljson.pgc"


int
main ()
{
/* exec sql begin declare section */
   
   

#line 12 "sqljson.pgc"
 char json [ 1024 ] ;
 
#line 13 "sqljson.pgc"
 bool is_json [ 8 ] ;
/* exec sql end declare section */
#line 14 "sqljson.pgc"


  ECPGdebug (1, stderr);

  { ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 18 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 18 "sqljson.pgc"

  { ECPGsetcommit(__LINE__, "on", NULL);
#line 19 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 19 "sqljson.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_object ( returning text )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 21 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 21 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_object ( returning text format json )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 24 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 24 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_array ( returning jsonb )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 27 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 27 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_array ( returning jsonb format json )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 30 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 30 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_object ( 1 : 1 , '1' : null with unique )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 33 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 33 "sqljson.pgc"

  // error

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_object ( 1 : 1 , '2' : null , 1 : '2' absent on null without unique keys )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 36 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_object ( 1 : 1 , '2' : null absent on null without unique returning jsonb )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 39 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 39 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json ( null )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 42 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 42 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json ( '{ \"a\" : 1 } ' format json )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 45 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 45 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json ( '{ \"a\" : 1 } ' format json encoding UTF8 )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 48 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 48 "sqljson.pgc"

  // error

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json ( '   1   ' :: jsonb )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 51 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 51 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json ( '   1   ' :: json with unique keys ) into json", ECPGt_EOIT, ECPGt_EORT);
#line 54 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 54 "sqljson.pgc"

  // error

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json ( '{\"a\": 1, \"a\": 2}' )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 57 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 57 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json ( '{\"a\": 1, \"a\": 2}' with unique keys )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 60 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 60 "sqljson.pgc"

  // error

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_scalar ( null )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 63 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 63 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_scalar ( null :: int )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 66 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 66 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_scalar ( 123.45 )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 69 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 69 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_scalar ( true )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 72 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 72 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_scalar ( ' 123.45' )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 75 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 75 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_scalar ( '2020-06-07 01:02:03' :: timestamp )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 78 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 78 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_scalar ( '{}' :: jsonb )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 81 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 81 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_serialize ( null )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 84 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 84 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_serialize ( json ( '{ \"a\" : 1 } ' ) )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 87 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 87 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_serialize ( '{ \"a\" : 1 } ' )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 90 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 90 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_serialize ( '1' format json )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 93 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 93 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_serialize ( '{ \"a\" : 1 } ' returning varchar )", ECPGt_EOIT, 
	ECPGt_char,(json),(long)1024,(long)1,(1024)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 96 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 96 "sqljson.pgc"

  printf("Found json=%s\n", json);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select json_serialize ( '{ \"a\" : 1 } ' returning jsonb )", ECPGt_EOIT, ECPGt_EORT);
#line 99 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 99 "sqljson.pgc"

  // error

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "with val ( js ) as ( values ( '{ \"a\": 1, \"b\": [{ \"a\": 1, \"b\": 0, \"a\": 2 }] }' ) ) select js is json \"IS JSON\" , js is not json \"IS NOT JSON\" , js is json value \"IS VALUE\" , js is json object \"IS OBJECT\" , js is json array \"IS ARRAY\" , js is json scalar \"IS SCALAR\" , js is json without unique keys \"WITHOUT UNIQUE\" , js is json with unique keys \"WITH UNIQUE\" from val", ECPGt_EOIT, 
	ECPGt_bool,&(is_json[0]),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(is_json[1]),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(is_json[2]),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(is_json[3]),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(is_json[4]),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(is_json[5]),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(is_json[6]),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_bool,&(is_json[7]),(long)1,(long)1,sizeof(bool), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 114 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 114 "sqljson.pgc"

	  for (int i = 0; i < sizeof(is_json); i++)
		  printf("Found is_json[%d]: %s\n", i, is_json[i] ? "true" : "false");

  { ECPGdisconnect(__LINE__, "CURRENT");
#line 118 "sqljson.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 118 "sqljson.pgc"


  return 0;
}
