/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "init.pgc"

#line 1 "./../../include/sqlca.h"
#ifndef POSTGRES_SQLCA_H
#define POSTGRES_SQLCA_H

#ifndef DLLIMPORT
#if  defined(WIN32) || defined(__CYGWIN__)
#define DLLIMPORT __declspec (dllimport)
#else
#define DLLIMPORT
#endif   /* __CYGWIN__ */
#endif   /* DLLIMPORT */

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

#line 1 "init.pgc"


enum e { ENUM0, ENUM1 };
struct sa { int member; };

int
fa(void)
{
	printf("in fa\n");
	return 2;
}

int
fb(int x)
{
	printf("in fb (%d)\n", x);
	return x;
}

int
fc(const char *x)
{
	printf("in fc (%s)\n", x);
	return *x;
}

int fd(const char *x,int i)
{
	printf("in fd (%s, %d)\n", x, i);
	return (*x)*i;
}

int fe(enum e x)
{
	printf("in fe (%d)\n", (int) x);
	return (int)x;
}

void sqlnotice(char *notice, short trans)
{
	if (!notice)
		notice = "-empty-";
	printf("in sqlnotice (%s, %d)\n", notice, trans);
}



#define YES 1

#ifdef _cplusplus
namespace N
{
	static const int i=2;
};
#endif

int main(void)
{
	struct sa x,*y;
	/* exec sql begin declare section */
		 
		 
		 
		 
		 
		 
		 

		 
		 
		  
		  /* = 1L */ 
		   /* = 40000000000LL */ 
	
#line 61 "init.pgc"
 int  a   = ( int ) 2 ;
 
#line 62 "init.pgc"
 int  b   = 2 + 2 ;
 
#line 63 "init.pgc"
 int  b2   = ( 14 * 7 ) ;
 
#line 64 "init.pgc"
 int  d   = x . member ;
 
#line 65 "init.pgc"
 int  g   = fb ( 2 ) ;
 
#line 66 "init.pgc"
 int  i   = 3 ^ 1 ;
 
#line 67 "init.pgc"
 int  j   = 1 ? 1 : 2 ;
 
#line 69 "init.pgc"
 int  e   = y -> member ;
 
#line 70 "init.pgc"
 int  c   = 10 >> 2 ;
 
#line 71 "init.pgc"
 bool  h   = 2 || 1 ;
 
#line 72 "init.pgc"
 long  iay    ;
 
#line 73 "init.pgc"
 long long  iax    ;
/* exec sql end declare section */
#line 74 "init.pgc"


	int f=fa();

#ifdef _cplusplus
	/* exec sql begin declare section */
	  /* compile error */
	
#line 80 "init.pgc"
 int  k   = N : : i ;
/* exec sql end declare section */
#line 81 "init.pgc"

#endif

	ECPGdebug(1, stderr);

	/* exec sql whenever sqlerror  do fa (  ) ; */
#line 86 "init.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "select  now ()     ", ECPGt_EOIT, ECPGt_EORT);
#line 87 "init.pgc"

if (sqlca.sqlcode < 0) fa (  );}
#line 87 "init.pgc"

	/* exec sql whenever sqlerror  do fb ( 20 ) ; */
#line 88 "init.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "select  now ()     ", ECPGt_EOIT, ECPGt_EORT);
#line 89 "init.pgc"

if (sqlca.sqlcode < 0) fb ( 20 );}
#line 89 "init.pgc"

	/* exec sql whenever sqlerror  do fc ( \"50\" ) ; */
#line 90 "init.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "select  now ()     ", ECPGt_EOIT, ECPGt_EORT);
#line 91 "init.pgc"

if (sqlca.sqlcode < 0) fc ( "50" );}
#line 91 "init.pgc"

	/* exec sql whenever sqlerror  do fd ( \"50\" , 1 ) ; */
#line 92 "init.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "select  now ()     ", ECPGt_EOIT, ECPGt_EORT);
#line 93 "init.pgc"

if (sqlca.sqlcode < 0) fd ( "50" , 1 );}
#line 93 "init.pgc"

	/* exec sql whenever sqlerror  do fe ( ENUM0 ) ; */
#line 94 "init.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "select  now ()     ", ECPGt_EOIT, ECPGt_EORT);
#line 95 "init.pgc"

if (sqlca.sqlcode < 0) fe ( ENUM0 );}
#line 95 "init.pgc"

	/* exec sql whenever sqlerror  do sqlnotice ( NULL , 0 ) ; */
#line 96 "init.pgc"
 
	{ ECPGdo(__LINE__, 0, 1, NULL, "select  now ()     ", ECPGt_EOIT, ECPGt_EORT);
#line 97 "init.pgc"

if (sqlca.sqlcode < 0) sqlnotice ( NULL , 0 );}
#line 97 "init.pgc"

	return 0;
}
