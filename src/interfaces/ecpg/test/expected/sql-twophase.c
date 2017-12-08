/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "twophase.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#line 1 "regression.h"






#line 5 "twophase.pgc"


/* exec sql whenever sqlerror  sqlprint ; */
#line 7 "twophase.pgc"


int main(void)
{
	char msg[128];

	ECPGdebug(1, stderr);

	strcpy(msg, "connect");
	{ ECPGconnect(__LINE__, 0, "ecpg1_regression" , NULL, NULL , NULL, 0); 
#line 16 "twophase.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 16 "twophase.pgc"

	{ ECPGsetcommit(__LINE__, "off", NULL);
#line 17 "twophase.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 17 "twophase.pgc"


	strcpy(msg, "create");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "create table t1 ( c int )", ECPGt_EOIT, ECPGt_EORT);
#line 20 "twophase.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 20 "twophase.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 23 "twophase.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 23 "twophase.pgc"


	strcpy(msg, "begin");
	{ ECPGtrans(__LINE__, NULL, "begin");
#line 26 "twophase.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 26 "twophase.pgc"


	strcpy(msg, "insert");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "insert into t1 values ( 1 )", ECPGt_EOIT, ECPGt_EORT);
#line 29 "twophase.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 29 "twophase.pgc"


	strcpy(msg, "prepare transaction");
	{ ECPGtrans(__LINE__, NULL, "prepare transaction 'gxid'");
#line 32 "twophase.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 32 "twophase.pgc"


	strcpy(msg, "commit prepared");
	{ ECPGtrans(__LINE__, NULL, "commit prepared 'gxid'");
#line 35 "twophase.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 35 "twophase.pgc"


	strcpy(msg, "drop");
	{ ECPGdo(__LINE__, 0, 1, NULL, 0, ECPGst_normal, "drop table t1", ECPGt_EOIT, ECPGt_EORT);
#line 38 "twophase.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 38 "twophase.pgc"


	strcpy(msg, "disconnect");
	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 41 "twophase.pgc"

if (sqlca.sqlcode < 0) sqlprint();}
#line 41 "twophase.pgc"


	return 0;
}
