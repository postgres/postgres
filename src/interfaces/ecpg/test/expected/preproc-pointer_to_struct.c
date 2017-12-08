/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "pointer_to_struct.pgc"
#include <stdio.h>
#include <stdlib.h>


#line 1 "regression.h"






#line 4 "pointer_to_struct.pgc"


/* exec sql whenever sqlerror  sqlprint ; */
#line 6 "pointer_to_struct.pgc"

/* exec sql whenever sql_warning  sqlprint ; */
#line 7 "pointer_to_struct.pgc"

/* exec sql whenever not found  sqlprint ; */
#line 8 "pointer_to_struct.pgc"


typedef  struct { 
#line 13 "pointer_to_struct.pgc"
  struct varchar_1  { int len; char arr[ 50 ]; }  name ;
 
#line 14 "pointer_to_struct.pgc"
 int phone ;
 } customer ;
#line 15 "pointer_to_struct.pgc"


typedef  struct ind { 
#line 20 "pointer_to_struct.pgc"
 short name_ind ;
 
#line 21 "pointer_to_struct.pgc"
 short phone_ind ;
 } cust_ind ;
#line 22 "pointer_to_struct.pgc"


int main()
{
    /* exec sql begin declare section */
              
              
       
      
         
             
       typedef struct { 
#line 31 "pointer_to_struct.pgc"
  struct varchar_2  { int len; char arr[ 50 ]; }  name ;
 
#line 32 "pointer_to_struct.pgc"
 int phone ;
 }  customer2 ;

#line 33 "pointer_to_struct.pgc"

              

       
      
         
             
               

       
      
         
             
             

       
       
    
#line 27 "pointer_to_struct.pgc"
 customer * custs1 = ( customer * ) malloc ( sizeof ( customer ) * 10 ) ;
 
#line 28 "pointer_to_struct.pgc"
 cust_ind * inds = ( cust_ind * ) malloc ( sizeof ( cust_ind ) * 10 ) ;
 
#line 34 "pointer_to_struct.pgc"
 customer2 * custs2 = ( customer2 * ) malloc ( sizeof ( customer2 ) * 10 ) ;
 
#line 40 "pointer_to_struct.pgc"
 struct customer3 { 
#line 38 "pointer_to_struct.pgc"
 char name [ 50 ] ;
 
#line 39 "pointer_to_struct.pgc"
 int phone ;
 } * custs3 = ( struct customer3 * ) malloc ( sizeof ( struct customer3 ) * 10 ) ;
 
#line 46 "pointer_to_struct.pgc"
 struct customer4 { 
#line 44 "pointer_to_struct.pgc"
  struct varchar_3  { int len; char arr[ 50 ]; }  name ;
 
#line 45 "pointer_to_struct.pgc"
 int phone ;
 } * custs4 = ( struct customer4 * ) malloc ( sizeof ( struct customer4 ) ) ;
 
#line 48 "pointer_to_struct.pgc"
 int r ;
 
#line 49 "pointer_to_struct.pgc"
  struct varchar_4  { int len; char arr[ 50 ]; }  onlyname [ 2 ] ;
/* exec sql end declare section */
#line 50 "pointer_to_struct.pgc"


    ECPGdebug(1, stderr);

    { ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 54 "pointer_to_struct.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 54 "pointer_to_struct.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 54 "pointer_to_struct.pgc"


    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table customers ( c varchar ( 50 ) , p int )", ECPGt_EOIT, ECPGt_EORT);
#line 56 "pointer_to_struct.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 56 "pointer_to_struct.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 56 "pointer_to_struct.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into customers values ( 'John Doe' , '12345' )", ECPGt_EOIT, ECPGt_EORT);
#line 57 "pointer_to_struct.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) sqlprint();
#line 57 "pointer_to_struct.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 57 "pointer_to_struct.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 57 "pointer_to_struct.pgc"

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into customers values ( 'Jane Doe' , '67890' )", ECPGt_EOIT, ECPGt_EORT);
#line 58 "pointer_to_struct.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) sqlprint();
#line 58 "pointer_to_struct.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 58 "pointer_to_struct.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 58 "pointer_to_struct.pgc"


    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select * from customers limit 2", ECPGt_EOIT, 
	ECPGt_varchar,&(custs1->name),(long)50,(long)-1,sizeof( customer ), 
	ECPGt_short,&(inds->name_ind),(long)1,(long)-1,sizeof( struct ind ), 
	ECPGt_int,&(custs1->phone),(long)1,(long)-1,sizeof( customer ), 
	ECPGt_short,&(inds->phone_ind),(long)1,(long)-1,sizeof( struct ind ), ECPGt_EORT);
#line 60 "pointer_to_struct.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) sqlprint();
#line 60 "pointer_to_struct.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 60 "pointer_to_struct.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 60 "pointer_to_struct.pgc"

    printf("custs1:\n");
    for (r = 0; r < 2; r++)
    {
	    printf( "name  - %s\n", custs1[r].name.arr );
	    printf( "phone - %d\n", custs1[r].phone );
    }

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select * from customers limit 2", ECPGt_EOIT, 
	ECPGt_varchar,&(custs2->name),(long)50,(long)-1,sizeof( customer2 ), 
	ECPGt_short,&(inds->name_ind),(long)1,(long)-1,sizeof( struct ind ), 
	ECPGt_int,&(custs2->phone),(long)1,(long)-1,sizeof( customer2 ), 
	ECPGt_short,&(inds->phone_ind),(long)1,(long)-1,sizeof( struct ind ), ECPGt_EORT);
