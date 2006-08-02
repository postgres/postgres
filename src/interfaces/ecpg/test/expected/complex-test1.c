/* Processed by ecpg (4.2.1) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */

#line 1 "test1.pgc"
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


#line 1 "./../regression.h"






#line 6 "test1.pgc"


/* just a test comment */ /* exec sql whenever sqlerror  do PrintAndStop ( msg ) ; */
#line 8 "test1.pgc"

/* exec sql whenever sql_warning  do warn (  ) ; */
#line 9 "test1.pgc"


static void PrintAndStop(char *msg)
{
	fprintf(stderr, "Error in statement '%s':\n", msg);
	sqlprint();
	exit(-1);
}

static void warn(void)
{
	fprintf(stderr, "Warning: At least one column was truncated\n");
}

/* comment */



/* exec sql type intarray is int [ 6 ]   */
#line 27 "test1.pgc"


typedef int intarray[ 6];

int
main(void)
{
/* exec sql begin declare section */

	   typedef char  string [ 8 ] ;

#line 36 "test1.pgc"

	 
	 
	   
	  
	 
	
		  
		 
		 
	  	 
	 
	
		 
		 
		 
	  
	 
	 
	 
	 
 
#line 37 "test1.pgc"
 intarray  amount    ;
 
#line 38 "test1.pgc"
 int  increment   = 100 ;
 
#line 39 "test1.pgc"
 char  name [ 6 ] [ 8 ]    ;
 
#line 40 "test1.pgc"
 char  letter [ 6 ] [ 1 ]    ;
 
#line 46 "test1.pgc"
 struct name_letter_struct { 
#line 43 "test1.pgc"
 char  name [ 8 ]    ;
 
#line 44 "test1.pgc"
 int  amount    ;
 
#line 45 "test1.pgc"
 char  letter    ;
 }  name_letter [ 6 ]    ;
 
#if 0
 
#line 48 "test1.pgc"
 int  not_used    ;
 
#endif
 
#line 56 "test1.pgc"
 struct ind_struct { 
#line 53 "test1.pgc"
 short  a    ;
 
#line 54 "test1.pgc"
 short  b    ;
 
#line 55 "test1.pgc"
 short  c    ;
 }  ind [ 6 ]    ;
 
#line 57 "test1.pgc"
 char  command [ 128 ]    ;
 
#line 58 "test1.pgc"
 char * connection   = "pm" ;
 
#line 59 "test1.pgc"
 int  how_many    ;
 
#line 60 "test1.pgc"
 char * user   = "regressuser1" ;
/* exec sql end declare section */
#line 61 "test1.pgc"

	/* exec sql var name is string [ 6 ]   */
#line 62 "test1.pgc"

	char msg[128];
	int i,j;

	ECPGdebug(1, stderr);

	strcpy(msg, "connect");
	{ ECPGconnect(__LINE__, 0, "regress1" , NULL,NULL , "main", 0); 
#line 69 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 69 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 69 "test1.pgc"


	strcpy(msg, "connect");
	{ ECPGconnect(__LINE__, 0, "connectdb" , user , NULL , "pm", 0); 
#line 72 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 72 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 72 "test1.pgc"


	strcpy(msg, "create");
	{ ECPGdo(__LINE__, 0, 1, "main", "create  table \"Test\" ( name char  ( 8 )    , amount int   , letter char  ( 1 )    )    ", ECPGt_EOIT, ECPGt_EORT);
#line 75 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 75 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 75 "test1.pgc"

	{ ECPGdo(__LINE__, 0, 1, NULL, "create  table \"Test\" ( name char  ( 8 )    , amount int   , letter char  ( 1 )    )    ", ECPGt_EOIT, ECPGt_EORT);
#line 76 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 76 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 76 "test1.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, "main", "commit");
#line 79 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 79 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 79 "test1.pgc"

	{ ECPGtrans(__LINE__, NULL, "commit");
#line 80 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 80 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 80 "test1.pgc"


	strcpy(msg, "set connection");
	{ ECPGsetconn(__LINE__, "main");
#line 83 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 83 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 83 "test1.pgc"


	strcpy(msg, "execute insert 1");
	sprintf(command, "insert into \"Test\" (name, amount, letter) values ('db: ''r1''', 1, 'f')");
	{ ECPGdo(__LINE__, 0, 1, NULL, "?", 
	ECPGt_char_variable,(command),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 87 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 87 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 87 "test1.pgc"

	printf("New tuple got OID = %ld\n", sqlca.sqlerrd[1]);

	sprintf(command, "insert into \"Test\" (name, amount, letter) values ('db: ''r1''', 2, 't')");
	{ ECPGdo(__LINE__, 0, 1, NULL, "?", 
	ECPGt_char_variable,(command),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 91 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 91 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 91 "test1.pgc"


	strcpy(msg, "execute insert 2");
	sprintf(command, "insert into \"Test\" (name, amount, letter) values ('db: ''pm''', 1, 'f')");
	{ ECPGdo(__LINE__, 0, 1, "pm", "?", 
	ECPGt_char_variable,(command),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 95 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 95 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 95 "test1.pgc"


	strcpy(msg, "execute insert 3");
	sprintf(command, "insert into \"Test\" (name, amount, letter) select name, amount+10, letter from \"Test\"");
	{ ECPGdo(__LINE__, 0, 1, NULL, "?", 
	ECPGt_char_variable,(command),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 99 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 99 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 99 "test1.pgc"


	printf("Inserted %ld tuples via execute immediate\n", sqlca.sqlerrd[2]);

	strcpy(msg, "execute insert 4");
	sprintf(command, "insert into \"Test\" (name, amount, letter) select name, amount+?, letter from \"Test\"");
	{ ECPGprepare(__LINE__, "I" , command);
#line 105 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 105 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 105 "test1.pgc"

	{ ECPGdo(__LINE__, 0, 1, "pm", "?", 
	ECPGt_char_variable,(ECPGprepared_statement("I")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(increment),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 106 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 106 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 106 "test1.pgc"


	printf("Inserted %ld tuples via prepared execute\n", sqlca.sqlerrd[2]);

	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 111 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 111 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 111 "test1.pgc"


	/* Start automatic transactioning for connection pm. */
	{ ECPGsetcommit(__LINE__, "on", "pm");
#line 114 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 114 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 114 "test1.pgc"

	{ ECPGtrans(__LINE__, "pm", "begin transaction ");
#line 115 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 115 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 115 "test1.pgc"


	strcpy(msg, "select");
	{ ECPGdo(__LINE__, 0, 1, NULL, "select  *  from \"Test\"   ", ECPGt_EOIT, 
	ECPGt_char,(name),(long)8,(long)6,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(amount),(long)1,(long)6,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(letter),(long)1,(long)6,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 118 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 118 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 118 "test1.pgc"


	printf("Database: main\n");
	for (i=0, how_many=j=sqlca.sqlerrd[2]; i<j; i++)
	{
		/* exec sql begin declare section */
		    
		   
		
#line 124 "test1.pgc"
 char  n [ 8 ]    ,  l   = letter [ i ] [ 0 ] ;
 
#line 125 "test1.pgc"
 int  a   = amount [ i ] ;
/* exec sql end declare section */
#line 126 "test1.pgc"


		strncpy(n, name[i],  8);
		printf("name[%d]=%8.8s\tamount[%d]=%d\tletter[%d]=%c\n", i, n, i, a, i, l);
		amount[i]+=1000;

		strcpy(msg, "insert");
		{ ECPGdo(__LINE__, 0, 1, "pm", "insert into \"Test\" ( name  , amount  , letter  ) values(  ? ,  ? ,  ? )", 
	ECPGt_char,(n),(long)8,(long)1,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(amount[i]),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(l),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 133 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 133 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 133 "test1.pgc"

	}

	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, "pm", "commit");
#line 137 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 137 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 137 "test1.pgc"


	sprintf (command, "select * from \"Test\"");

	{ ECPGprepare(__LINE__, "F" , command);
#line 141 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 141 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 141 "test1.pgc"

	/* declare CUR  cursor  for ? */
#line 142 "test1.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 0, 1, NULL, "declare CUR  cursor  for ?", 
	ECPGt_char_variable,(ECPGprepared_statement("F")),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 145 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 145 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 145 "test1.pgc"


	strcpy(msg, "fetch");
	{ ECPGdo(__LINE__, 0, 1, NULL, "fetch  ? in CUR", 
	ECPGt_int,&(how_many),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_char,(name),(long)8,(long)6,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(amount),(long)1,(long)6,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(letter),(long)1,(long)6,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 148 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 148 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 148 "test1.pgc"


	printf("Database: main\n");
	for (i=0, j=sqlca.sqlerrd[2]; i<j; i++)
	{
		/* exec sql begin declare section */
		    
		   
		
#line 154 "test1.pgc"
 char  n [ 8 ]    ,  l   = letter [ i ] [ 0 ] ;
 
#line 155 "test1.pgc"
 int  a   = amount [ i ] ;
/* exec sql end declare section */
#line 156 "test1.pgc"


		strncpy(n, name[i], 8);
		printf("name[%d]=%8.8s\tamount[%d]=%d\tletter[%d]=%c\n", i, n, i, a, i, l);
	}

	{ ECPGdo(__LINE__, 0, 1, NULL, "close CUR", ECPGt_EOIT, ECPGt_EORT);
#line 162 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 162 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 162 "test1.pgc"


	strcpy(msg, "select");
	{ ECPGdo(__LINE__, 0, 1, connection, "select  name , amount , letter  from \"Test\"   ", ECPGt_EOIT, 
	ECPGt_char,(name),(long)8,(long)6,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,(amount),(long)1,(long)6,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,(letter),(long)1,(long)6,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 165 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 165 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 165 "test1.pgc"


	printf("Database: %s\n", connection);
	for (i=0, j=sqlca.sqlerrd[2]; i<j; i++)
		printf("name[%d]=%8.8s\tamount[%d]=%d\tletter[%d]=%c\n", i, name[i], i, amount[i],i, letter[i][0]);

	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 172 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 172 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 172 "test1.pgc"


	strcpy(msg, "select");
	{ ECPGdo(__LINE__, 0, 1, "pm", "select  name , amount , letter  from \"Test\"   ", ECPGt_EOIT, 
	ECPGt_char,&(name_letter->name),(long)8,(long)6,sizeof( struct name_letter_struct ), 
	ECPGt_short,&(ind->a),(long)1,(long)6,sizeof( struct ind_struct ), 
	ECPGt_int,&(name_letter->amount),(long)1,(long)6,sizeof( struct name_letter_struct ), 
	ECPGt_short,&(ind->b),(long)1,(long)6,sizeof( struct ind_struct ), 
	ECPGt_char,&(name_letter->letter),(long)1,(long)6,sizeof( struct name_letter_struct ), 
	ECPGt_short,&(ind->c),(long)1,(long)6,sizeof( struct ind_struct ), ECPGt_EORT);
#line 175 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 175 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 175 "test1.pgc"


	printf("Database: pm\n");
	for (i=0, j=sqlca.sqlerrd[2]; i<j; i++)
		printf("name[%d]=%8.8s\tamount[%d]=%d\tletter[%d]=%c\n", i, name_letter[i].name, i, name_letter[i].amount,i, name_letter[i].letter);

	name_letter[4].amount=1407;
	strcpy(msg, "insert");
	{ ECPGdo(__LINE__, 0, 1, NULL, "insert into \"Test\" ( name  , amount  , letter  ) values(  ? ,  ? ,  ? )", 
	ECPGt_char,&(name_letter[4].name),(long)8,(long)1,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(name_letter[4].amount),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(name_letter[4].letter),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 183 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 183 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 183 "test1.pgc"


	strcpy(msg, "select");
	{ ECPGdo(__LINE__, 0, 1, NULL, "select  name , amount , letter  from \"Test\" where amount = 1407  ", ECPGt_EOIT, 
	ECPGt_char,&(name_letter[2].name),(long)8,(long)1,(8)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_int,&(name_letter[2].amount),(long)1,(long)1,sizeof(int), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, 
	ECPGt_char,&(name_letter[2].letter),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 186 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 186 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 186 "test1.pgc"


	printf("Database: main\n");
	printf("name[2]=%8.8s\tamount[2]=%d\tletter[2]=%c\n", name_letter[2].name, name_letter[2].amount, name_letter[2].letter);

	/* Start automatic transactioning for connection main. */
	{ ECPGsetcommit(__LINE__, "on", NULL);
#line 192 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 192 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 192 "test1.pgc"


	strcpy(msg, "drop");
	{ ECPGdo(__LINE__, 0, 1, NULL, "drop table \"Test\" ", ECPGt_EOIT, ECPGt_EORT);
#line 195 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 195 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 195 "test1.pgc"

	{ ECPGdo(__LINE__, 0, 1, "pm", "drop table \"Test\" ", ECPGt_EOIT, ECPGt_EORT);
#line 196 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 196 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 196 "test1.pgc"


	strcpy(msg, "disconnect");
	{ ECPGdisconnect(__LINE__, "main");
#line 199 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 199 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 199 "test1.pgc"

	{ ECPGdisconnect(__LINE__, "pm");
#line 200 "test1.pgc"

if (sqlca.sqlwarn[0] == 'W') warn (  );
#line 200 "test1.pgc"

if (sqlca.sqlcode < 0) PrintAndStop ( msg );}
#line 200 "test1.pgc"


	return (0);
}
