/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "dynalloc.pgc"
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

#line 2 "dynalloc.pgc"

#include <stdlib.h>

#line 1 "regression.h"






#line 4 "dynalloc.pgc"


int main(void)
{
   /* exec sql begin declare section */
        
     
    
    
    
    
    
/*   char **d8=0; */
    
    
    
    
    
    
    
    
/*   int *i8=0; */
    
   
#line 9 "dynalloc.pgc"
 int * d1 = 0 ;
 
#line 10 "dynalloc.pgc"
 double * d2 = 0 ;
 
#line 11 "dynalloc.pgc"
 char ** d3 = 0 ;
 
#line 12 "dynalloc.pgc"
 char ** d4 = 0 ;
 
#line 13 "dynalloc.pgc"
 char ** d5 = 0 ;
 
#line 14 "dynalloc.pgc"
 char ** d6 = 0 ;
 
#line 15 "dynalloc.pgc"
 char ** d7 = 0 ;
 
#line 17 "dynalloc.pgc"
 char ** d9 = 0 ;
 
#line 18 "dynalloc.pgc"
 int * i1 = 0 ;
 
#line 19 "dynalloc.pgc"
 int * i2 = 0 ;
 
#line 20 "dynalloc.pgc"
 int * i3 = 0 ;
 
#line 21 "dynalloc.pgc"
 int * i4 = 0 ;
 
#line 22 "dynalloc.pgc"
 int * i5 = 0 ;
 
#line 23 "dynalloc.pgc"
 int * i6 = 0 ;
 
#line 24 "dynalloc.pgc"
 int * i7 = 0 ;
 
#line 26 "dynalloc.pgc"
 int * i9 = 0 ;
/* exec sql end declare section */
#line 27 "dynalloc.pgc"

   int i;

   ECPGdebug(1, stderr);

   /* exec sql whenever sqlerror  do sqlprint ( ) ; */
#line 32 "dynalloc.pgc"

   { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); 
#line 33 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 33 "dynalloc.pgc"


   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set datestyle to mdy", ECPGt_EOIT, ECPGt_EORT);
#line 35 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 35 "dynalloc.pgc"


   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table test ( a serial , b numeric ( 12 , 3 ) , c varchar , d varchar ( 3 ) , e char ( 4 ) , f timestamptz , g boolean , h box , i inet )", ECPGt_EOIT, ECPGt_EORT);
#line 37 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 37 "dynalloc.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test ( b , c , d , e , f , g , h , i ) values ( 23.456 , 'varchar' , 'v' , 'c' , '2003-03-03 12:33:07 PDT' , true , '(1,2,3,4)' , '2001:4f8:3:ba:2e0:81ff:fe22:d1f1/128' )", ECPGt_EOIT, ECPGt_EORT);
#line 38 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 38 "dynalloc.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into test ( b , c , d , e , f , g , h , i ) values ( 2.446456 , null , 'v' , 'c' , '2003-03-03 12:33:07 PDT' , false , null , null )", ECPGt_EOIT, ECPGt_EORT);
#line 39 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 39 "dynalloc.pgc"


   ECPGallocate_desc(__LINE__, "mydesc");
#line 41 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );
#line 41 "dynalloc.pgc"

   { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select a , b , c , d , e , f , g , h , i from test order by a", ECPGt_EOIT, 
	ECPGt_descriptor, "mydesc", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 42 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 42 "dynalloc.pgc"

   { ECPGget_desc(__LINE__, "mydesc", 1,ECPGd_indicator,
	ECPGt_int,&(i1),(long)1,(long)0,sizeof(int), ECPGd_data,
	ECPGt_int,&(d1),(long)1,(long)0,sizeof(int), ECPGd_EODT);

#line 43 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 43 "dynalloc.pgc"

   { ECPGget_desc(__LINE__, "mydesc", 2,ECPGd_indicator,
	ECPGt_int,&(i2),(long)1,(long)0,sizeof(int), ECPGd_data,
	ECPGt_double,&(d2),(long)1,(long)0,sizeof(double), ECPGd_EODT);

#line 44 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 44 "dynalloc.pgc"

   { ECPGget_desc(__LINE__, "mydesc", 3,ECPGd_indicator,
	ECPGt_int,&(i3),(long)1,(long)0,sizeof(int), ECPGd_data,
	ECPGt_char,&(d3),(long)0,(long)0,(1)*sizeof(char), ECPGd_EODT);

#line 45 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 45 "dynalloc.pgc"

   { ECPGget_desc(__LINE__, "mydesc", 4,ECPGd_indicator,
	ECPGt_int,&(i4),(long)1,(long)0,sizeof(int), ECPGd_data,
	ECPGt_char,&(d4),(long)0,(long)0,(1)*sizeof(char), ECPGd_EODT);

#line 46 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 46 "dynalloc.pgc"

   { ECPGget_desc(__LINE__, "mydesc", 5,ECPGd_indicator,
	ECPGt_int,&(i5),(long)1,(long)0,sizeof(int), ECPGd_data,
	ECPGt_char,&(d5),(long)0,(long)0,(1)*sizeof(char), ECPGd_EODT);

#line 47 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 47 "dynalloc.pgc"

   { ECPGget_desc(__LINE__, "mydesc", 6,ECPGd_indicator,
	ECPGt_int,&(i6),(long)1,(long)0,sizeof(int), ECPGd_data,
	ECPGt_char,&(d6),(long)0,(long)0,(1)*sizeof(char), ECPGd_EODT);

#line 48 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 48 "dynalloc.pgc"

   { ECPGget_desc(__LINE__, "mydesc", 7,ECPGd_indicator,
	ECPGt_int,&(i7),(long)1,(long)0,sizeof(int), ECPGd_data,
	ECPGt_char,&(d7),(long)0,(long)0,(1)*sizeof(char), ECPGd_EODT);

#line 49 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 49 "dynalloc.pgc"

   /* skip box for now */
   /* exec sql get descriptor mydesc value 8 :d8=DATA, :i8=INDICATOR; */
   { ECPGget_desc(__LINE__, "mydesc", 9,ECPGd_indicator,
	ECPGt_int,&(i9),(long)1,(long)0,sizeof(int), ECPGd_data,
	ECPGt_char,&(d9),(long)0,(long)0,(1)*sizeof(char), ECPGd_EODT);

#line 52 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 52 "dynalloc.pgc"


   printf("Result:\n");
   for (i=0;i<sqlca.sqlerrd[2];++i)
   {
      if (i1[i]) printf("NULL, ");
      else printf("%d, ",d1[i]); 

      if (i2[i]) printf("NULL, ");
      else printf("%f, ",d2[i]); 

      if (i3[i]) printf("NULL, ");
      else printf("'%s', ",d3[i]); 

      if (i4[i]) printf("NULL, ");
      else printf("'%s', ",d4[i]); 

      if (i5[i]) printf("NULL, ");
      else printf("'%s', ",d5[i]); 

      if (i6[i]) printf("NULL, ");
      else printf("'%s', ",d6[i]); 

      if (i7[i]) printf("NULL, ");
      else printf("'%s', ",d7[i]); 

      if (i9[i]) printf("NULL, ");
      else printf("'%s', ",d9[i]); 

      printf("\n");
   }
   ECPGfree_auto_mem();
   printf("\n");

   ECPGdeallocate_desc(__LINE__, "mydesc");
#line 86 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );
#line 86 "dynalloc.pgc"

   { ECPGdisconnect(__LINE__, "CURRENT");
#line 87 "dynalloc.pgc"

if (sqlca.sqlcode < 0) sqlprint ( );}
#line 87 "dynalloc.pgc"

   return 0;
}
