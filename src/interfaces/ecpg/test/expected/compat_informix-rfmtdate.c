/* Processed by ecpg (regression mode) */
/* These include files are added by the preprocessor */
#include <ecpgtype.h>
#include <ecpglib.h>
#include <ecpgerrno.h>
#include <sqlca.h>
/* Needed for informix compatibility */
#include <ecpg_informix.h>
/* End of automatic include section */
#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))

#line 1 "rfmtdate.pgc"
#include <stdio.h>
#include <stdlib.h>
#include <pgtypes_error.h>
#include <sqltypes.h>

/*
 * This file tests various forms of date-input/output by means of
 * rfmtdate / rdefmtdate / rstrdate
 */


static void
check_return(int ret);

static void
date_test_strdate(char *input)
{
	static int i;
	date d;
	int r, q;
	char dbuf[11];

	r = rstrdate(input, &d);
	printf("r: %d ", r);
	if (r == 0)
	{
		q = rdatestr(d, dbuf);
		printf("q: %d ", q);
		if (q == 0)
		{
			printf("date %d: %s\n", i++, dbuf);
		}
		else
			printf("\n");
	}
	else
		check_return(r);
}

static void
date_test_defmt(char *fmt, char *input)
{
	static int i;
	char dbuf[11];
	date d;
	int q, r;

	r = rdefmtdate(&d, fmt, input);
	printf("r: %d ", r);
	if (r == 0)
	{
		q = rdatestr(d, dbuf);
		printf("q: %d ", q);
		if (q == 0)
		{
			printf("date %d: %s\n", i++, dbuf);
		}
		else
			printf("\n");
	}
	else
		check_return(r);
}

static void
date_test_fmt(date d, char *fmt)
{
	static int i;
	char buf[200];
	int r;

	r = rfmtdate(d, fmt, buf);
	printf("r: %d ", r);
	if (r != 0)
		check_return(r);
	else
		printf("date: %d: %s\n", i++, buf);
}


int
main(void)
{
	short mdy[3] = { 11, 23, 1959 };
	char dbuf[11];
	date d;
	int r;

	ECPGdebug(1, stderr);

	r = rmdyjul(mdy, &d);
	printf("create: r: %d\n", r);
	if (r == 0)
	{
		rdatestr(d, dbuf);
		printf("date: %s\n", dbuf);
	}

	/* input mask is mmddyyyy */
	date_test_strdate("12031994");
	date_test_strdate("9.6.1994");

	date_test_fmt(d, "mmddyy");
	date_test_fmt(d, "ddmmyy");
	date_test_fmt(d, "yymmdd");
	date_test_fmt(d, "yy/mm/dd");
	date_test_fmt(d, "yy mm dd");
	date_test_fmt(d, "yy.mm.dd");
	date_test_fmt(d, ".mm.yyyy.dd.");
	date_test_fmt(d, "mmm. dd, yyyy");
	date_test_fmt(d, "mmm dd yyyy");
	date_test_fmt(d, "yyyy dd mm");
	date_test_fmt(d, "ddd, mmm. dd, yyyy");
	date_test_fmt(d, "(ddd) mmm. dd, yyyy");

	date_test_defmt("ddmmyy", "21-2-54");
	date_test_defmt("ddmmyy", "2-12-54");
	date_test_defmt("ddmmyy", "20111954");
	date_test_defmt("ddmmyy", "130464");
	date_test_defmt("mmm.dd.yyyy", "MAR-12-1967");
	date_test_defmt("yy/mm/dd", "1954, February 3rd");
	date_test_defmt("mmm.dd.yyyy", "041269");
	date_test_defmt("yy/mm/dd", "In the year 2525, in the month of July, mankind will be alive on the 28th day");
	date_test_defmt("dd-mm-yy", "I said on the 28th of July in the year 2525");
	date_test_defmt("mmm.dd.yyyy", "9/14/58");
	date_test_defmt("yy/mm/dd", "47/03/29");
	date_test_defmt("mmm.dd.yyyy", "oct 28 1975");
	date_test_defmt("mmddyy", "Nov 14th, 1985");
	/* ok: still contains dd mm yy */
	date_test_defmt("bladdfoommbaryybong", "20/11/1954");
	/* 1994 is not a leap year, it accepts the date as 01-03-1994 */
	date_test_defmt("ddmmyy", "29-02-1994");

	/* ECPG_INFORMIX_ENOTDMY, need "dd", "mm" and "yy" */
	date_test_defmt("dmy", "20/11/1954");

	/* ECPG_INFORMIX_ENOSHORTDATE */
	date_test_defmt("ddmmyy", "21254");
	date_test_defmt("ddmmyy", "    21254    ");

	/* ECPG_INFORMIX_BAD_DAY */
	date_test_defmt("ddmmyy", "320494");

	/* ECPG_INFORMIX_BAD_MONTH */
	date_test_defmt("mm-yyyy-dd", "13-1993-21");

	/* ECPG_INFORMIX_BAD_YEAR */
	/* ??? */

	return (0);
}

static void
check_return(int ret)
{
	switch(ret)
	{
		case ECPG_INFORMIX_ENOTDMY:
			printf("(ECPG_INFORMIX_ENOTDMY)");
			break;
		case ECPG_INFORMIX_ENOSHORTDATE:
			printf("(ECPG_INFORMIX_ENOSHORTDATE)");
			break;
		case ECPG_INFORMIX_BAD_DAY:
			printf("(ECPG_INFORMIX_BAD_DAY)");
			break;
		case ECPG_INFORMIX_BAD_MONTH:
			printf("(ECPG_INFORMIX_BAD_MONTH)");
			break;
		default:
			printf("(unknown ret: %d)", ret);
			break;
	}
	printf("\n");
}
