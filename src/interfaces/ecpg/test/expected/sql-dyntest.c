/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "dyntest.pgc"
/* dynamic SQL test program
 */

#include <stdio.h>
#include <stdlib.h>


#line 1 "sql3types.h"
#ifndef _ECPG_SQL3TYPES_H
#define _ECPG_SQL3TYPES_H

/* SQL3 dynamic type codes */

/* chapter 13.1 table 2: Codes used for SQL data types in Dynamic SQL */

enum
{
	SQL3_CHARACTER = 1,
	SQL3_NUMERIC,
	SQL3_DECIMAL,
	SQL3_INTEGER,
	SQL3_SMALLINT,
	SQL3_FLOAT,
	SQL3_REAL,
	SQL3_DOUBLE_PRECISION,
	SQL3_DATE_TIME_TIMESTAMP,
	SQL3_INTERVAL,				/* 10 */
	SQL3_CHARACTER_VARYING = 12,
	SQL3_ENUMERATED,
	SQL3_BIT,
	SQL3_BIT_VARYING,
	SQL3_BOOLEAN,
	SQL3_abstract
	/* the rest is xLOB stuff */
};

/* chapter 13.1 table 3: Codes associated with datetime data types in Dynamic SQL */

enum
{
	SQL3_DDT_DATE = 1,
	SQL3_DDT_TIME,
	SQL3_DDT_TIMESTAMP,
	SQL3_DDT_TIME_WITH_TIME_ZONE,
	SQL3_DDT_TIMESTAMP_WITH_TIME_ZONE,

	SQL3_DDT_ILLEGAL			/* not a datetime data type (not part of
								 * standard) */
};

#endif   /* !_ECPG_SQL3TYPES_H */

#line 7 "dyntest.pgc"


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

#line 8 "dyntest.pgc"


#line 1 "regression.h"






#line 9 "dyntest.pgc"


static void
error (void)
{
  printf ("\n#%ld:%s\n", sqlca.sqlcode, sqlca.sqlerrm.sqlerrmc);
  exit (1);
}

int
main (int argc, char **argv)
{
  /* exec sql begin declare section */
   
   
   
   
        
   
    
   
   
   
  
#line 22 "dyntest.pgc"
 int  COUNT    ;
 
#line 23 "dyntest.pgc"
 int  INTVAR    ;
 
#line 24 "dyntest.pgc"
 int  INDEX    ;
 
#line 25 "dyntest.pgc"
 int  INDICATOR    ;
 
#line 26 "dyntest.pgc"
 int  TYPE    ,  LENGTH    ,  OCTET_LENGTH    ,  PRECISION    ,  SCALE    ,  RETURNED_OCTET_LENGTH    ;
 
#line 27 "dyntest.pgc"
 int  DATETIME_INTERVAL_CODE    ;
 
#line 28 "dyntest.pgc"
 char  NAME [ 120 ]    ,  BOOLVAR    ;
 
#line 29 "dyntest.pgc"
 char  STRINGVAR [ 1024 ]    ;
 
#line 30 "dyntest.pgc"
 double  DOUBLEVAR    ;
 
#line 31 "dyntest.pgc"
 char * QUERY    ;
/* exec sql end declare section */
#line 32 "dyntest.pgc"

  int done = 0;

  /* exec sql var BOOLVAR is bool   */
#line 35 "dyntest.pgc"


  ECPGdebug (1, stderr);

  QUERY = "select * from dyntest";

  /* exec sql whenever sqlerror  do error (  ) ; */
#line 43 "dyntest.pgc"


  ECPGallocate_desc(__LINE__, "MYDESC");
#line 45 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );
#line 45 "dyntest.pgc"


  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); 
