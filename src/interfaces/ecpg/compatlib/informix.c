#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include <ecpg_informix.h>
#include <pgtypes_error.h>
#include <pgtypes_date.h>

/* we start with the numeric functions */
int
decadd(Numeric *arg1, Numeric *arg2, Numeric *sum)
{
	int i = PGTYPESnumeric_add(arg1, arg2, sum);

	if (i == 0) /* No error */
		return 0;
	if (errno == PGTYPES_NUM_OVERFLOW)
		return -1200;

	return -1201;	
}

int
deccmp(Numeric *arg1, Numeric *arg2)
{
	int i = PGTYPESnumeric_cmp(arg1, arg2);
	
	/* TODO: Need to return DECUNKNOWN instead of PGTYPES_NUM_BAD_NUMERIC */
	return (i);
}

void
deccopy(Numeric *src, Numeric *target)
{
	PGTYPESnumeric_copy(src, target);
}

static char *
strndup(char *str, int len)
{
	int real_len = strlen(str);
	int use_len = (real_len > len) ? len : real_len;
	
	char *new = malloc(use_len + 1);

	if (new)
	{
		memcpy(str, new, use_len);
		new[use_len] = '\0';
	}
	else
		errno = ENOMEM;

	return new;
}

int
deccvasc(char *cp, int len, Numeric *np)
{
	char *str = strndup(cp, len); /* Numeric_in always converts the complete string */
	int ret = 0;
	
	if (!str)
		ret = -1201;
	else
	{
		np = PGTYPESnumeric_from_asc(str, NULL);
		if (!np)
		{
			switch (errno)
			{
				case PGTYPES_NUM_OVERFLOW:    ret = -1200;
						          break;
				case PGTYPES_NUM_BAD_NUMERIC: ret = -1213;
							  break;
				default:		  ret = -1216;
							  break;
			}
		}
	}
	
	return ret;
}

int
deccvdbl(double dbl, Numeric *np)
{
	return(PGTYPESnumeric_from_double(dbl, np));
}

int
deccvint(int in, Numeric *np)
{
	return(PGTYPESnumeric_from_int(in, np));
}

int
deccvlong(long lng, Numeric *np)
{
	return(PGTYPESnumeric_from_long(lng, np));	
}

int
decdiv(Numeric *n1, Numeric *n2, Numeric *n3)
{
	int i = PGTYPESnumeric_div(n1, n2, n3), ret = 0;

	if (i != 0)
		switch (errno)
		{
			case PGTYPES_NUM_DIVIDE_ZERO: ret = -1202;
						  break;
			case PGTYPES_NUM_OVERFLOW:    ret = -1200;
						  break;
			default:		  ret = -1201;
						  break;
		}

	return ret;
}

int 
decmul(Numeric *n1, Numeric *n2, Numeric *n3)
{
	int i = PGTYPESnumeric_mul(n1, n2, n3), ret = 0;

	if (i != 0)
		switch (errno)
		{
			case PGTYPES_NUM_OVERFLOW:    ret = -1200;
						  break;
			default:		  ret = -1201;
						  break;
		}

	return ret;
}

int
decsub(Numeric *n1, Numeric *n2, Numeric *n3)
{
	int i = PGTYPESnumeric_sub(n1, n2, n3), ret = 0;

	if (i != 0)
		switch (errno)
		{
			case PGTYPES_NUM_OVERFLOW:    ret = -1200;
						  break;
			default:		  ret = -1201;
						  break;
		}

	return ret;
}

int
dectoasc(Numeric *np, char *cp, int len, int right)
{
	char *str;
	
	if (right >= 0)
		str = PGTYPESnumeric_to_asc(np, right);
	else
		str = PGTYPESnumeric_to_asc(np, 0);

	if (!str)
		return -1;
	
	/* TODO: have to take care of len here and create exponatial notion if necessary */
	strncpy(cp, str, len);
	free (str);

	return 0;
}

int
dectodbl(Numeric *np, double *dblp)
{
	return(PGTYPESnumeric_to_double(np, dblp));
}

