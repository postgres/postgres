/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* Needed for informix compatibility */
#include <ecpg_informix.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "sqlda.pgc"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>


#line 1 "regression.h"






#line 5 "sqlda.pgc"



#line 1 "sqlda.h"
#ifndef ECPG_SQLDA_H
#define ECPG_SQLDA_H

#ifdef _ECPG_INFORMIX_H

#include "sqlda-compat.h"
typedef struct sqlvar_compat	sqlvar_t;
typedef struct sqlda_compat	sqlda_t;

#else

#include "sqlda-native.h"
typedef struct sqlvar_struct	sqlvar_t;
typedef struct sqlda_struct	sqlda_t;

#endif

#endif /* ECPG_SQLDA_H */

#line 7 "sqlda.pgc"


#line 1 "sqltypes.h"
#ifndef ECPG_SQLTYPES_H
#define ECPG_SQLTYPES_H

#include <limits.h>

#define CCHARTYPE	ECPGt_char
#define CSHORTTYPE	ECPGt_short
#define CINTTYPE	ECPGt_int
#define CLONGTYPE	ECPGt_long
#define CFLOATTYPE	ECPGt_float
#define CDOUBLETYPE ECPGt_double
#define CDECIMALTYPE	ECPGt_decimal
#define CFIXCHARTYPE	108
#define CSTRINGTYPE ECPGt_char
#define CDATETYPE	ECPGt_date
#define CMONEYTYPE	111
#define CDTIMETYPE	ECPGt_timestamp
#define CLOCATORTYPE	113
#define CVCHARTYPE	ECPGt_varchar
#define CINVTYPE	115
#define CFILETYPE	116
#define CINT8TYPE	ECPGt_long_long
#define CCOLLTYPE		118
#define CLVCHARTYPE		119
#define CFIXBINTYPE		120
#define CVARBINTYPE		121
#define CBOOLTYPE		ECPGt_bool
#define CROWTYPE		123
#define CLVCHARPTRTYPE	124
#define CTYPEMAX	25

/*
 * Values used in sqlda->sqlvar[i]->sqltype
 */
#define	SQLCHAR		ECPGt_char
#define	SQLSMINT	ECPGt_short
#define	SQLINT		ECPGt_int
#define	SQLFLOAT	ECPGt_double
#define	SQLSMFLOAT	ECPGt_float
#define	SQLDECIMAL	ECPGt_decimal
#define	SQLSERIAL	ECPGt_int
#define	SQLDATE		ECPGt_date
#define	SQLDTIME	ECPGt_timestamp
#define	SQLTEXT		ECPGt_char
#define	SQLVCHAR	ECPGt_char
#define SQLINTERVAL     ECPGt_interval
#define	SQLNCHAR	ECPGt_char
#define	SQLNVCHAR	ECPGt_char
#ifdef HAVE_LONG_LONG_INT_64
#define	SQLINT8		ECPGt_long_long
#define	SQLSERIAL8	ECPGt_long_long
#else
#define	SQLINT8		ECPGt_long
#define	SQLSERIAL8	ECPGt_long
#endif

#endif   /* ndef ECPG_SQLTYPES_H */

#line 8 "sqlda.pgc"


/* exec sql whenever sqlerror  stop ; */
#line 10 "sqlda.pgc"


/* These shouldn't be under DECLARE SECTION */
sqlda_t	*inp_sqlda, *outp_sqlda;

static void
dump_sqlda(sqlda_t *sqlda)
{
	int	i;

	if (sqlda == NULL)
	{
		printf("dump_sqlda called with NULL sqlda\n");
		return;
	}

	for (i = 0; i < sqlda->sqld; i++)
	{
		if (sqlda->sqlvar[i].sqlind && *(sqlda->sqlvar[i].sqlind) == -1)
			printf("name sqlda descriptor: '%s' value NULL'\n", sqlda->sqlvar[i].sqlname);
		else
		switch (sqlda->sqlvar[i].sqltype)
		{
		case SQLCHAR:
			printf("name sqlda descriptor: '%s' value '%s'\n", sqlda->sqlvar[i].sqlname, sqlda->sqlvar[i].sqldata);
			break;
		case SQLINT:
			printf("name sqlda descriptor: '%s' value %d\n", sqlda->sqlvar[i].sqlname, *(int *)sqlda->sqlvar[i].sqldata);
			break;
		case SQLINT8:
			printf("name sqlda descriptor: '%s' value %" PRId64 "\n", sqlda->sqlvar[i].sqlname, *(int64_t *)sqlda->sqlvar[i].sqldata);
			break;
		case SQLFLOAT:
			printf("name sqlda descriptor: '%s' value %lf\n", sqlda->sqlvar[i].sqlname, *(double *)sqlda->sqlvar[i].sqldata);
			break;
		case SQLDECIMAL:
			{
				char    val[64];
				dectoasc((decimal *)sqlda->sqlvar[i].sqldata, val, 64, -1);
				printf("name sqlda descriptor: '%s' value DECIMAL '%s'\n", sqlda->sqlvar[i].sqlname, val);
				break;
			}
		}
	}
}

