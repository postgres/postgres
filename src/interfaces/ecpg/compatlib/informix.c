/* $PostgreSQL: pgsql/src/interfaces/ecpg/compatlib/informix.c,v 1.48 2006/10/04 00:30:11 momjian Exp $ */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>

#include <ecpgtype.h>
#include <ecpg_informix.h>
#include <pgtypes_error.h>
#include <pgtypes_date.h>
#include <pgtypes_numeric.h>
#include <sqltypes.h>

char	   *ECPGalloc(long, int);

static int
deccall2(decimal *arg1, decimal *arg2, int (*ptr) (numeric *, numeric *))
{
	numeric    *a1,
			   *a2;
	int			i;

	if ((a1 = PGTYPESnumeric_new()) == NULL)
		return ECPG_INFORMIX_OUT_OF_MEMORY;

	if ((a2 = PGTYPESnumeric_new()) == NULL)
	{
		PGTYPESnumeric_free(a1);
		return ECPG_INFORMIX_OUT_OF_MEMORY;
	}

	if (PGTYPESnumeric_from_decimal(arg1, a1) != 0)
	{
		PGTYPESnumeric_free(a1);
		PGTYPESnumeric_free(a2);
		return ECPG_INFORMIX_OUT_OF_MEMORY;
	}

	if (PGTYPESnumeric_from_decimal(arg2, a2) != 0)
	{
		PGTYPESnumeric_free(a1);
		PGTYPESnumeric_free(a2);
		return ECPG_INFORMIX_OUT_OF_MEMORY;
	}

	i = (*ptr) (a1, a2);

	PGTYPESnumeric_free(a1);
	PGTYPESnumeric_free(a2);

	return (i);
}

static int
deccall3(decimal *arg1, decimal *arg2, decimal *result, int (*ptr) (numeric *, numeric *, numeric *))
{
	numeric    *a1,
			   *a2,
			   *nres;
	int			i;

	/*
	 * we must NOT set the result to NULL here because it may be the same
	 * variable as one of the arguments
	 */
	if (risnull(CDECIMALTYPE, (char *) arg1) || risnull(CDECIMALTYPE, (char *) arg2))
		return 0;

	if ((a1 = PGTYPESnumeric_new()) == NULL)
		return ECPG_INFORMIX_OUT_OF_MEMORY;

	if ((a2 = PGTYPESnumeric_new()) == NULL)
	{
		PGTYPESnumeric_free(a1);
		return ECPG_INFORMIX_OUT_OF_MEMORY;
	}

	if ((nres = PGTYPESnumeric_new()) == NULL)
	{
		PGTYPESnumeric_free(a1);
		PGTYPESnumeric_free(a2);
		return ECPG_INFORMIX_OUT_OF_MEMORY;
	}

	if (PGTYPESnumeric_from_decimal(arg1, a1) != 0)
	{
		PGTYPESnumeric_free(a1);
		PGTYPESnumeric_free(a2);
		PGTYPESnumeric_free(nres);
		return ECPG_INFORMIX_OUT_OF_MEMORY;
	}

	if (PGTYPESnumeric_from_decimal(arg2, a2) != 0)
	{
		PGTYPESnumeric_free(a1);
		PGTYPESnumeric_free(a2);
		PGTYPESnumeric_free(nres);
		return ECPG_INFORMIX_OUT_OF_MEMORY;
	}

	i = (*ptr) (a1, a2, nres);

	if (i == 0)					/* No error */
	{

		/* set the result to null in case it errors out later */
		rsetnull(CDECIMALTYPE, (char *) result);
		PGTYPESnumeric_to_decimal(nres, result);
	}

	PGTYPESnumeric_free(nres);
	PGTYPESnumeric_free(a1);
	PGTYPESnumeric_free(a2);

	return (i);
}

/* we start with the numeric functions */
int
decadd(decimal *arg1, decimal *arg2, decimal *sum)
{
	errno = 0;
	deccall3(arg1, arg2, sum, PGTYPESnumeric_add);

	if (errno == PGTYPES_NUM_OVERFLOW)
		return ECPG_INFORMIX_NUM_OVERFLOW;
	else if (errno == PGTYPES_NUM_UNDERFLOW)
		return ECPG_INFORMIX_NUM_UNDERFLOW;
	else if (errno != 0)
		return -1;
	else
		return 0;
}

