#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>

#include <ecpgtype.h>
#include <ecpg_informix.h>
#include <pgtypes_error.h>
#include <pgtypes_date.h>

char * ECPGalloc(long, int);

static int
deccall2(Decimal *arg1, Decimal *arg2, int (*ptr)(Numeric *, Numeric *))
{
	Numeric *a1, *a2;
	int i;

	if ((a1 = PGTYPESnumeric_new()) == NULL)
		return -1211;
	
	if ((a2 = PGTYPESnumeric_new()) == NULL)
	{
		PGTYPESnumeric_free(a1);
		return -1211;
	}

	if (PGTYPESnumeric_from_decimal(arg1, a1) != 0)
	{
		PGTYPESnumeric_free(a1);
		PGTYPESnumeric_free(a2);
		return -1211;
	}
	
	if (PGTYPESnumeric_from_decimal(arg2, a2) != 0)
	{
		PGTYPESnumeric_free(a1);
		PGTYPESnumeric_free(a2);
		return -1211;
	}
	
	i = (*ptr)(a1, a2);
	
	PGTYPESnumeric_free(a1);
	PGTYPESnumeric_free(a2);
	
	return (i);
}

static int
deccall3(Decimal *arg1, Decimal *arg2, Decimal *result, int (*ptr)(Numeric *, Numeric *, Numeric *))
{
	Numeric *a1, *a2, *nres;
	int i;

	if ((a1 = PGTYPESnumeric_new()) == NULL)
		return -1211;
	
	if ((a2 = PGTYPESnumeric_new()) == NULL)
	{
		PGTYPESnumeric_free(a1);
		return -1211;
	}

	if ((nres = PGTYPESnumeric_new()) == NULL)
	{
		PGTYPESnumeric_free(a1);
		PGTYPESnumeric_free(a2);
		return -1211;
	}

	if (PGTYPESnumeric_from_decimal(arg1, a1) != 0)
	{
		PGTYPESnumeric_free(a1);
		PGTYPESnumeric_free(a2);
		PGTYPESnumeric_free(nres);
		return -1211;
	}
	
	if (PGTYPESnumeric_from_decimal(arg2, a2) != 0)
	{
		PGTYPESnumeric_free(a1);
		PGTYPESnumeric_free(a2);
		PGTYPESnumeric_free(nres);
		return -1211;
	}
	
	i = (*ptr)(a1, a2, nres);
	
	if (i == 0) /* No error */
		PGTYPESnumeric_to_decimal(nres, result);

	PGTYPESnumeric_free(nres);
	PGTYPESnumeric_free(a1);
	PGTYPESnumeric_free(a2);
	
	return (i);
}
/* we start with the numeric functions */
int
decadd(Decimal *arg1, Decimal *arg2, Decimal *sum)
{
	deccall3(arg1, arg2, sum, PGTYPESnumeric_add);

	if (errno == PGTYPES_NUM_OVERFLOW)
		return -1200;
	else if (errno != 0)
		return -1201;	
	else return 0;
}

int
deccmp(Decimal *arg1, Decimal *arg2)
{
	return(deccall2(arg1, arg2, PGTYPESnumeric_cmp));
}

void
deccopy(Decimal *src, Decimal *target)
{
	memcpy(target, src, sizeof(Decimal));
}

