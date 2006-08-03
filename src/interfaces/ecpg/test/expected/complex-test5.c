/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "test5.pgc"
#include <stdio.h>
#include <stdlib.h>


#line 1 "./../regression.h"






#line 4 "test5.pgc"


typedef long  mmInteger ;

#line 6 "test5.pgc"

#line 6 "test5.pgc"

typedef char  mmChar ;

#line 7 "test5.pgc"

#line 7 "test5.pgc"

typedef short  mmSmallInt ;

#line 8 "test5.pgc"

#line 8 "test5.pgc"


/* exec sql begin declare section */
 

   
   
   
   

struct TBempl { 
#line 13 "test5.pgc"
 mmInteger  idnum    ;
 
#line 14 "test5.pgc"
 mmChar  name [ 21 ]    ;
 
#line 15 "test5.pgc"
 mmSmallInt  accs    ;
 
#line 16 "test5.pgc"
 mmChar  byte [ 20 ]    ;
 } ;/* exec sql end declare section */
#line 18 "test5.pgc"


int
main (void)
{
  /* exec sql begin declare section */
    
     
   
  
	 
	 
   
  
#line 24 "test5.pgc"
 struct TBempl  empl    ;
 
#line 25 "test5.pgc"
 char * data   = "\\001\\155\\000\\212" ;
 
#line 30 "test5.pgc"
 union { 
#line 28 "test5.pgc"
 mmSmallInt  accs    ;
 
#line 29 "test5.pgc"
 char  t [ 2 ]    ;
 }  a    ;
/* exec sql end declare section */
#line 31 "test5.pgc"

  int i;

  ECPGdebug (1, stderr);

  empl.idnum = 1;
  { ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , NULL, 0); }
#line 37 "test5.pgc"

  if (sqlca.sqlcode)
    {
      printf ("connect error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, "create  table empl ( idnum integer   , name char  ( 20 )    , accs smallint   , byte bytea   )    ", ECPGt_EOIT, ECPGt_EORT);}
#line 45 "test5.pgc"

  if (sqlca.sqlcode)
    {
      printf ("create error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, "insert into empl values( 1 , 'first user' , 320 ,  ? )", 
	ECPGt_char,&(data),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);}
#line 52 "test5.pgc"

  if (sqlca.sqlcode)
    {
      printf ("insert error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, "select  name , accs , byte  from empl where idnum =  ?  ", 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_char,(empl.name),(long)21,(long)1,(21)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(empl.accs),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(empl.byte),(long)20,(long)1,(20)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 62 "test5.pgc"

  if (sqlca.sqlcode)
    {
      printf ("select error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }
  printf ("name=%s, accs=%d byte=%s\n", empl.name, empl.accs, empl.byte);

  /* declare C  cursor  for select  name , accs , byte  from empl where idnum =  ?   */
#line 70 "test5.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "declare C  cursor  for select  name , accs , byte  from empl where idnum =  ?  ", 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);}
#line 71 "test5.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "fetch C", ECPGt_EOIT, 
	ECPGt_char,(empl.name),(long)21,(long)1,(21)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(empl.accs),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(empl.byte),(long)20,(long)1,(20)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 72 "test5.pgc"

  if (sqlca.sqlcode)
    {
      printf ("fetch error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  printf ("name=%s, accs=%d byte=%s\n", empl.name, empl.accs, empl.byte);

  memset(empl.name, 0, 21L);
  memset(empl.byte, '#', 20L);
  /* declare B  binary cursor  for select  name , accs , byte  from empl where idnum =  ?   */
#line 83 "test5.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "declare B  binary cursor  for select  name , accs , byte  from empl where idnum =  ?  ", 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);}
#line 84 "test5.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, "fetch B", ECPGt_EOIT, 
	ECPGt_char,(empl.name),(long)21,(long)1,(21)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(a.accs),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(empl.byte),(long)20,(long)1,(20)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 85 "test5.pgc"

  if (sqlca.sqlcode)
    {
      printf ("fetch error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, "close B", ECPGt_EOIT, ECPGt_EORT);}
#line 92 "test5.pgc"


  printf ("name=%s, accs=%d byte=", empl.name, a.accs);
  for (i=0; i<20; i++)
  {
	if (empl.byte[i] == '#')
		break;
	printf("(%o)", (unsigned char)empl.byte[i]);
  }
  printf("\n");
  { ECPGdisconnect(__LINE__, "CURRENT");}
#line 102 "test5.pgc"

  exit (0);
}