int
deccmp(decimal *arg1, decimal *arg2)
{
	return (deccall2(arg1, arg2, PGTYPESnumeric_cmp));
}

void
deccopy(decimal *src, decimal *target)
{
	memcpy(target, src, sizeof(decimal));
}

static char *
ecpg_strndup(const char *str, size_t len)
{
	int			real_len = strlen(str);
	int			use_len = (real_len > len) ? len : real_len;

	char	   *new = malloc(use_len + 1);

	if (new)
	{
		memcpy(new, str, use_len);
		new[use_len] = '\0';
	}
	else
		errno = ENOMEM;

	return new;
}

int
deccvasc(char *cp, int len, decimal *np)
{
	char	   *str;
	int			ret = 0;
	numeric    *result;

	rsetnull(CDECIMALTYPE, (char *) np);
	if (risnull(CSTRINGTYPE, cp))
		return 0;

	str = ecpg_strndup(cp, len);/* decimal_in always converts the complete
								 * string */
	if (!str)
		ret = ECPG_INFORMIX_NUM_UNDERFLOW;
	else
	{
		errno = 0;
		result = PGTYPESnumeric_from_asc(str, NULL);
		if (!result)
		{
			switch (errno)
			{
				case PGTYPES_NUM_OVERFLOW:
					ret = ECPG_INFORMIX_NUM_OVERFLOW;
					break;
				case PGTYPES_NUM_BAD_NUMERIC:
					ret = ECPG_INFORMIX_BAD_NUMERIC;
					break;
				default:
					ret = ECPG_INFORMIX_BAD_EXPONENT;
					break;
			}
		}
		else
		{
			if (PGTYPESnumeric_to_decimal(result, np) != 0)
				ret = ECPG_INFORMIX_NUM_OVERFLOW;

			free(result);
		}
	}

	free(str);
	return ret;
}

int
deccvdbl(double dbl, decimal *np)
{
	numeric    *nres;
	int			result = 1;

	rsetnull(CDECIMALTYPE, (char *) np);
	if (risnull(CDOUBLETYPE, (char *) &dbl))
		return 0;

	nres = PGTYPESnumeric_new();
	if (nres == NULL)
		return ECPG_INFORMIX_OUT_OF_MEMORY;

	result = PGTYPESnumeric_from_double(dbl, nres);
	if (result == 0)
		result = PGTYPESnumeric_to_decimal(nres, np);

	PGTYPESnumeric_free(nres);
	return (result);
}

int
deccvint(int in, decimal *np)
{
	numeric    *nres;
	int			result = 1;

	rsetnull(CDECIMALTYPE, (char *) np);
	if (risnull(CINTTYPE, (char *) &in))
		return 0;

	nres = PGTYPESnumeric_new();
	if (nres == NULL)
		return ECPG_INFORMIX_OUT_OF_MEMORY;

	result = PGTYPESnumeric_from_int(in, nres);
	if (result == 0)
		result = PGTYPESnumeric_to_decimal(nres, np);

	PGTYPESnumeric_free(nres);
	return (result);
}

int
deccvlong(long lng, decimal *np)
{
	numeric    *nres;
	int			result = 1;

	rsetnull(CDECIMALTYPE, (char *) np);
	if (risnull(CLONGTYPE, (char *) &lng))
		return 0;

	nres = PGTYPESnumeric_new();
	if (nres == NULL)
		return ECPG_INFORMIX_OUT_OF_MEMORY;

	result = PGTYPESnumeric_from_long(lng, nres);
	if (result == 0)
		result = PGTYPESnumeric_to_decimal(nres, np);

	PGTYPESnumeric_free(nres);
	return (result);
}

int
decdiv(decimal *n1, decimal *n2, decimal *result)
{

	int			i;

	errno = 0;
	i = deccall3(n1, n2, result, PGTYPESnumeric_div);

	if (i != 0)
		switch (errno)
		{
			case PGTYPES_NUM_DIVIDE_ZERO:
				return ECPG_INFORMIX_DIVIDE_ZERO;
				break;
			case PGTYPES_NUM_OVERFLOW:
				return ECPG_INFORMIX_NUM_OVERFLOW;
				break;
			default:
				return ECPG_INFORMIX_NUM_UNDERFLOW;
				break;
		}

	return 0;
}

