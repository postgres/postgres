/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "desc.pgc"

#line 1 "./../regression.h"






#line 1 "desc.pgc"

/* exec sql whenever sqlerror  sqlprint ; */
#line 2 "desc.pgc"


int
main(void)
{
	/* exec sql begin declare section */
	   
	   
	   

	   
	      
	      
	   
	
#line 8 "desc.pgc"
 char * stmt1   = "INSERT INTO test1 VALUES (?, ?)" ;
 
#line 9 "desc.pgc"
 char * stmt2   = "SELECT * from test1 where a = ? and b = ?" ;
 
#line 10 "desc.pgc"
 char * stmt3   = "SELECT * from test1 where a = ?" ;
 
#line 12 "desc.pgc"
 int  val1   = 1 ;
 
#line 13 "desc.pgc"
 char  val2 []   = "one" ,  val2output []   = "AAA" ;
 
#line 14 "desc.pgc"
 int  val1output   = 2 ,  val2i   = 0 ;
 
#line 15 "desc.pgc"
 int  val2null   = - 1 ;
/* exec sql end declare section */
#line 16 "desc.pgc"


	ECPGdebug(1, stderr);

	ECPGallocate_desc(__LINE__, "indesc");
#line 20 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 20 "desc.pgc"

	ECPGallocate_desc(__LINE__, "outdesc");
#line 21 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 21 "desc.pgc"


	{ ECPGset_desc(__LINE__, "indesc", 1,ECPGd_data,
	ECPGt_int,&(val1),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 23 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 23 "desc.pgc"

	{ ECPGset_desc(__LINE__, "indesc", 2,ECPGd_data,
	ECPGt_char,&(val2),(long)-1,(long)1,(-1)*sizeof(char), ECPGd_indicator,
	ECPGt_int,&(val2i),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 24 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 24 "desc.pgc"


	{ ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); 
#line 26 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "desc.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, "create  table test1 ( a int   , b text   )    ", ECPGt_EOIT, ECPGt_EORT);
#line 28 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 28 "desc.pgc"

	{ ECPGprepare(__LINE__, "foo1" , stmt1);
#line 29 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 29 "desc.pgc"

	{ ECPGprepare(__LINE__, "foo2" , stmt2);
#line 30 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 30 "desc.pgc"

	{ ECPGprepare(__LINE__, "foo3" , stmt3);
#line 31 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 31 "desc.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, "?", 
	ECPGt_char_variable,(ECPGprepared_statement("foo1")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_descriptor, "indesc", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 33 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 33 "desc.pgc"


	{ ECPGset_desc(__LINE__, "indesc", 1,ECPGd_data,
	ECPGt_const,"2",(long)1,(long)1,strlen("2"), ECPGd_EODT);

#line 35 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 35 "desc.pgc"

	{ ECPGset_desc(__LINE__, "indesc", 2,ECPGd_data,
	ECPGt_char,&(val2),(long)-1,(long)1,(-1)*sizeof(char), ECPGd_indicator,
	ECPGt_int,&(val2null),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 36 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 36 "desc.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, "?", 
	ECPGt_char_variable,(ECPGprepared_statement("foo1")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_descriptor, "indesc", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 38 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 38 "desc.pgc"


	{ ECPGset_desc(__LINE__, "indesc", 1,ECPGd_data,
	ECPGt_int,&(val1),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 40 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 40 "desc.pgc"

	{ ECPGset_desc(__LINE__, "indesc", 2,ECPGd_data,
	ECPGt_char,&(val2),(long)-1,(long)1,(-1)*sizeof(char), ECPGd_indicator,
	ECPGt_int,&(val2i),(long)1,(long)1,sizeof(int), ECPGd_EODT);

#line 41 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 41 "desc.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, "?", 
	ECPGt_char_variable,(ECPGprepared_statement("foo2")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_descriptor, "indesc", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_descriptor, "outdesc", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 43 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 43 "desc.pgc"


	{ ECPGget_desc(__LINE__, "outdesc", 1,ECPGd_data,
	ECPGt_char,&(val2output),(long)-1,(long)1,(-1)*sizeof(char), ECPGd_EODT);

#line 45 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 45 "desc.pgc"

	printf("output = %s\n", val2output);

	/* declare c1  cursor  for ? */
#line 48 "desc.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "declare c1  cursor  for ?", 
	ECPGt_char_variable,(ECPGprepared_statement("foo2")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_descriptor, "indesc", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 49 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 49 "desc.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, "fetch next from c1", ECPGt_EOIT, 
	ECPGt_int,&(val1output),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(val2output),(long)-1,(long)1,(-1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 51 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 51 "desc.pgc"

	printf("val1=%d val2=%s\n", val1output, val2output);

	{ ECPGdo(__LINE__, 0, 1, NULL, "close c1", ECPGt_EOIT, ECPGt_EORT);
#line 54 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 54 "desc.pgc"


	{ ECPGset_desc_header(__LINE__, "indesc", (int)(1));

#line 56 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 56 "desc.pgc"

	{ ECPGset_desc(__LINE__, "indesc", 1,ECPGd_data,
	ECPGt_const,"2",(long)1,(long)1,strlen("2"), ECPGd_EODT);

#line 57 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 57 "desc.pgc"


	/* declare c2  cursor  for ? */
#line 59 "desc.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "declare c2  cursor  for ?", 
	ECPGt_char_variable,(ECPGprepared_statement("foo3")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_descriptor, "indesc", 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 60 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 60 "desc.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, "fetch next from c2", ECPGt_EOIT, 
	ECPGt_int,&(val1output),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(val2output),(long)-1,(long)1,(-1)*sizeof(char), 
	ECPGt_int,&(val2i),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 62 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 62 "desc.pgc"

	printf("val1=%d val2=%s\n", val1output, val2i ? "null" : val2output);

	{ ECPGdo(__LINE__, 0, 1, NULL, "close c2", ECPGt_EOIT, ECPGt_EORT);
#line 65 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 65 "desc.pgc"


	{ ECPGdo(__LINE__, 0, 1, NULL, "select  *  from test1 where a = 2  ", ECPGt_EOIT, 
	ECPGt_int,&(val1output),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(val2output),(long)-1,(long)1,(-1)*sizeof(char), 
	ECPGt_int,&(val2i),(long)1,(long)1,sizeof(int), ECPGt_EORT);
#line 67 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 67 "desc.pgc"

	printf("val1=%d val2=%s\n", val1output, val2i ? "null" : val2output);

	{ ECPGdo(__LINE__, 0, 1, NULL, "drop table test1 ", ECPGt_EOIT, ECPGt_EORT);
#line 70 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 70 "desc.pgc"

	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 71 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 71 "desc.pgc"


	ECPGdeallocate_desc(__LINE__, "indesc");
#line 73 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 73 "desc.pgc"

	ECPGdeallocate_desc(__LINE__, "outdesc");
#line 74 "desc.pgc"

if (sqlca.sqlcode < 0) sqlprint();
#line 74 "desc.pgc"


	return 0;
}