static char *
strndup(const char *str, size_t len)
{
	int real_len = strlen(str);
	int use_len = (real_len > len) ? len : real_len;
	
	char *new = malloc(use_len + 1);

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
deccvasc(char *cp, int len, Decimal *np)
{
	char *str = strndup(cp, len); /* Decimal_in always converts the complete string */
	int ret = 0;
	Numeric *result;
	
	
	if (!str)
		ret = -1201;
	else
	{
		result = PGTYPESnumeric_from_asc(str, NULL);
		if (!result)
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
		else
		{
			if (PGTYPESnumeric_to_decimal(result, np) !=0)
				ret = -1200;

			free(result);
		}
	}
	
	free(str);
	return ret;
}

int
deccvdbl(double dbl, Decimal *np)
{
	Numeric *nres = PGTYPESnumeric_new();
	int result = 1;
	
	if (nres == NULL)
		return -1211;

	result = PGTYPESnumeric_from_double(dbl, nres);
	if (result == 0)
		result = PGTYPESnumeric_to_decimal(nres, np);

	PGTYPESnumeric_free(nres);
	return(result);
}

int
deccvint(int in, Decimal *np)
{
	Numeric *nres = PGTYPESnumeric_new();
	int result = 1;
	
	if (nres == NULL)
		return -1211;

	result = PGTYPESnumeric_from_int(in, nres);
	if (result == 0)
		result = PGTYPESnumeric_to_decimal(nres, np);

	PGTYPESnumeric_free(nres);
	return(result);
}

int
deccvlong(long lng, Decimal *np)
{
	Numeric *nres = PGTYPESnumeric_new();
	int result = 1;
	
	if (nres == NULL)
		return -1211;

	result = PGTYPESnumeric_from_long(lng, nres);
	if (result == 0)
		result = PGTYPESnumeric_to_decimal(nres, np);

	PGTYPESnumeric_free(nres);
	return(result);
}

int
decdiv(Decimal *n1, Decimal *n2, Decimal *n3)
{
	int i = deccall3(n1, n2, n3, PGTYPESnumeric_div);

	if (i != 0)
		switch (errno)
		{
			case PGTYPES_NUM_DIVIDE_ZERO: return -1202;
						  break;
			case PGTYPES_NUM_OVERFLOW:    return  -1200;
						  break;
			default:		  return -1201;
						  break;
		}

	return 0;
}

int 
decmul(Decimal *n1, Decimal *n2, Decimal *n3)
{
	int i = deccall3(n1, n2, n3, PGTYPESnumeric_mul);
	
	if (i != 0)
		switch (errno)
		{
			case PGTYPES_NUM_OVERFLOW:    return -1200;
						  break;
			default:		  return -1201;
						  break;
		}

	return 0;
}

int
decsub(Decimal *n1, Decimal *n2, Decimal *n3)
{
	int i = deccall3(n1, n2, n3, PGTYPESnumeric_sub);

	if (i != 0)
		switch (errno)
		{
			case PGTYPES_NUM_OVERFLOW:    return -1200;
						  break;
			default:		  return -1201;
						  break;
		}

	return 0;
}

int
dectoasc(Decimal *np, char *cp, int len, int right)
{
	char *str;
	Numeric *nres = PGTYPESnumeric_new();

	if (nres == NULL)
		return -1211;

	if (PGTYPESnumeric_from_decimal(np, nres) != 0)
		return -1211;
	
	if (right >= 0)
		str = PGTYPESnumeric_to_asc(nres, right);
	else
		str = PGTYPESnumeric_to_asc(nres, 0);

	PGTYPESnumeric_free(nres);
	if (!str)
		return -1;
	
	/* TODO: have to take care of len here and create exponatial notion if necessary */
	strncpy(cp, str, len);
	free (str);

	return 0;
}

int
dectodbl(Decimal *np, double *dblp)
{
	Numeric *nres = PGTYPESnumeric_new();
	int i;

	if (nres == NULL)
		return -1211;
			
	if (PGTYPESnumeric_from_decimal(np, nres) != 0)
		return -1211;
	
	i = PGTYPESnumeric_to_double(nres, dblp);
	PGTYPESnumeric_free(nres);

	return i;
}

int
dectoint(Decimal *np, int *ip)
{
	int ret;
	Numeric *nres = PGTYPESnumeric_new();

	if (nres == NULL)
		return -1211;
			
	if (PGTYPESnumeric_from_decimal(np, nres) != 0)
		return -1211;
	
	ret = PGTYPESnumeric_to_int(nres, ip);

	if (ret == PGTYPES_NUM_OVERFLOW)
		ret = -1200;
	
	return ret;
}

int
dectolong(Decimal *np, long *lngp)	
{
	int ret;
	Numeric *nres = PGTYPESnumeric_new();;

	if (nres == NULL)
		return -1211;
			
	if (PGTYPESnumeric_from_decimal(np, nres) != 0)
		return -1211;
	
	ret = PGTYPESnumeric_to_long(nres, lngp);

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

int
rdayofweek(Date d)
{
	return(PGTYPESdate_dayofweek(d));
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

/***************************************************************************
                          rfmt.c  -  description
                             -------------------
    begin                : Wed Apr 2 2003
    copyright            : (C) 2003 by Carsten Wolff
    email                : carsten.wolff@credativ.de
 ***************************************************************************/

static struct {
	long val;
	int maxdigits;
	int digits;
	int remaining;
	char sign;
	char *val_string;
} value;

/**
 * initialize the struct, wich holds the different forms
 * of the long value
 */
static void initValue(long lng_val) {
	int i, div, dig;
	char tmp[2] = " ";

	// set some obvious things
	value.val = lng_val >= 0 ? lng_val : lng_val * (-1);
	value.sign = lng_val >= 0 ? '+' : '-';
	value.maxdigits = log10(2)*(8*sizeof(long)-1);

	// determine the number of digits
	for(i=1; i <= value.maxdigits; i++) {
		if ((int)(value.val / pow(10, i)) != 0) {
			value.digits = i+1;
		}
	}
	value.remaining = value.digits;

	// convert the long to string
	value.val_string = (char *)malloc(value.digits + 1);
	for(i=value.digits; i > 0; i--) {
		div = pow(10,i);
		dig = (value.val % div) / (div / 10);
		tmp[0] = (char)(dig + 48);
		strcat(value.val_string, tmp);
    }
	// safety-net
	value.val_string[value.digits] = '\0';
	// clean up
	free(tmp);
}

/* return the position oft the right-most dot in some string */
static int getRightMostDot(char* str) {
	size_t len = strlen(str);
	int i,j;
	j=0;
	for(i=len-1; i >= 0; i--) {
		if (str[i] == '.') {
			return len-j-1;
		}
		j++;
	}
	return -1;
}

/* And finally some misc functions */
int
rfmtlong(long lng_val, char *fmt, char *outbuf)
{
	size_t fmt_len = strlen(fmt);
	size_t temp_len;
	int i, j, k, dotpos;
	int leftalign = 0, blank = 0, sign = 0, entity = 0,
	    entitydone = 0, signdone = 0, brackets_ok = 0;
	char temp[fmt_len+1], tmp[2] = " ", lastfmt = ' ', fmtchar = ' ';

	// put all info about the long in a struct
	initValue(lng_val);

	// '<' is the only format, where we have to align left
	if (strchr(fmt, (int)'<')) {
		leftalign = 1;
	}

	// '(' requires ')'
	if (strchr(fmt, (int)'(') && strchr(fmt, (int)')')) {
		brackets_ok = 1;
	}

	// get position of the right-most dot in the format-string
	// and fill the temp-string wit '0's up to there.
	dotpos = getRightMostDot(fmt);

	// start to parse the formatstring
	temp[0] = '\0';
	j = 0;                  // position in temp
	k = value.digits - 1;   // position in the value_string
	for(i=fmt_len-1, j=0; i>=0; i--, j++) {
		// qualify, where we are in the value_string
		if (k < 0) {
			if (leftalign) {
				// can't use strncat(,,0) here, Solaris would freek out
				temp[j] = '\0';
				break;
			}
			blank = 1;
			if (k == -2) {
				entity = 1;
			}
			else if (k == -1) {
				sign = 1;
			}
		}
		// if we're right side of the right-most dot, print '0'
		if (dotpos >= 0 && dotpos <= i) {
			if (dotpos < i) {
				if (fmt[i] == ')') tmp[0] = value.sign == '-' ? ')' : ' ';
				else tmp[0] = '0';
			}
			else {
				tmp[0] = '.';
			}
			strcat(temp, tmp);
			continue;
		}
		// the ',' needs special attention, if it is in the blank area
		if (blank && fmt[i] == ',') fmtchar = lastfmt;
		else fmtchar = fmt[i];
		// analyse this format-char
		switch(fmtchar) {
			case ',':
				tmp[0] = ',';
				k++;
				break;
			case '*':
				if (blank) tmp[0] = '*';
				else tmp[0] = value.val_string[k];
				break;
			case '&':
				if (blank) tmp[0] = '0';
				else tmp[0] = value.val_string[k];
				break;
			case '#':
				if (blank) tmp[0] = ' ';
				else tmp[0] = value.val_string[k];
				break;
			case '<':
				tmp[0] = value.val_string[k];
				break;
			case '-':
				if (sign && value.sign == '-' && !signdone) {
					tmp[0] = '-';
					signdone = 1;
				}
				else if (blank) tmp[0] = ' ';
				else tmp[0] = value.val_string[k];
				break;
			case '+':
				if (sign && !signdone) {
					tmp[0] = value.sign;
					signdone = 1;
				}
				else if (blank) tmp[0] = ' ';
				else tmp[0] = value.val_string[k];
				break;
			case '(':
				if (sign && brackets_ok && value.sign == '-') tmp[0] = '(';
				else if (blank) tmp[0] = ' ';
				else tmp[0] = value.val_string[k];
				break;
			case ')':
				if (brackets_ok && value.sign == '-') tmp[0] = ')';
				else tmp[0] = ' ';
				break;
			case '$':
				if (blank && !entitydone) {
					tmp[0] = '$';
					entitydone = 1;
				}
				else if (blank) tmp[0] = ' ';
				else tmp[0] = value.val_string[k];
				break;
			default: tmp[0] = fmt[i];
		}
		strcat(temp, tmp);
		lastfmt = fmt[i];
		k--;
	}
	// safety-net
	temp[fmt_len] = '\0';

	// reverse the temp-string and put it into the outbuf
	temp_len = strlen(temp);
	outbuf[0] = '\0';
	for(i=temp_len-1; i>=0; i--) {
		tmp[0] = temp[i];
		strcat(outbuf, tmp); 
	}
	outbuf[temp_len] = '\0';

	// cleaning up
	free(tmp);
	free(value.val_string);

	return 0;
}

void
rupshift(char *str)
{
	for (; *str != '\0'; str++)
		if (islower(*str)) *str = toupper(*str);
	return;
}

int
byleng(char *str, int len)
{
        for (len--; str[len] && str[len] == ' '; len--);
        return (len+1);
}

void
ldchar(char *src, int len, char *dest)
{
        memmove(dest, src, len);
        dest[len]=0;
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

int
dtcvfmtasc (char *inbuf, char *fmtstr, dtime_t *dtvalue)
{
	return 0;
}

static struct var_list
{
	int number;
	void *pointer;
	struct var_list *next;
} *ivlist = NULL;

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
	ptr = (struct var_list *) ECPGalloc (sizeof(struct var_list), lineno);
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

int rsetnull(int t, char *ptr)
{
	ECPGset_informix_null(t, ptr);
	return 0;
}

int risnull(int t, char *ptr)
{
	return(ECPGis_informix_null(t, ptr));
}