int
decmul(decimal *n1, decimal *n2, decimal *result)
{
	int			i;

	errno = 0;
	i = deccall3(n1, n2, result, PGTYPESnumeric_mul);

	if (i != 0)
		switch (errno)
		{
			case PGTYPES_NUM_OVERFLOW:
				return ECPG_INFORMIX_NUM_OVERFLOW;
				break;
			default:
				return ECPG_INFORMIX_NUM_UNDERFLOW;
				break;
		}

	return 0;
}

int
decsub(decimal *n1, decimal *n2, decimal *result)
{
	int			i;

	errno = 0;
	i = deccall3(n1, n2, result, PGTYPESnumeric_sub);

	if (i != 0)
		switch (errno)
		{
			case PGTYPES_NUM_OVERFLOW:
				return ECPG_INFORMIX_NUM_OVERFLOW;
				break;
			default:
				return ECPG_INFORMIX_NUM_UNDERFLOW;
				break;
		}

	return 0;
}

int
dectoasc(decimal *np, char *cp, int len, int right)
{
	char	   *str;
	numeric    *nres;

	rsetnull(CSTRINGTYPE, (char *) cp);
	if (risnull(CDECIMALTYPE, (char *) np))
		return 0;

	nres = PGTYPESnumeric_new();
	if (nres == NULL)
		return ECPG_INFORMIX_OUT_OF_MEMORY;

	if (PGTYPESnumeric_from_decimal(np, nres) != 0)
	{
		PGTYPESnumeric_free(nres);
		return ECPG_INFORMIX_OUT_OF_MEMORY;
	}

	if (right >= 0)
		str = PGTYPESnumeric_to_asc(nres, right);
	else
		str = PGTYPESnumeric_to_asc(nres, nres->dscale);

	PGTYPESnumeric_free(nres);
	if (!str)
		return -1;

	/*
	 * TODO: have to take care of len here and create exponential notation if
	 * necessary
	 */
	if ((int) (strlen(str) + 1) > len)
	{
		if (len > 1)
		{
			cp[0] = '*';
			cp[1] = '\0';
		}
		free(str);
		return -1;
	}
	else
	{
		strcpy(cp, str);
		free(str);
		return 0;
	}
}

int
dectodbl(decimal *np, double *dblp)
{
	int			i;
	numeric    *nres = PGTYPESnumeric_new();

	if (nres == NULL)
		return ECPG_INFORMIX_OUT_OF_MEMORY;

	if (PGTYPESnumeric_from_decimal(np, nres) != 0)
	{
		PGTYPESnumeric_free(nres);
		return ECPG_INFORMIX_OUT_OF_MEMORY;
	}

	i = PGTYPESnumeric_to_double(nres, dblp);
	PGTYPESnumeric_free(nres);

	return i;
}

int
dectoint(decimal *np, int *ip)
{
	int			ret;
	numeric    *nres = PGTYPESnumeric_new();

	if (nres == NULL)
		return ECPG_INFORMIX_OUT_OF_MEMORY;

	if (PGTYPESnumeric_from_decimal(np, nres) != 0)
	{
		PGTYPESnumeric_free(nres);
		return ECPG_INFORMIX_OUT_OF_MEMORY;
	}

	ret = PGTYPESnumeric_to_int(nres, ip);
	PGTYPESnumeric_free(nres);

	if (ret == PGTYPES_NUM_OVERFLOW)
		ret = ECPG_INFORMIX_NUM_OVERFLOW;

	return ret;
}

int
dectolong(decimal *np, long *lngp)
{
	int			ret;
	numeric    *nres = PGTYPESnumeric_new();

	if (nres == NULL)
		return ECPG_INFORMIX_OUT_OF_MEMORY;

	if (PGTYPESnumeric_from_decimal(np, nres) != 0)
	{
		PGTYPESnumeric_free(nres);
		return ECPG_INFORMIX_OUT_OF_MEMORY;
	}

	ret = PGTYPESnumeric_to_long(nres, lngp);
	PGTYPESnumeric_free(nres);

	if (ret == PGTYPES_NUM_OVERFLOW)
		ret = ECPG_INFORMIX_NUM_OVERFLOW;

	return ret;
}

