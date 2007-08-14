/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "copystdout.pgc"
#include <stdio.h>


#line 1 "sqlca.h"
#ifndef POSTGRES_SQLCA_H
#define POSTGRES_SQLCA_H

#ifndef PGDLLIMPORT
#if  defined(WIN32) || defined(__CYGWIN__)
#define PGDLLIMPORT __declspec (dllimport)
#else
#define PGDLLIMPORT
#endif   /* __CYGWIN__ */
#endif   /* PGDLLIMPORT */

#define SQLERRMC_LEN	150

#ifdef __cplusplus
extern		"C"
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
	/* stored into a host variable.				*/

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

#line 3 "copystdout.pgc"


#line 1 "regression.h"






#line 4 "copystdout.pgc"


/* exec sql whenever sqlerror  sqlprint ; */
#line 6 "copystdout.pgc"


int
main ()
{
/*
  EXEC SQL BEGIN DECLARE SECTION;
  char *fname = "/tmp/foo";
  EXEC SQL END DECLARE SECTION;
*/

  ECPGdebug (1, stderr);

  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); 
#line 19 "copystdout.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 19 "copystdout.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create  table foo ( a int   , b varchar    )    ", ECPGt_EOIT, ECPGt_EORT);
#line 20 "copystdout.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 20 "copystdout.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into foo values ( 5 , 'abc' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 21 "copystdout.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 21 "copystdout.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into foo values ( 6 , 'def' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 22 "copystdout.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 22 "copystdout.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into foo values ( 7 , 'ghi' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 23 "copystdout.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 23 "copystdout.pgc"


  /* produces expected file "/tmp/foo" */
  /* EXEC SQL COPY foo TO:fname WITH DELIMITER ','; */
  /* printf ("copy to /tmp/foo : sqlca.sqlcode = %ld", sqlca.sqlcode); */

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "copy  foo  to stdout  with  delimiter  ','", ECPGt_EOIT, ECPGt_EORT);
#line 29 "copystdout.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 29 "copystdout.pgc"

  printf ("copy to STDOUT : sqlca.sqlcode = %ld\n", sqlca.sqlcode);

  { ECPGdisconnect(__LINE__, "CURRENT");
#line 32 "copystdout.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "copystdout.pgc"


  return 0;
}
