#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>

#include <ecpg_informix.h>
#include <pgtypes_error.h>
#include <pgtypes_date.h>

char * ECPGalloc(long, int);

/* we start with the numeric functions */
int
decadd(Numeric *arg1, Numeric *arg2, Numeric *sum)
{
	Numeric *temp_sum = malloc(sizeof(Numeric)) ;
	int i;
	
	if (temp_sum == NULL)
		return -1211;
	
	i = PGTYPESnumeric_add(arg1, arg2, temp_sum);

	if (i == 0) /* No error */
	{

		if (PGTYPESnumeric_copy(temp_sum, sum) !=0)
			return -1211;

		free(temp_sum);
		return 0;
	}
	else
	{
		free(temp_sum);
		
		if (errno == PGTYPES_NUM_OVERFLOW)
			return -1200;
	}

	return -1201;	
}

int
deccmp(Numeric *arg1, Numeric *arg2)
{
	int i = PGTYPESnumeric_cmp(arg1, arg2);
	
	return (i);
}

void
deccopy(Numeric *src, Numeric *target)
{
	PGTYPESnumeric_copy(src, target);
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
deccvasc(char *cp, int len, Numeric *np)
{
	char *str = strndup(cp, len); /* Numeric_in always converts the complete string */
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
			if (PGTYPESnumeric_copy(result, np) !=0)
				ret = -1211;

			free(result);
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
	Numeric *temp = malloc(sizeof(Numeric));
	int i, ret = 0;

	if (temp == NULL)
		return -1211;
	
	i = PGTYPESnumeric_div(n1, n2, temp);

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
	else
		if (PGTYPESnumeric_copy(temp, n3) !=0)
			ret = -1211;
		
	free(temp);
	return ret;
}

int 
decmul(Numeric *n1, Numeric *n2, Numeric *n3)
{
	Numeric *temp = malloc(sizeof(Numeric));
	int i, ret = 0;
	
	if (temp == NULL)
		return -1211;
	
	i = PGTYPESnumeric_mul(n1, n2, temp);

	if (i != 0)
		switch (errno)
		{
			case PGTYPES_NUM_OVERFLOW:    ret = -1200;
						  break;
			default:		  ret = -1201;
						  break;
		}
	else
		if (PGTYPESnumeric_copy(temp, n3) !=0)
			ret = -1211;
		
	free(temp);

	return ret;
}

int
decsub(Numeric *n1, Numeric *n2, Numeric *n3)
{
	Numeric *temp = malloc(sizeof(Numeric));
	int i, ret = 0;
	
	if (temp == NULL)
		return -1211;
	
	i = PGTYPESnumeric_sub(n1, n2, temp);

	if (i != 0)
		switch (errno)
		{
			case PGTYPES_NUM_OVERFLOW:    ret = -1200;
						  break;
			default:		  ret = -1201;
						  break;
		}
	else
		if (PGTYPESnumeric_copy(temp, n3) !=0)
			ret = -1211;
		
	free(temp);

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

bool
ECPGconnect_informix(int lineno, const char *name, const char *user, const char *passwd, const char *connection_name, int autocommit)
{
        char *informix_name = (char *)name, *envname;

        /* Informix uses an environment variable DBPATH that overrides
	 * the connection parameters given here.
	 * We do the same with PG_DBPATH as the syntax is different. */
        envname = getenv("PG_DBPATH");
        if (envname)
                informix_name = envname;
	        return (ECPGconnect(lineno, informix_name, user, passwd, connection_name , autocommit));
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

