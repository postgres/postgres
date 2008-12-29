/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "binary.pgc"
#include <stdio.h>
#include <stdlib.h>


#line 1 "regression.h"






#line 4 "binary.pgc"


/* exec sql begin declare section */
 

   
   
   
   

struct TBempl { 
#line 9 "binary.pgc"
 long idnum ;
 
#line 10 "binary.pgc"
 char name [ 21 ] ;
 
#line 11 "binary.pgc"
 short accs ;
 
#line 12 "binary.pgc"
 char byte [ 20 ] ;
 } ;/* exec sql end declare section */
#line 14 "binary.pgc"


int
main (void)
{
  /* exec sql begin declare section */
    
     
  
#line 20 "binary.pgc"
 struct TBempl empl ;
 
#line 21 "binary.pgc"
 char * data = "\\001\\155\\000\\212" ;
/* exec sql end declare section */
#line 22 "binary.pgc"

  int i;

  ECPGdebug (1, stderr);

  empl.idnum = 1;
  { ECPGconnect(__LINE__, 0, "regress1" , NULL, NULL , NULL, 0); }
#line 28 "binary.pgc"

  if (sqlca.sqlcode)
    {
      printf ("connect error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table empl ( idnum integer , name char ( 20 ) , accs smallint , byte bytea )", ECPGt_EOIT, ECPGt_EORT);}
#line 36 "binary.pgc"

  if (sqlca.sqlcode)
    {
      printf ("create error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into empl values ( 1 , 'first user' , 320 , $1  )", 
	ECPGt_char,&(data),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);}
#line 43 "binary.pgc"

  if (sqlca.sqlcode)
    {
      printf ("insert error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  /* declare C cursor for select name , accs , byte from empl where idnum = $1  */
#line 50 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare C cursor for select name , accs , byte from empl where idnum = $1 ", 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);}
#line 51 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch C", ECPGt_EOIT, 
	ECPGt_char,(empl.name),(long)21,(long)1,(21)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(empl.accs),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(empl.byte),(long)20,(long)1,(20)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 52 "binary.pgc"

  if (sqlca.sqlcode)
    {
      printf ("fetch error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  printf ("name=%s, accs=%d byte=%s\n", empl.name, empl.accs, empl.byte);

  memset(empl.name, 0, 21L);
  memset(empl.byte, '#', 20L);
  /* declare B binary cursor for select name , accs , byte from empl where idnum = $1  */
#line 63 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare B binary cursor for select name , accs , byte from empl where idnum = $1 ", 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);}
#line 64 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch B", ECPGt_EOIT, 
	ECPGt_char,(empl.name),(long)21,(long)1,(21)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(empl.accs),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(empl.byte),(long)20,(long)1,(20)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);}
#line 65 "binary.pgc"

  if (sqlca.sqlcode)
    {
      printf ("fetch error = %ld\n", sqlca.sqlcode);
      exit (sqlca.sqlcode);
    }

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close B", ECPGt_EOIT, ECPGt_EORT);}
#line 72 "binary.pgc"


  /* do not print a.accs because big/little endian will have different outputs here */
  printf ("name=%s, byte=", empl.name);
  for (i=0; i<20; i++)
  {
	if (empl.byte[i] == '#')
		break;
	printf("(%o)", (unsigned char)empl.byte[i]);
  }
  printf("\n");
  { ECPGdisconnect(__LINE__, "CURRENT");}
#line 83 "binary.pgc"

  exit (0);
}
