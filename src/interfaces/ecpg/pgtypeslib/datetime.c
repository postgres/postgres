#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <float.h>
#include <stdlib.h>
#include <stdio.h>

#include "dt.h"
#include "extern.h"
#include "pgtypes_error.h"
#include "pgtypes_date.h"
#include "ecpg_informix.h"

Date
PGTYPESdate_atod(char *str, char **endptr)
{
	
	Date		dDate;
	fsec_t		fsec;
	struct tm	tt,
			   *tm = &tt;
	int			tzp;
	int			dtype;
	int			nf;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + 1];
	char		*realptr;
	char **ptr = (endptr != NULL) ? endptr : &realptr;
	
	bool		EuroDates = FALSE;

	if (strlen(str) >= sizeof(lowstr))
	{
		errno = PGTYPES_BAD_DATE;
		return -1;
	}

	if ((ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf, ptr) != 0)
	 || (DecodeDateTime(field, ftype, nf, &dtype, tm, &fsec, &tzp, EuroDates) != 0))
	{
		errno = PGTYPES_BAD_DATE;
		return -1;
	}

	switch (dtype)
	{
		case DTK_DATE:
			break;

		case DTK_EPOCH:
			GetEpochTime(tm); 
			break;

		default:
			errno = PGTYPES_BAD_DATE;
			return -1;
	}

	dDate = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - date2j(2000, 1, 1));

	return dDate;
}

char *
PGTYPESdate_dtoa(Date dDate)
{
	struct tm       tt, *tm = &tt;
	char            buf[MAXDATELEN + 1];
	int DateStyle=0;
	bool		EuroDates = FALSE;
						   
	j2date((dDate + date2j(2000, 1, 1)), &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
	EncodeDateOnly(tm, DateStyle, buf, EuroDates);
	return pgtypes_strdup(buf);
}

int
PGTYPESdate_julmdy(Date jd, int* mdy)
{
	printf("day: %d\n", mdy[0]);
	printf("month: %d\n", mdy[1]);
	printf("year: %d\n", mdy[2]);
	j2date((int) jd, mdy+2, mdy+1, mdy+0);
	return 0;
}

int
PGTYPESdate_mdyjul(int* mdy, Date *jdate)
{
	/* month is mdy[0] */
	/* day   is mdy[1] */
	/* year  is mdy[2] */
	printf("day: %d\n", mdy[1]);
	printf("month: %d\n", mdy[0]);
	printf("year: %d\n", mdy[2]);
	*jdate = (Date) date2j(mdy[2], mdy[0], mdy[1]);
	return 0;
}

int
PGTYPESdate_day(Date dDate)
{
	return j2day(dDate);
}

int
rdatestr (Date d, char *str)
{
	return 0;
}

void
rtoday (Date *d)
{
	return;
}

int
rjulmdy (Date d, short mdy[3])
{
	return 0;
}

int
rdefmtdate (Date *d, char *fmt, char *str)
{
	return 0;
}

int
rfmtdate (Date d, char *fmt, char *str)
{
	return 0;
}

int
rmdyjul (short mdy[3], Date *d)
{
	return 0;
}

int
rstrdate (char *str, Date *d)
{
	return 0;
}

