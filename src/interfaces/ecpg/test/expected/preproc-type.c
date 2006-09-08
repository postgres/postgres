/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "type.pgc"
#include <stdio.h>
#include <stdlib.h>


#line 1 "regression.h"






#line 4 "type.pgc"


typedef long  mmInteger ;

#line 6 "type.pgc"

#line 6 "type.pgc"

typedef char  mmChar ;

#line 7 "type.pgc"

#line 7 "type.pgc"

typedef short  mmSmallInt ;

#line 8 "type.pgc"

#line 8 "type.pgc"


/* exec sql type string is char [ 11 ]   */
#line 10 "type.pgc"

typedef char string[11];

/* exec sql type c is char  reference */
#line 13 "type.pgc"

typedef char* c;

/* exec sql begin declare section */
 

   
   
   

struct TBempl { 
#line 19 "type.pgc"
 mmInteger  idnum    ;
 
#line 20 "type.pgc"
 mmChar  name [ 21 ]    ;
 
#line 21 "type.pgc"
 mmSmallInt  accs    ;
 } ;/* exec sql end declare section */
#line 23 "type.pgc"


int
main (void)
{
  /* exec sql begin declare section */
    
   
     
   
  
  	 
	 
   
  
#line 29 "type.pgc"
 struct TBempl  empl    ;
 
#line 30 "type.pgc"
 string  str    ;
 
#line 31 "type.pgc"
 c  ptr   = NULL ;
 
#line 36 "type.pgc"
 struct varchar_vc { 
#line 34 "type.pgc"
 int  len    ;
 
#line 35 "type.pgc"
 char  text [ 10 ]    ;
 }  vc    ;
/* exec sql end declare section */
#line 37 "type.pgc"


  /* exec sql var vc is  [ 10 ]   */
#line 39 "type.pgc"

  ECPGdebug (1, stderr);

  empl.idnum = 1;
  { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); }
#line 43 "type.pgc"

  if (sqlca.sqlcode)
    {
      printf ("connect error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, "create  table empl ( idnum integer   , name char  ( 20 )    , accs smallint   , string1 char  ( 10 )    , string2 char  ( 10 )    , string3 char  ( 10 )    )    ", ECPGt_EOIT, ECPGt_EORT);}
#line 51 "type.pgc"

  if (sqlca.sqlcode)
    {
      printf ("create error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, "insert into empl values ( 1 , 'user name' , 320 , 'first str' , 'second str' , 'third str' ) ", ECPGt_EOIT, ECPGt_EORT);}
#line 58 "type.pgc"

  if (sqlca.sqlcode)
    {
      printf ("insert error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, "select  idnum , name , accs , string1 , string2 , string3  from empl where idnum =  ?  ", 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(empl.name),(long)21,(long)1,(21)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(empl.accs),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(str),(long)11,(long)1,(11)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(ptr),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_varchar,&(vc),(long)10,(long)1,sizeof(struct varchar_vc), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 68 "type.pgc"

  if (sqlca.sqlcode)
    {
      printf ("select error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }
  printf ("id=%ld name='%s' accs=%d str='%s' ptr='%s' vc='%10.10s'\n", empl.idnum, empl.name, empl.accs, str, ptr, vc.text);

  { ECPGdisconnect(__LINE__, "CURRENT");}
#line 76 "type.pgc"


  free(ptr);
  exit (0);
}
