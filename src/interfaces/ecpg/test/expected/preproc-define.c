/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "define.pgc"
#include <stdio.h>
#include <stdlib.h>


#line 1 "regression.h"






#line 4 "define.pgc"


typedef long  mmInteger ;

#line 6 "define.pgc"

#line 6 "define.pgc"

typedef char  mmChar ;

#line 7 "define.pgc"

#line 7 "define.pgc"

typedef short  mmSmallInt ;

#line 8 "define.pgc"

#line 8 "define.pgc"


/* exec sql begin declare section */
 

   
   
   

struct TBempl { 
#line 13 "define.pgc"
 mmInteger  idnum    ;
 
#line 14 "define.pgc"
 mmChar  name [ 21 ]    ;
 
#line 15 "define.pgc"
 mmSmallInt  accs    ;
 } ;/* exec sql end declare section */
#line 17 "define.pgc"


int
main (void)
{
  /* exec sql begin declare section */
    
  
#line 23 "define.pgc"
 struct TBempl  empl    ;
/* exec sql end declare section */
#line 24 "define.pgc"


  ECPGdebug (1, stderr);

  empl.idnum = 1;
  { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); }
#line 29 "define.pgc"

  if (sqlca.sqlcode)
    {
      printf ("connect error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, "create  table empl ( idnum integer   , name char  ( 20 )    , accs smallint   )    ", ECPGt_EOIT, ECPGt_EORT);}
#line 37 "define.pgc"

  if (sqlca.sqlcode)
    {
      printf ("create error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, "insert into empl values ( 1 , 'first user' , 320 ) ", ECPGt_EOIT, ECPGt_EORT);}
#line 44 "define.pgc"

  if (sqlca.sqlcode)
    {
      printf ("insert error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, "select  idnum , name , accs  from empl where idnum =  ?  ", 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(empl.name),(long)21,(long)1,(21)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(empl.accs),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 54 "define.pgc"

  if (sqlca.sqlcode)
    {
      printf ("select error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }
  printf ("id=%ld name=%s, accs=%d\n", empl.idnum, empl.name, empl.accs);

  { ECPGdisconnect(__LINE__, "CURRENT");}
#line 62 "define.pgc"

  exit (0);
}
