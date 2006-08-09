/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "dyntest.pgc"
/* dynamic SQL test program
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


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

#line 8 "dyntest.pgc"


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

#line 9 "dyntest.pgc"


#line 1 "./../regression.h"






#line 10 "dyntest.pgc"


static void error(void)
{  printf("#%ld:%s\n",sqlca.sqlcode,sqlca.sqlerrm.sqlerrmc);
   exit(1);
}

int main(int argc,char **argv)
{ /* exec sql begin declare section */
   
   
   
   
   
   
   
   
   
   
   
   
  
#line 19 "dyntest.pgc"
 int  COUNT    ;
 
#line 20 "dyntest.pgc"
 int  INTVAR    ;
 
#line 21 "dyntest.pgc"
 int  INDEX    ;
 
#line 22 "dyntest.pgc"
 int  INDICATOR    ;
 
#line 23 "dyntest.pgc"
 bool  BOOLVAR    ;
 
#line 24 "dyntest.pgc"
 int  TYPE    ,  LENGTH    ,  OCTET_LENGTH    ,  PRECISION    ,  SCALE    ,  RETURNED_OCTET_LENGTH    ;
 
#line 25 "dyntest.pgc"
 int  DATETIME_INTERVAL_CODE    ;
 
#line 26 "dyntest.pgc"
 char  NAME [ 120 ]    ;
 
#line 27 "dyntest.pgc"
 char  STRINGVAR [ 1024 ]    ;
 
#line 28 "dyntest.pgc"
 float  FLOATVAR    ;
 
#line 29 "dyntest.pgc"
 double  DOUBLEVAR    ;
 
#line 30 "dyntest.pgc"
 char * QUERY    ;
/* exec sql end declare section */
#line 31 "dyntest.pgc"

  int done=0;

  ECPGdebug(1, stderr);

  QUERY="select rulename, ev_class, ev_attr, ev_type, is_instead, ev_qual from pg_rewrite";

  /* exec sql whenever sqlerror  do error (  ) ; */
#line 38 "dyntest.pgc"


  ECPGallocate_desc(__LINE__, "MYDESC");
#line 40 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );
#line 40 "dyntest.pgc"


  { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); 
