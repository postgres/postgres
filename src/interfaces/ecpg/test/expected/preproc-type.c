/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

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

typedef short  access ;

#line 8 "type.pgc"

#line 8 "type.pgc"
	/* matches an unreserved SQL keyword */
typedef access  access_renamed ;

#line 9 "type.pgc"

#line 9 "type.pgc"


/* exec sql type string is char [ 11 ] */
#line 11 "type.pgc"

typedef char string[11];

/* exec sql type c is char reference */
#line 14 "type.pgc"

typedef char* c;

/* exec sql begin declare section */
 

   
   
   

struct TBempl { 
#line 20 "type.pgc"
 mmInteger idnum ;
 
#line 21 "type.pgc"
 mmChar name [ 21 ] ;
 
#line 22 "type.pgc"
 access accs ;
 } ;/* exec sql end declare section */
#line 24 "type.pgc"


int
main (void)
{
  /* exec sql begin declare section */
    
   
     
     
   
  
	 
	 
   
  
#line 30 "type.pgc"
 struct TBempl empl ;
 
#line 31 "type.pgc"
 string str ;
 
#line 32 "type.pgc"
 access accs_val = 320 ;
 
#line 33 "type.pgc"
 c ptr = NULL ;
 
#line 38 "type.pgc"
 struct varchar { 
#line 36 "type.pgc"
 int len ;
 
#line 37 "type.pgc"
 char text [ 10 ] ;
 } vc ;
/* exec sql end declare section */
#line 39 "type.pgc"


  /* exec sql var vc is [ 10 ] */
#line 41 "type.pgc"

  ECPGdebug (1, stderr);

  empl.idnum = 1;
  { ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); }
#line 45 "type.pgc"

  if (sqlca.sqlcode)
    {
      printf ("connect error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table empl ( idnum integer , name char ( 20 ) , accs smallint , string1 char ( 10 ) , string2 char ( 10 ) , string3 char ( 10 ) )", ECPGt_EOIT, ECPGt_EORT);}
#line 53 "type.pgc"

  if (sqlca.sqlcode)
    {
      printf ("create error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into empl values ( 1 , 'user name' , $1  , 'first str' , 'second str' , 'third str' )", 
	ECPGt_short,&(accs_val),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);}
#line 60 "type.pgc"

  if (sqlca.sqlcode)
    {
      printf ("insert error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select idnum , name , accs , string1 , string2 , string3 from empl where idnum = $1 ", 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof( struct TBempl ), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(empl.name),(long)21,(long)1,sizeof( struct TBempl ), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(empl.accs),(long)1,(long)1,sizeof( struct TBempl ), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(str),(long)11,(long)1,(11)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(ptr),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_varchar,&(vc),(long)10,(long)1,sizeof(struct varchar), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 70 "type.pgc"

  if (sqlca.sqlcode)
    {
      printf ("select error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }
  printf ("id=%ld name='%s' accs=%d str='%s' ptr='%s' vc='%10.10s'\n", empl.idnum, empl.name, empl.accs, str, ptr, vc.text);

  { ECPGdisconnect(__LINE__, "CURRENT");}
#line 78 "type.pgc"


  free(ptr);
  exit (0);
}