int
main (void)
{
/* exec sql begin declare section */
		  
		  
		
		

#line 60 "sqlda.pgc"
 char * stmt1 = "SELECT * FROM t1" ;
 
#line 61 "sqlda.pgc"
 char * stmt2 = "SELECT * FROM t1 WHERE id = ?" ;
 
#line 62 "sqlda.pgc"
 int rec ;
 
#line 63 "sqlda.pgc"
 int id ;
/* exec sql end declare section */
#line 64 "sqlda.pgc"


	char msg[128];

	ECPGdebug(1, stderr);

	strcpy(msg, "connect");
	{ ECPGconnect(__LINE__, 1, "regress1" , NULL, NULL , "regress1", 0); 
#line 71 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 71 "sqlda.pgc"


	strcpy(msg, "set");
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "set datestyle to iso", ECPGt_EOIT, ECPGt_EORT);
#line 74 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 74 "sqlda.pgc"


	strcpy(msg, "create");
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "create table t1 ( id integer , t text , d1 numeric , d2 float8 , c char ( 10 ) )", ECPGt_EOIT, ECPGt_EORT);
#line 82 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 82 "sqlda.pgc"


	strcpy(msg, "insert");
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "insert into t1 values ( 1 , 'a' , 1.0 , 1 , 'a' ) , ( 2 , null , null , null , null ) , ( 3 , '\"c\"' , - 3 , 'nan' :: float8 , 'c' ) , ( 4 , 'd' , 4.0 , 4 , 'd' )", ECPGt_EOIT, ECPGt_EORT);
#line 89 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 89 "sqlda.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 92 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 92 "sqlda.pgc"


	/* SQLDA test for getting all records from a table */

	outp_sqlda = NULL;

	strcpy(msg, "prepare");
	{ ECPGprepare(__LINE__, NULL, 0, "st_id1", stmt1);
#line 99 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 99 "sqlda.pgc"


	strcpy(msg, "declare");
	ECPG_informix_reset_sqlca(); /* declare mycur1 cursor for $1 */
#line 102 "sqlda.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "declare mycur1 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "st_id1", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 105 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 105 "sqlda.pgc"


	/* exec sql whenever not found  break ; */
#line 107 "sqlda.pgc"


	rec = 0;
	while (1)
	{
		strcpy(msg, "fetch");
		{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "fetch 1 from mycur1", ECPGt_EOIT, 
	ECPGt_sqlda, &outp_sqlda, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 113 "sqlda.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 113 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 113 "sqlda.pgc"
 

		printf("FETCH RECORD %d\n", ++rec);
		dump_sqlda(outp_sqlda);
	}

	/* exec sql whenever not found  continue ; */
#line 119 "sqlda.pgc"


	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "close mycur1", ECPGt_EOIT, ECPGt_EORT);
#line 122 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 122 "sqlda.pgc"


	strcpy(msg, "deallocate");
	{ ECPGdeallocate(__LINE__, 1, NULL, "st_id1");
#line 125 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 125 "sqlda.pgc"


	free(outp_sqlda);

	/* SQLDA test for getting all records from a table
	   using the Informix-specific FETCH ... USING DESCRIPTOR
	 */

	outp_sqlda = NULL;

	strcpy(msg, "prepare");
	{ ECPGprepare(__LINE__, NULL, 0, "st_id2", stmt1);
#line 136 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 136 "sqlda.pgc"


	strcpy(msg, "declare");
	ECPG_informix_reset_sqlca(); /* declare mycur2 cursor for $1 */
#line 139 "sqlda.pgc"


	strcpy(msg, "open");
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "declare mycur2 cursor for $1", 
	ECPGt_char_variable,(ECPGprepared_statement(NULL, "st_id2", __LINE__)),(long)1,(long)1,(1)*sizeof(char), 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, ECPGt_EORT);
#line 142 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 142 "sqlda.pgc"


	/* exec sql whenever not found  break ; */
#line 144 "sqlda.pgc"


	rec = 0;
	while (1)
	{
		strcpy(msg, "fetch");
		{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "fetch from mycur2", ECPGt_EOIT, 
	ECPGt_sqlda, &outp_sqlda, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 150 "sqlda.pgc"

if (sqlca.sqlcode == ECPG_NOT_FOUND) break;
#line 150 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 150 "sqlda.pgc"


		printf("FETCH RECORD %d\n", ++rec);
		dump_sqlda(outp_sqlda);
	}

	/* exec sql whenever not found  continue ; */
#line 156 "sqlda.pgc"


	strcpy(msg, "close");
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "close mycur2", ECPGt_EOIT, ECPGt_EORT);
#line 159 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 159 "sqlda.pgc"


	strcpy(msg, "deallocate");
	{ ECPGdeallocate(__LINE__, 1, NULL, "st_id2");
#line 162 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 162 "sqlda.pgc"


	free(outp_sqlda);

	/* SQLDA test for getting one record using an input descriptor */

	/* Input sqlda has to be built manually */
	inp_sqlda = (sqlda_t *)malloc(sizeof(sqlda_t));
	memset(inp_sqlda, 0, sizeof(sqlda_t));
	inp_sqlda->sqld = 1;
	inp_sqlda->sqlvar = malloc(sizeof(sqlvar_t));
	memset(inp_sqlda->sqlvar, 0, sizeof(sqlvar_t));

	inp_sqlda->sqlvar[0].sqltype = SQLINT;
	inp_sqlda->sqlvar[0].sqldata = (char *)&id;

	printf("EXECUTE RECORD 4\n");

	id = 4;

	outp_sqlda = NULL;

	strcpy(msg, "prepare");
	{ ECPGprepare(__LINE__, NULL, 0, "st_id3", stmt2);
#line 185 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 185 "sqlda.pgc"


	strcpy(msg, "execute");
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_execute, "st_id3", 
	ECPGt_sqlda, &inp_sqlda, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_sqlda, &outp_sqlda, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 188 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 188 "sqlda.pgc"


	dump_sqlda(outp_sqlda);

	strcpy(msg, "deallocate");
	{ ECPGdeallocate(__LINE__, 1, NULL, "st_id3");
#line 193 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 193 "sqlda.pgc"


	free(inp_sqlda->sqlvar);
	free(inp_sqlda);
	free(outp_sqlda);

	/* SQLDA test for getting one record using an input descriptor
	 * on a named connection
	 */

	{ ECPGconnect(__LINE__, 1, "regress1" , NULL, NULL , "con2", 0); 
#line 203 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 203 "sqlda.pgc"


	/* Input sqlda has to be built manually */
	inp_sqlda = (sqlda_t *)malloc(sizeof(sqlda_t));
	memset(inp_sqlda, 0, sizeof(sqlda_t));
	inp_sqlda->sqld = 1;
	inp_sqlda->sqlvar = malloc(sizeof(sqlvar_t));
	memset(inp_sqlda->sqlvar, 0, sizeof(sqlvar_t));

	inp_sqlda->sqlvar[0].sqltype = SQLINT;
	inp_sqlda->sqlvar[0].sqldata = (char *)&id;

	printf("EXECUTE RECORD 4\n");

	id = 4;

	outp_sqlda = NULL;

	strcpy(msg, "prepare");
	{ ECPGprepare(__LINE__, "con2", 0, "st_id4", stmt2);
#line 222 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 222 "sqlda.pgc"


	strcpy(msg, "execute");
	{ ECPGdo(__LINE__, 1, 1, "con2", 0, ECPGst_execute, "st_id4", 
	ECPGt_sqlda, &inp_sqlda, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EOIT, 
	ECPGt_sqlda, &outp_sqlda, 0L, 0L, 0L, 
	ECPGt_NO_INDICATOR, NULL , 0L, 0L, 0L, ECPGt_EORT);
#line 225 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 225 "sqlda.pgc"


	dump_sqlda(outp_sqlda);

	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, "con2", "commit");
#line 230 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 230 "sqlda.pgc"


	strcpy(msg, "deallocate");
	{ ECPGdeallocate(__LINE__, 1, NULL, "st_id4");
#line 233 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 233 "sqlda.pgc"


	free(inp_sqlda->sqlvar);
	free(inp_sqlda);
	free(outp_sqlda);

	strcpy(msg, "disconnect");
	{ ECPGdisconnect(__LINE__, "con2");
#line 240 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 240 "sqlda.pgc"


	/* End test */

	strcpy(msg, "drop");
	{ ECPGdo(__LINE__, 1, 1, NULL, 0, ECPGst_normal, "drop table t1", ECPGt_EOIT, ECPGt_EORT);
#line 245 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 245 "sqlda.pgc"


	strcpy(msg, "commit");
	{ ECPGtrans(__LINE__, NULL, "commit");
#line 248 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 248 "sqlda.pgc"


	strcpy(msg, "disconnect");
	{ ECPGdisconnect(__LINE__, "CURRENT");
#line 251 "sqlda.pgc"

if (sqlca.sqlcode < 0) exit (1);}
#line 251 "sqlda.pgc"


	return (0);
}
