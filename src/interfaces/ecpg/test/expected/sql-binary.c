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


/* exec sql whenever sqlerror  stop ; */
#line 16 "binary.pgc"


int
main (void)
{
  /* exec sql begin declare section */
    
     
     
  
#line 22 "binary.pgc"
 struct TBempl empl ;
 
#line 23 "binary.pgc"
 char * pointer = NULL ;
 
#line 24 "binary.pgc"
 char * data = "\\001\\155\\000\\212" ;
/* exec sql end declare section */
#line 25 "binary.pgc"

  int i;

  ECPGdebug (1, stderr);

  empl.idnum = 1;
  { ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 31 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 31 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "set bytea_output = escape", ECPGt_EOIT, ECPGt_EORT);
#line 32 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 32 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table empl ( idnum integer , name char ( 20 ) , accs smallint , byte bytea )", ECPGt_EOIT, ECPGt_EORT);
#line 34 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 34 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into empl values ( 1 , 'first user' , 320 , $1  )", 
	ECPGt_char,&(data),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 35 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 35 "binary.pgc"

  ECPGset_var( 0, &( empl.idnum ), __LINE__);\
 /* declare C cursor for select name , accs , byte from empl where idnum = $1  */
#line 36 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 36 "binary.pgc"

#line 36 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare C cursor for select name , accs , byte from empl where idnum = $1 ", 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 37 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 37 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch C", ECPGt_EOIT, 
	ECPGt_char,(empl.name),(long)21,(long)1,(21)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(empl.accs),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(empl.byte),(long)20,(long)1,(20)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 38 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 38 "binary.pgc"

  printf ("name=%s, accs=%d byte=%s\n", empl.name, empl.accs, empl.byte);

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close C", ECPGt_EOIT, ECPGt_EORT);
#line 41 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 41 "binary.pgc"


  memset(empl.name, 0, 21L);
  ECPGset_var( 1, &( empl.idnum ), __LINE__);\
 /* declare B binary cursor for select name , accs , byte from empl where idnum = $1  */
#line 44 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 44 "binary.pgc"

#line 44 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare B binary cursor for select name , accs , byte from empl where idnum = $1 ", 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 45 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 45 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch B", ECPGt_EOIT, 
	ECPGt_char,(empl.name),(long)21,(long)1,(21)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_short,&(empl.accs),(long)1,(long)1,sizeof(short), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(empl.byte),(long)20,(long)1,(20)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 46 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 46 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close B", ECPGt_EOIT, ECPGt_EORT);
#line 47 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 47 "binary.pgc"


  /* do not print a.accs because big/little endian will have different outputs here */
  printf ("name=%s, byte=", empl.name);
  for (i=0; i<4; i++)
	printf("(%o)", (unsigned char)empl.byte[i]);
  printf("\n");

  ECPGset_var( 2, &( empl.idnum ), __LINE__);\
 /* declare A binary cursor for select byte from empl where idnum = $1  */
#line 55 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);
#line 55 "binary.pgc"

#line 55 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "declare A binary cursor for select byte from empl where idnum = $1 ", 
	ECPGt_long,&(empl.idnum),(long)1,(long)1,sizeof(long), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 56 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 56 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "fetch A", ECPGt_EOIT, 
	ECPGt_char,&(pointer),(long)0,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 57 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 57 "binary.pgc"

  { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "close A", ECPGt_EOIT, ECPGt_EORT);
#line 58 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 58 "binary.pgc"


  if (pointer) {
	printf ("pointer=");
	for (i=0; i<4; i++)
		printf("(%o)", (unsigned char)pointer[i]);
	printf("\n");
	free(pointer);
  }

  { ECPGdisconnect(__LINE__, "CURRENT");
#line 68 "binary.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 68 "binary.pgc"

  exit (0);
}