int
dectoint(Numeric *np, int *ip)
{
	int ret = PGTYPESnumeric_to_int(np, ip);

	if (ret == PGTYPES_NUM_OVERFLOW)
		ret = -1200;
	
	return ret;
}

int
dectolong(Numeric *np, long *lngp)	
{
	int ret = PGTYPESnumeric_to_long(np, lngp);

	if (ret == PGTYPES_NUM_OVERFLOW)
		ret = -1200;
	
	return ret;
}

/* Now the date functions */
int
rdatestr (Date d, char *str)
{
	char *tmp = PGTYPESdate_to_asc(d);

	if (!tmp)
		return -1210;
	
	/* move to user allocated buffer */
	strcpy(tmp, str);
	free(str);
	
	return 0;
}

int
rstrdate (char *str, Date *d)
{
	Date dat = PGTYPESdate_from_asc(str, NULL);

	if (errno != PGTYPES_DATE_BAD_DATE && dat == 0)
		return -1218;

	*d=dat;
	return 0;
}

void
rtoday (Date *d)
{
	PGTYPESdate_today(d);
	return;
}

int
rjulmdy (Date d, short mdy[3])
{
	PGTYPESdate_julmdy(d, (int *)mdy);
	return 0;
}

int
rdefmtdate (Date *d, char *fmt, char *str)
{
	/* TODO: take care of DBCENTURY environment variable */
	/* PGSQL functions allow all centuries */

	if (PGTYPESdate_defmt_asc(d, fmt, str) == 0)
		return 0;
	
	switch (errno)
	{
		case PGTYPES_DATE_ERR_ENOSHORTDATE: 	return -1209;
		case PGTYPES_DATE_ERR_EARGS:
		case PGTYPES_DATE_ERR_ENOTDMY: 		return -1212;
		case PGTYPES_DATE_BAD_DAY: 		return -1204;
		case PGTYPES_DATE_BAD_MONTH:		return -1205;
		default: 				return -1206; 
	}
}

int
rfmtdate (Date d, char *fmt, char *str)
{
	if (PGTYPESdate_fmt_asc(d, fmt, str) == 0)
		return 0;
		
	if (errno == ENOMEM)
		return -1211;

	return -1210;
}

int
rmdyjul (short mdy[3], Date *d)
{
	PGTYPESdate_mdyjul((int *)mdy, d);
	return 0;
}

/* And the datetime stuff */

void
dtcurrent (Timestamp *ts)
{
	PGTYPEStimestamp_current (ts);
}

int
dtcvasc (char *str, Timestamp *ts)
{
	Timestamp ts_tmp;
        int i;
        char **endptr = &str;

        ts_tmp = PGTYPEStimestamp_from_asc(str, endptr);
        i = errno;
        if (i) {
                return i;
        }
        if (**endptr) {
                /* extra characters exist at the end */
                return -1264;
        }

        /* everything went fine */
        *ts = ts_tmp;
								
	return 0;
}

int
dtsub (Timestamp *ts1, Timestamp *ts2, Interval *iv)
{
	return PGTYPEStimestamp_sub(ts1, ts2, iv);
}

int
dttoasc (Timestamp *ts, char *output)
{
	return 0;
}

int
dttofmtasc (Timestamp *ts, char *output, int str_len, char *fmtstr)
{
	return PGTYPEStimestamp_fmt_asc(ts, output, str_len, fmtstr);
}

int
intoasc(Interval *i, char *str)
{
	str = PGTYPESinterval_to_asc(i);
	
	if (!str)
		return -errno;
	
	return 0;
}

/* And finally some misc functions */
int
rfmtlong(long lvalue, char *format, char *outbuf)
{
	return 0;
}

int
rgetmsg(int msgnum, char *s, int maxsize)
{
	return 0;
}

int
risnull(int vtype, char *pcvar)
{
	return 0;
}

int
rsetnull(int vtype, char *pcvar)
{
	return 0;
}

int
rtypalign(int offset, int type)
{
	return 0;
}

int
rtypmsize(int type, int len)
{
	return 0;
}

void
rupshift(char *str)
{
	for (; *str != '\0'; str++)
		if (islower(*str)) *str = toupper(*str);
	return;
}