/* Now the date functions */
int
rdatestr(date d, char *str)
{
	char	   *tmp = PGTYPESdate_to_asc(d);

	if (!tmp)
		return ECPG_INFORMIX_DATE_CONVERT;

	/* move to user allocated buffer */
	strcpy(str, tmp);
	free(tmp);

	return 0;
}

/*
*
* the input for this function is mmddyyyy and any non-numeric
* character can be used as a separator
*
*/
int
rstrdate(char *str, date * d)
{
	return rdefmtdate(d, "mm/dd/yyyy", str);
}

void
rtoday(date * d)
{
	PGTYPESdate_today(d);
	return;
}

int
rjulmdy(date d, short mdy[3])
{
	int			mdy_int[3];

	PGTYPESdate_julmdy(d, mdy_int);
	mdy[0] = (short) mdy_int[0];
	mdy[1] = (short) mdy_int[1];
	mdy[2] = (short) mdy_int[2];
	return 0;
}

int
rdefmtdate(date * d, char *fmt, char *str)
{
	/* TODO: take care of DBCENTURY environment variable */
	/* PGSQL functions allow all centuries */

	errno = 0;
	if (PGTYPESdate_defmt_asc(d, fmt, str) == 0)
		return 0;

	switch (errno)
	{
		case PGTYPES_DATE_ERR_ENOSHORTDATE:
			return ECPG_INFORMIX_ENOSHORTDATE;
		case PGTYPES_DATE_ERR_EARGS:
		case PGTYPES_DATE_ERR_ENOTDMY:
			return ECPG_INFORMIX_ENOTDMY;
		case PGTYPES_DATE_BAD_DAY:
			return ECPG_INFORMIX_BAD_DAY;
		case PGTYPES_DATE_BAD_MONTH:
			return ECPG_INFORMIX_BAD_MONTH;
		default:
			return ECPG_INFORMIX_BAD_YEAR;
	}
}

int
rfmtdate(date d, char *fmt, char *str)
{
	errno = 0;
	if (PGTYPESdate_fmt_asc(d, fmt, str) == 0)
		return 0;

	if (errno == ENOMEM)
		return ECPG_INFORMIX_OUT_OF_MEMORY;

	return ECPG_INFORMIX_DATE_CONVERT;
}

int
rmdyjul(short mdy[3], date * d)
{
	int			mdy_int[3];

	mdy_int[0] = mdy[0];
	mdy_int[1] = mdy[1];
	mdy_int[2] = mdy[2];
	PGTYPESdate_mdyjul(mdy_int, d);
	return 0;
}

int
rdayofweek(date d)
{
	return (PGTYPESdate_dayofweek(d));
}

/* And the datetime stuff */

void
dtcurrent(timestamp * ts)
{
	PGTYPEStimestamp_current(ts);
}

int
dtcvasc(char *str, timestamp * ts)
{
	timestamp	ts_tmp;
	int			i;
	char	  **endptr = &str;

	errno = 0;
	ts_tmp = PGTYPEStimestamp_from_asc(str, endptr);
	i = errno;
	if (i)
		/* TODO: rewrite to Informix error codes */
		return i;
	if (**endptr)
	{
		/* extra characters exist at the end */
		return ECPG_INFORMIX_EXTRA_CHARS;
	}
	/* TODO: other Informix error codes missing */

	/* everything went fine */
	*ts = ts_tmp;

	return 0;
}

int
dtcvfmtasc(char *inbuf, char *fmtstr, timestamp * dtvalue)
{
	return PGTYPEStimestamp_defmt_asc(inbuf, fmtstr, dtvalue);
}

int
dtsub(timestamp * ts1, timestamp * ts2, interval * iv)
{
	return PGTYPEStimestamp_sub(ts1, ts2, iv);
}

int
dttoasc(timestamp * ts, char *output)
{
	char	   *asctime = PGTYPEStimestamp_to_asc(*ts);

	strcpy(output, asctime);
	free(asctime);
	return 0;
}

int
dttofmtasc(timestamp * ts, char *output, int str_len, char *fmtstr)
{
	return PGTYPEStimestamp_fmt_asc(ts, output, str_len, fmtstr);
}