#line 68 "pointer_to_struct.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) sqlprint();
#line 68 "pointer_to_struct.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 68 "pointer_to_struct.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 68 "pointer_to_struct.pgc"

    printf("\ncusts2:\n");
    for (r = 0; r < 2; r++)
    {
	    printf( "name  - %s\n", custs2[r].name.arr );
	    printf( "phone - %d\n", custs2[r].phone );
    }

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select * from customers limit 2", ECPGt_EOIT, 
	ECPGt_char,&(custs3->name),(long)50,(long)-1,sizeof( struct customer3 ), 
	ECPGt_short,&(inds->name_ind),(long)1,(long)-1,sizeof( struct ind ), 
	ECPGt_int,&(custs3->phone),(long)1,(long)-1,sizeof( struct customer3 ), 
	ECPGt_short,&(inds->phone_ind),(long)1,(long)-1,sizeof( struct ind ), ECPGt_EORT);
#line 76 "pointer_to_struct.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) sqlprint();
#line 76 "pointer_to_struct.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 76 "pointer_to_struct.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 76 "pointer_to_struct.pgc"

    printf("\ncusts3:\n");
    for (r = 0; r < 2; r++)
    {
	    printf( "name  - %s\n", custs3[r].name );
	    printf( "phone - %d\n", custs3[r].phone );
    }

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select * from customers limit 1", ECPGt_EOIT, 
	ECPGt_varchar,&(custs4->name),(long)50,(long)-1,sizeof( struct customer4 ), 
	ECPGt_short,&(inds->name_ind),(long)1,(long)-1,sizeof( struct ind ), 
	ECPGt_int,&(custs4->phone),(long)1,(long)-1,sizeof( struct customer4 ), 
	ECPGt_short,&(inds->phone_ind),(long)1,(long)-1,sizeof( struct ind ), ECPGt_EORT);
#line 84 "pointer_to_struct.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) sqlprint();
#line 84 "pointer_to_struct.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 84 "pointer_to_struct.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 84 "pointer_to_struct.pgc"

    printf("\ncusts4:\n");
    printf( "name  - %s\n", custs4->name.arr );
    printf( "phone - %d\n", custs4->phone );

    { ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "select c from customers limit 2", ECPGt_EOIT, 
	ECPGt_varchar,(onlyname),(long)50,(long)2,sizeof(struct varchar_4), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 89 "pointer_to_struct.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) sqlprint();
#line 89 "pointer_to_struct.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 89 "pointer_to_struct.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 89 "pointer_to_struct.pgc"

    printf("\nname:\n");
    for (r = 0; r < 2; r++)
    {
	    printf( "name  - %s\n", onlyname[r].arr );
    }

    { ECPGdisconnect(__LINE__, "ALL");
#line 96 "pointer_to_struct.pgc"

if (sqlca.sqlwarn[0] == 'W') sqlprint();
#line 96 "pointer_to_struct.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 96 "pointer_to_struct.pgc"


	/* All the memory will anyway be freed at the end */
    return 0;
}
