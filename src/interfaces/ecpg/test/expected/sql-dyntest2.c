/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "dyntest2.pgc"
/* dynamic SQL test program
 */

#include <stdio.h>
#include <stdlib.h>


#line 1 "./../../include/sql3types.h"
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

#line 7 "dyntest2.pgc"


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

#line 8 "dyntest2.pgc"


#line 1 "./../regression.h"






#line 9 "dyntest2.pgc"


static void error(void)
{
   printf("\n#%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);
   exit(1);
}

int main(int argc,char **argv)
{
/* exec sql begin declare section */
   
    
   
   
   
   
   
   
   
   

#line 20 "dyntest2.pgc"
 int  COUNT    ;
 
#line 21 "dyntest2.pgc"
 int  INTVAR    ,  BOOLVAR    ;
 
#line 22 "dyntest2.pgc"
 int  INDEX    ;
 
#line 23 "dyntest2.pgc"
 int  INDICATOR    ;
 
#line 24 "dyntest2.pgc"
 int  TYPE    ,  LENGTH    ,  OCTET_LENGTH    ,  PRECISION    ,  SCALE    ,  RETURNED_OCTET_LENGTH    ;
 
#line 25 "dyntest2.pgc"
 int  DATETIME_INTERVAL_CODE    ;
 
#line 26 "dyntest2.pgc"
 char  NAME [ 120 ]    ;
 
#line 27 "dyntest2.pgc"
 char  STRINGVAR [ 1024 ]    ;
 
#line 28 "dyntest2.pgc"
 double  DOUBLEVAR    ;
 
#line 29 "dyntest2.pgc"
 char * QUERY    ;
/* exec sql end declare section */
#line 30 "dyntest2.pgc"

  int done=0;

  /* exec sql var BOOLVAR is bool   */
#line 33 "dyntest2.pgc"


  ECPGdebug(1, stderr);

  QUERY="select * from dyntest";

  /* exec sql whenever sqlerror  do error (  ) ; */
#line 39 "dyntest2.pgc"


  ECPGallocate_desc(__LINE__, "MYDESC");
#line 41 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );
#line 41 "dyntest2.pgc"


  { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); 
#line 43 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 43 "dyntest2.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "create  table dyntest ( name char  ( 14 )    , d float8   , i int   , bignumber int8   , b boolean   , comment text   , day date   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 45 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 45 "dyntest2.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "insert into dyntest values( 'first entry' , 14.7 , 14 , 123045607890 , true , 'The world''s most advanced open source database.' , '1987-07-14' )", ECPGt_EOIT, ECPGt_EORT);
#line 46 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 46 "dyntest2.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "insert into dyntest values( 'second entry' , 1407.87 , 1407 , 987065403210 , false , 'The elephant never forgets.' , '1999-11-5' )", ECPGt_EOIT, ECPGt_EORT);
#line 47 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 47 "dyntest2.pgc"


  { ECPGprepare(__LINE__, "MYQUERY" , QUERY);
#line 49 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 49 "dyntest2.pgc"

  /* declare MYCURS  cursor  for ? */
#line 50 "dyntest2.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "declare MYCURS  cursor  for ?", 
	ECPGt_char_variable,(ECPGprepared_statement("MYQUERY")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 52 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 52 "dyntest2.pgc"


  while (1)
  {
     { ECPGdo(__LINE__, 0, 1, NULL, "fetch in MYCURS", ECPGt_EOIT, 
	ECPGt_descriptor, "MYDESC", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 56 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 56 "dyntest2.pgc"


     if (sqlca.sqlcode) break;

     { ECPGget_desc_header(__LINE__, "MYDESC", &(COUNT));

#line 60 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 60 "dyntest2.pgc"

     if (!done)
     {
        printf("Count %d\n",COUNT);
        done=1;
     }

     for (INDEX=1;INDEX<=COUNT;++INDEX)
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

#line 74 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 74 "dyntest2.pgc"

     	printf("%2d\t%s (type: %d length: %d precision: %d scale: %d\n"
		"\toctet_length: %d returned_octet_length: %d)\n\t= "
     			,INDEX,NAME,TYPE,LENGTH,PRECISION,SCALE
     			,OCTET_LENGTH,RETURNED_OCTET_LENGTH);
     	if (INDICATOR==-1) printf("NULL\n");
        else switch (TYPE)
     	{
	  case SQL3_BOOLEAN:
     	        { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_bool,&(BOOLVAR),(long)1,(long)1,sizeof(bool), ECPGd_EODT);

#line 83 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 83 "dyntest2.pgc"

     		printf("%s\n",BOOLVAR ? "true":"false");
     		break;
     	   case SQL3_INTEGER:
     	   case SQL3_SMALLINT:
     	        { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_int,&(INTVAR),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 88 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 88 "dyntest2.pgc"

     	        printf("%d\n",INTVAR);
     	        break;
     	   case SQL3_DOUBLE_PRECISION:
     	        { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_double,&(DOUBLEVAR),(long)1,(long)1,sizeof(double), ECPGd_EODT);

#line 92 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 92 "dyntest2.pgc"

     	        printf("%.*f\n",PRECISION,DOUBLEVAR);
     	        break;
     	   case SQL3_DATE_TIME_TIMESTAMP:
     	   	{ ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_char,(STRINGVAR),(long)1024,(long)1,(1024)*sizeof(char), ECPGd_di_code,
	ECPGt_int,&(DATETIME_INTERVAL_CODE),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 98 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 98 "dyntest2.pgc"

     	        printf("%d \"%s\"\n",DATETIME_INTERVAL_CODE,STRINGVAR);
     	        break;
     	   case SQL3_CHARACTER:
     	   case SQL3_CHARACTER_VARYING:
     	        { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_char,(STRINGVAR),(long)1024,(long)1,(1024)*sizeof(char), ECPGd_EODT);

#line 103 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 103 "dyntest2.pgc"

     	        printf("\"%s\"\n",STRINGVAR);
     	        break;
     	   default:
     	        { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_char,(STRINGVAR),(long)1024,(long)1,(1024)*sizeof(char), ECPGd_EODT);

#line 107 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 107 "dyntest2.pgc"

     	        printf("<\"%s\">\n",STRINGVAR);
     	        break;
     	}
     }
  }

  { ECPGdo(__LINE__, 0, 1, NULL, "close MYCURS", ECPGt_EOIT, ECPGt_EORT);
#line 114 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 114 "dyntest2.pgc"


  ECPGdeallocate_desc(__LINE__, "MYDESC");
#line 116 "dyntest2.pgc"

if (sqlca.sqlcode < 0) error (  );
#line 116 "dyntest2.pgc"


  return 0;
}