int
intoasc(interval * i, char *str)
{
	errno = 0;
	str = PGTYPESinterval_to_asc(i);

	if (!str)
		return -errno;

	return 0;
}

/*
 *	rfmt.c	-  description
 *	by Carsten Wolff <carsten.wolff@credativ.de>, Wed Apr 2 2003
 */

static struct
{
	long		val;
	int			maxdigits;
	int			digits;
	int			remaining;
	char		sign;
	char	   *val_string;
}	value;

/**
 * initialize the struct, which holds the different forms
 * of the long value
 */
static void
initValue(long lng_val)
{
	int			i,
				j;
	long		l,
				dig;

	/* set some obvious things */
	value.val = lng_val >= 0 ? lng_val : lng_val * (-1);
	value.sign = lng_val >= 0 ? '+' : '-';
	value.maxdigits = log10(2) * (8 * sizeof(long) - 1);

	/* determine the number of digits */
	i = 0;
	l = 1;
	do
	{
		i++;
		l *= 10;
	}
	while ((l - 1) < value.val && l <= LONG_MAX / 10);

	if (l <= LONG_MAX / 10)
	{
		value.digits = i;
		l /= 10;
	}
	else
		value.digits = i + 1;

	value.remaining = value.digits;

	/* convert the long to string */
	value.val_string = (char *) malloc(value.digits + 1);
	dig = value.val;
	for (i = value.digits, j = 0; i > 0; i--, j++)
	{
		value.val_string[j] = dig / l + '0';
		dig = dig % l;
		l /= 10;
	}
	value.val_string[value.digits] = '\0';
}

/* return the position oft the right-most dot in some string */
static int
getRightMostDot(char *str)
{
	size_t		len = strlen(str);
	int			i,
				j;

	j = 0;
	for (i = len - 1; i >= 0; i--)
	{
		if (str[i] == '.')
			return len - j - 1;
		j++;
	}
	return -1;
}