#line 42 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 42 "dyntest.pgc"


  { ECPGprepare(__LINE__, "MYQUERY" , QUERY);
#line 44 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 44 "dyntest.pgc"

  /* declare MYCURS  cursor  for ? */
#line 45 "dyntest.pgc"


  { ECPGdo(__LINE__, 0, 1, NULL, "declare MYCURS  cursor  for ?", 
	ECPGt_char_variable,(ECPGprepared_statement("MYQUERY")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 47 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 47 "dyntest.pgc"


  while (1)
  {
     { ECPGdo(__LINE__, 0, 1, NULL, "fetch in MYCURS", ECPGt_EOIT, 
	ECPGt_descriptor, "MYDESC", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 51 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 51 "dyntest.pgc"


     if (sqlca.sqlcode) break;

     { ECPGget_desc_header(__LINE__, "MYDESC", &(COUNT));

#line 55 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 55 "dyntest.pgc"

     if (!done)
     {
        printf("%d Columns\n",COUNT);
        for (INDEX=1;INDEX<=COUNT;++INDEX)
        {
		{ ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_ret_octet,
	ECPGt_int,&(RETURNED_OCTET_LENGTH),(long)1,(long)1,sizeof(int), ECPGd_name,
	ECPGt_char,(NAME),(long)120,(long)1,(120)*sizeof(char), ECPGd_scale,
	ECPGt_int,&(SCALE),(long)1,(long)1,sizeof(int), ECPGd_precision,
	ECPGt_int,&(PRECISION),(long)1,(long)1,sizeof(int), ECPGd_octet,
	ECPGt_int,&(OCTET_LENGTH),(long)1,(long)1,sizeof(int), ECPGd_length,
	ECPGt_int,&(LENGTH),(long)1,(long)1,sizeof(int), ECPGd_type,
	ECPGt_int,&(TYPE),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 66 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 66 "dyntest.pgc"

			printf("%s ",NAME);
        	switch (TYPE)
        	{  case SQL3_BOOLEAN:
        			printf("bool ");
        			break;
        	   case SQL3_NUMERIC:
        	   		printf("numeric(%d,%d) ",PRECISION,SCALE);
        	        break;
        	   case SQL3_DECIMAL:
        	   		printf("decimal(%d,%d) ",PRECISION,SCALE);
        	        break;
        	   case SQL3_INTEGER:
        	   		printf("integer ");
        	   		break;
        	   case SQL3_SMALLINT:
        	   		printf("smallint ");
        	        break;
        	   case SQL3_FLOAT:
        	   		printf("float(%d,%d) ",PRECISION,SCALE);
        	        break;
        	   case SQL3_REAL:
        	   		printf("real ");
        	        break;
        	   case SQL3_DOUBLE_PRECISION:
        	   		printf("double precision ");
        	        break;
        	   case SQL3_DATE_TIME_TIMESTAMP:
        	   		{ ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_di_code,
	ECPGt_int,&(DATETIME_INTERVAL_CODE),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 95 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 95 "dyntest.pgc"

        	   		switch(DATETIME_INTERVAL_CODE)
        	   		{	case SQL3_DDT_DATE:
        	   				printf("date "); break;
        	   			case SQL3_DDT_TIME:
        	   				printf("time "); break;
        	   			case SQL3_DDT_TIMESTAMP:
        	   				printf("timestamp "); break;
        	   			case SQL3_DDT_TIME_WITH_TIME_ZONE:
        	   				printf("time with time zone "); break;
        	   			case SQL3_DDT_TIMESTAMP_WITH_TIME_ZONE:
        	   				printf("timestamp with time zone "); break;
        	   		}
        	        break;
        	   case SQL3_INTERVAL:
        	   		printf("interval ");
        	        break;
        	   case SQL3_CHARACTER:
        	        if (LENGTH>0) printf("char(%d) ",LENGTH);
        	        else printf("char(?) ");
        	        break;
        	   case SQL3_CHARACTER_VARYING:
        	        if (LENGTH>0) printf("varchar(%d) ",LENGTH);
        	        else printf("varchar() ");
        	        break;
        	   default:
        	        if (TYPE<0) printf("<OID %d> ",-TYPE);
        	        else printf("<SQL3 %d> ",TYPE);
        	        break;
        	}
        	if (OCTET_LENGTH>0) printf("[%d bytes]",OCTET_LENGTH);
        	putchar('\n');
        }
        putchar('\n');
        done=1;
     }

     for (INDEX=1;INDEX<=COUNT;++INDEX)
     {
     	{ ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_indicator,
	ECPGt_int,&(INDICATOR),(long)1,(long)1,sizeof(int), ECPGd_precision,
	ECPGt_int,&(PRECISION),(long)1,(long)1,sizeof(int), ECPGd_scale,
	ECPGt_int,&(SCALE),(long)1,(long)1,sizeof(int), ECPGd_type,
	ECPGt_int,&(TYPE),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 136 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 136 "dyntest.pgc"

     	if (INDICATOR==-1) printf("NULL");
        else switch (TYPE)
     	{	case SQL3_BOOLEAN:
     	        { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_bool,&(BOOLVAR),(long)1,(long)1,sizeof(bool), ECPGd_EODT);

#line 140 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 140 "dyntest.pgc"

     			printf(BOOLVAR?"true":"false");
     			break;
     	   	case SQL3_NUMERIC:
     	   	case SQL3_DECIMAL:
     	        if (SCALE==0) /* we might even print leading zeros "%0*d" */
     	        {  { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_int,&(INTVAR),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 146 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 146 "dyntest.pgc"

     	           printf("%*d",PRECISION,INTVAR);
     	        }
     	        else
     	        {  { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_float,&(FLOATVAR),(long)1,(long)1,sizeof(float), ECPGd_EODT);

#line 150 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 150 "dyntest.pgc"

     	           printf("%*.*f",PRECISION+1,SCALE,FLOATVAR);
     	        }
     	        break;
     	   	case SQL3_INTEGER:
     	   	case SQL3_SMALLINT:
     	        { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_int,&(INTVAR),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 156 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 156 "dyntest.pgc"

     	        printf("%d",INTVAR);
     	        break;
     	   	case SQL3_FLOAT:
     	   	case SQL3_REAL:
     	        { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_float,&(FLOATVAR),(long)1,(long)1,sizeof(float), ECPGd_EODT);

#line 161 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 161 "dyntest.pgc"

     	        printf("%f",FLOATVAR);
     	        break;
     	   	case SQL3_DOUBLE_PRECISION:
     	        { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_double,&(DOUBLEVAR),(long)1,(long)1,sizeof(double), ECPGd_EODT);

#line 165 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 165 "dyntest.pgc"

     	        printf("%f",DOUBLEVAR);
     	        break;
     	   	case SQL3_DATE_TIME_TIMESTAMP:
     	   	case SQL3_INTERVAL:
     	   	case SQL3_CHARACTER:
     	   	case SQL3_CHARACTER_VARYING:
     	   	default:
     	        { ECPGget_desc(__LINE__, "MYDESC", INDEX,ECPGd_data,
	ECPGt_char,(STRINGVAR),(long)1024,(long)1,(1024)*sizeof(char), ECPGd_EODT);

#line 173 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 173 "dyntest.pgc"

     	        printf("'%s'",STRINGVAR);
     	        break;
     	}
     	putchar('|');
     }
     putchar('\n');
  }

  { ECPGdo(__LINE__, 0, 1, NULL, "close MYCURS", ECPGt_EOIT, ECPGt_EORT);
#line 182 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );}
#line 182 "dyntest.pgc"

  ECPGdeallocate_desc(__LINE__, "MYDESC");
#line 183 "dyntest.pgc"

if (sqlca.sqlcode < 0) error (  );
#line 183 "dyntest.pgc"


  /* no exec sql disconnect; */

  return 0;
}