#line 47 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 47 "dyntest.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set datestyle to german", ECPGt_EOIT, ECPGt_EORT);
#line 49 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 49 "dyntest.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create  table dyntest ( name char  ( 14 )    , d float8    , i int   , bignumber int8    , b boolean   , comment text    , day date    )    ", ECPGt_EOIT, ECPGt_EORT);
#line 53 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 53 "dyntest.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into dyntest values ( 'first entry' , 14.7 , 14 , 123045607890 , true , 'The world''s most advanced open source database.' , '1987-07-14' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 54 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 54 "dyntest.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into dyntest values ( 'second entry' , 1407.87 , 1407 , 987065403210 , false , 'The elephant never forgets.' , '1999-11-5' ) ", ECPGt_EOIT, ECPGt_EORT);
#line 55 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 55 "dyntest.pgc"


  { ECPGprepare(__LINE__, NULL, 0, "myquery", QUERY);
#line 57 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 57 "dyntest.pgc"

  /* declare MYCURS  cursor  for $1 */
#line 58 "dyntest.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare MYCURS  cursor  for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "myquery", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 60 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 60 "dyntest.pgc"


  while (1)
    {
      { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch in MYCURS", ECPGt_EOIT, 
	ECPGt_descriptor, "MYDESC", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 64 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 64 "dyntest.pgc"


      if (sqlca.sqlcode)
	break;

      { ECPGget_desc_header(__LINE__, "MYDESC", &(COUNT));

#line 69 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 69 "dyntest.pgc"

      if (!done)
	{
	  printf ("Found %d columns\n", COUNT);
	  done = 1;
	}

      for (INDEX = 1; INDEX <= COUNT; ++INDEX)
	{
	{ ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_indicator,
	ECPGt_int,&(INDICATOR),(long)1,(long)1,sizeof(int), ECPGd_name,
	ECPGt_char,(NAME),(long)120,(long)1,(120)*sizeof(char), ECPGd_scale,
	ECPGt_int,&(SCALE),(long)1,(long)1,sizeof(int), ECPGd_precision,
	ECPGt_int,&(PRECISION),(long)1,(long)1,sizeof(int), ECPGd_ret_octet,
	ECPGt_int,&(RETURNED_OCTET_LENGTH),(long)1,(long)1,sizeof(int), ECPGd_octet,
	ECPGt_int,&(OCTET_LENGTH),(long)1,(long)1,sizeof(int), ECPGd_length,
	ECPGt_int,&(LENGTH),(long)1,(long)1,sizeof(int), ECPGd_type,
	ECPGt_int,&(TYPE),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 86 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 86 "dyntest.pgc"

	  printf ("%2d\t%s (type: %d length: %d precision: %d scale: %d = " , INDEX, NAME, TYPE, LENGTH, PRECISION, SCALE);
	  switch (TYPE)
	    {
	    case SQL3_BOOLEAN:
	      printf ("bool");
	      break;
	    case SQL3_NUMERIC:
	      printf ("numeric(%d,%d)", PRECISION, SCALE);
	      break;
	    case SQL3_DECIMAL:
	      printf ("decimal(%d,%d)", PRECISION, SCALE);
	      break;
	    case SQL3_INTEGER:
	      printf ("integer");
	      break;
	    case SQL3_SMALLINT:
	      printf ("smallint");
	      break;
	    case SQL3_FLOAT:
	      printf ("float(%d,%d)", PRECISION, SCALE);
	      break;
	    case SQL3_REAL:
	      printf ("real");
	      break;
	    case SQL3_DOUBLE_PRECISION:
	      printf ("double precision");
	      break;
	    case SQL3_DATE_TIME_TIMESTAMP:
	    { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_di_code,
	ECPGt_int,&(DATETIME_INTERVAL_CODE),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 116 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 116 "dyntest.pgc"

	      switch (DATETIME_INTERVAL_CODE)
		{
		case SQL3_DDT_DATE:
		  printf ("date");
		  break;
		case SQL3_DDT_TIME:
		  printf ("time");
		  break;
		case SQL3_DDT_TIMESTAMP:
		  printf ("timestamp");
		  break;
		case SQL3_DDT_TIME_WITH_TIME_ZONE:
		  printf ("time with time zone");
		  break;
		case SQL3_DDT_TIMESTAMP_WITH_TIME_ZONE:
		  printf ("timestamp with time zone");
		  break;
		}
	      break;
	    case SQL3_INTERVAL:
	      printf ("interval");
	      break;
	    case SQL3_CHARACTER:
	      if (LENGTH > 0)
		printf ("char(%d)", LENGTH);
	      else
		printf ("text");
	      break;
	    case SQL3_CHARACTER_VARYING:
	      if (LENGTH > 0)
		printf ("varchar(%d)", LENGTH);
	      else
		printf ("varchar()");
	      break;
	    default:
	      if (TYPE < 0)
		printf ("<OID %d>", -TYPE);
	      else
		printf ("<SQL3 %d>", TYPE);
	      break;
	    }
	  printf (")\n\toctet_length: %d returned_octet_length: %d)\n\t= ",
		  OCTET_LENGTH, RETURNED_OCTET_LENGTH);
	  if (INDICATOR == -1)
	    printf ("NULL\n");
	  else
	    switch (TYPE)
	      {
	      case SQL3_BOOLEAN:
	      { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_bool,&(BOOLVAR),(long)1,(long)1,sizeof(bool), ECPGd_EODT);

#line 166 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 166 "dyntest.pgc"

		printf ("%s\n", BOOLVAR ? "true" : "false");
		break;
	      case SQL3_INTEGER:
	      case SQL3_SMALLINT:
	      { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_int,&(INTVAR),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 171 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 171 "dyntest.pgc"

		printf ("%d\n", INTVAR);
		break;
	      case SQL3_DOUBLE_PRECISION:
	      { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_double,&(DOUBLEVAR),(long)1,(long)1,sizeof(double), ECPGd_EODT);

#line 175 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 175 "dyntest.pgc"

		printf ("%.*f\n", PRECISION, DOUBLEVAR);
		break;
	      case SQL3_DATE_TIME_TIMESTAMP:
	      { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_char,(STRINGVAR),(long)1024,(long)1,(1024)*sizeof(char), ECPGd_di_code,
	ECPGt_int,&(DATETIME_INTERVAL_CODE),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 181 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 181 "dyntest.pgc"

		printf ("%d \"%s\"\n", DATETIME_INTERVAL_CODE, STRINGVAR);
		break;
	      case SQL3_CHARACTER:
	      case SQL3_CHARACTER_VARYING:
	      { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_char,(STRINGVAR),(long)1024,(long)1,(1024)*sizeof(char), ECPGd_EODT);

#line 186 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 186 "dyntest.pgc"

		printf ("\"%s\"\n", STRINGVAR);
		break;
	      default:
	      { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_char,(STRINGVAR),(long)1024,(long)1,(1024)*sizeof(char), ECPGd_EODT);

#line 190 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 190 "dyntest.pgc"

		printf ("<\"%s\">\n", STRINGVAR);
		break;
	      }
	}
    }

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close MYCURS", ECPGt_EOIT, ECPGt_EORT);
#line 197 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 197 "dyntest.pgc"


  ECPGdeallocate_desc(__LINE__, "MYDESC");
#line 199 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );
#line 199 "dyntest.pgc"


  return 0;
  }