/* And finally some misc functions */
int
rfmtlong(long lng_val, char *fmt, char *outbuf)
{
	size_t		fmt_len = strlen(fmt);
	size_t		temp_len;
	int			i,
				j,
				k,
				dotpos;
	int			leftalign = 0,
				blank = 0,
				sign = 0,
				entity = 0,
				entitydone = 0,
				signdone = 0,
				brackets_ok = 0;
	char	   *temp;
	char		tmp[2] = " ";
	char		lastfmt = ' ',
				fmtchar = ' ';

	temp = (char *) malloc(fmt_len + 1);

	/* put all info about the long in a struct */
	initValue(lng_val);

	/* '<' is the only format, where we have to align left */
	if (strchr(fmt, (int) '<'))
		leftalign = 1;

	/* '(' requires ')' */
	if (strchr(fmt, (int) '(') && strchr(fmt, (int) ')'))
		brackets_ok = 1;

	/* get position of the right-most dot in the format-string */
	/* and fill the temp-string wit '0's up to there. */
	dotpos = getRightMostDot(fmt);

	/* start to parse the formatstring */
	temp[0] = '\0';
	j = 0;						/* position in temp */
	k = value.digits - 1;		/* position in the value_string */
	for (i = fmt_len - 1, j = 0; i >= 0; i--, j++)
	{
		/* qualify, where we are in the value_string */
		if (k < 0)
		{
			blank = 1;
			if (k == -2)
				entity = 1;
			else if (k == -1)
				sign = 1;
			if (leftalign)
			{
				/* can't use strncat(,,0) here, Solaris would freek out */
				if (sign)
					if (signdone)
					{
						temp[j] = '\0';
						break;
					}
			}
		}
		/* if we're right side of the right-most dot, print '0' */
		if (dotpos >= 0 && dotpos <= i)
		{
			if (dotpos < i)
			{
				if (fmt[i] == ')')
					tmp[0] = value.sign == '-' ? ')' : ' ';
				else
					tmp[0] = '0';
			}
			else
				tmp[0] = '.';
			strcat(temp, tmp);
			continue;
		}
		/* the ',' needs special attention, if it is in the blank area */
		if (blank && fmt[i] == ',')
			fmtchar = lastfmt;
		else
			fmtchar = fmt[i];
		/* waiting for the sign */
		if (k < 0 && leftalign && sign && !signdone && fmtchar != '+' && fmtchar != '-')
			continue;
		/* analyse this format-char */
		switch (fmtchar)
		{
			case ',':
				tmp[0] = ',';
				k++;
				break;
			case '*':
				if (blank)
					tmp[0] = '*';
				else
					tmp[0] = value.val_string[k];
				break;
			case '&':
				if (blank)
					tmp[0] = '0';
				else
					tmp[0] = value.val_string[k];
				break;
			case '#':
				if (blank)
					tmp[0] = ' ';
				else
					tmp[0] = value.val_string[k];
				break;
			case '-':
				if (sign && value.sign == '-' && !signdone)
				{
					tmp[0] = '-';
					signdone = 1;
				}
				else if (blank)
					tmp[0] = ' ';
				else
					tmp[0] = value.val_string[k];
				break;
			case '+':
				if (sign && !signdone)
				{
					tmp[0] = value.sign;
					signdone = 1;
				}
				else if (blank)
					tmp[0] = ' ';
				else
					tmp[0] = value.val_string[k];
				break;
			case '(':
				if (sign && brackets_ok && value.sign == '-')
					tmp[0] = '(';
				else if (blank)
					tmp[0] = ' ';
				else
					tmp[0] = value.val_string[k];
				break;
			case ')':
				if (brackets_ok && value.sign == '-')
					tmp[0] = ')';
				else
					tmp[0] = ' ';
				break;
			case '$':
				if (blank && !entitydone)
				{
					tmp[0] = '$';
					entitydone = 1;
				}
				else if (blank)
					tmp[0] = ' ';
				else
					tmp[0] = value.val_string[k];
				break;
			case '<':
				tmp[0] = value.val_string[k];
				break;
			default:
				tmp[0] = fmt[i];
		}
		strcat(temp, tmp);
		lastfmt = fmt[i];
		k--;
	}
	/* safety-net */
	temp[fmt_len] = '\0';

	/* reverse the temp-string and put it into the outbuf */
	temp_len = strlen(temp);
	outbuf[0] = '\0';
	for (i = temp_len - 1; i >= 0; i--)
	{
		tmp[0] = temp[i];
		strcat(outbuf, tmp);
	}
	outbuf[temp_len] = '\0';

	/* cleaning up */
	free(temp);
	free(value.val_string);

	return 0;
}

void
rupshift(char *str)
{
	for (; *str != '\0'; str++)
		if (islower((unsigned char) *str))
			*str = toupper((unsigned char) *str);
	return;
}

int
byleng(char *str, int len)
{
	for (len--; str[len] && str[len] == ' '; len--);
	return (len + 1);
}

void
ldchar(char *src, int len, char *dest)
{
	int			dlen = byleng(src, len);

	memmove(dest, src, dlen);
	dest[dlen] = '\0';
}

int
rgetmsg(int msgnum, char *s, int maxsize)
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

int
rtypwidth(int sqltype, int sqllen)
{
	return 0;
}

static struct var_list
{
	int			number;
	void	   *pointer;
	struct var_list *next;
}	*ivlist = NULL;

void
ECPG_informix_set_var(int number, void *pointer, int lineno)
{
	struct var_list *ptr;

	for (ptr = ivlist; ptr != NULL; ptr = ptr->next)
	{
		if (ptr->number == number)
		{
			/* already known => just change pointer value */
			ptr->pointer = pointer;
			return;
		}
	}

	/* a new one has to be added */
	ptr = (struct var_list *) ECPGalloc(sizeof(struct var_list), lineno);
	ptr->number = number;
	ptr->pointer = pointer;
	ptr->next = ivlist;
	ivlist = ptr;
}

void *
ECPG_informix_get_var(int number)
{
	struct var_list *ptr;

	for (ptr = ivlist; ptr != NULL && ptr->number != number; ptr = ptr->next);
	return (ptr) ? ptr->pointer : NULL;
}

int
rsetnull(int t, char *ptr)
{
	ECPGset_noind_null(t, ptr);
	return 0;
}

int
risnull(int t, char *ptr)
{
	return (ECPGis_noind_null(t, ptr));
}
