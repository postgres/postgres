/*
cash.c
Written by D'Arcy J.M. Cain

Functions to allow input and output of money normally but store
and handle it as longs

Set tabstops to 4 for best results

A slightly modified version of this file and a discussion of the
workings can be found in the book "Software Solutions in C" by
Dale Schumacher, Academic Press, ISBN: 0-12-632360-7.

*/

#include	<stdio.h>
#include	<string.h>
#include	<limits.h>
#include	<ctype.h>
#include	<locale.h>

#ifdef		TEST_MAIN
# include	<stdlib.h>
# define	palloc malloc
#else
# include	<palloc.h>
#endif

#include	"cash.h"

/* when we go to 64 bit values we will have to modify this */
#define		CASH_BUFSZ	24

#define		TERMINATOR	(CASH_BUFSZ - 1)
#define		LAST_PAREN	(TERMINATOR - 1)
#define		LAST_DIGIT	(LAST_PAREN - 1)

/* function to convert a long to a dollars and cents representation */
const char *
cash_out(long value)
{
	char			*retbuf, buf[CASH_BUFSZ];
	struct lconv	*lc = localeconv();
	int				mod_group = *lc->mon_grouping;
	int				comma = *lc->mon_thousands_sep;
	int				points = lc->frac_digits;		/* int_frac_digits? */
	int				minus = 0;
	int				count = LAST_DIGIT;
	int				point_pos;
	int				comma_position = 0;

	/* frac_digits in the C locale seems to return CHAR_MAX */
	/* best guess is 2 in this case I think */
	if (points == CHAR_MAX)
		points = 2;

	point_pos = LAST_DIGIT - points;

	/* We're playing a little fast and loose with this.  Shoot me. */
	if (!mod_group || mod_group == CHAR_MAX)
		mod_group = 3;

	/* allow more than three decimal points and separate them */
	if (comma)
	{
		point_pos -= (points - 1)/mod_group;
		comma_position = point_pos % (mod_group + 1);
	}

	/* we work with positive amounts and add the minus sign at the end */
	if (value < 0)
	{
		minus = 1;
		value *= -1;
	}

	/* allow for trailing negative strings */
	memset(buf, ' ', CASH_BUFSZ);
	buf[TERMINATOR] = buf[LAST_PAREN] = 0;

	while (value || count > (point_pos - 2))
	{
		if (points && count == point_pos)
			buf[count--] = *lc->decimal_point;
		else if (comma && count % (mod_group + 1) == comma_position)
			buf[count--] = comma;

		buf[count--] = (value % 10) + '0';
		value /= 10;
	}

	if (buf[LAST_DIGIT] == ',')
		buf[LAST_DIGIT] = buf[LAST_PAREN];

	/* see if we need to signify negative amount */
	if (minus)
	{
		retbuf = palloc(CASH_BUFSZ + 2 - count + strlen(lc->negative_sign));

		/* Position code of 0 means use parens */
		if (!lc->n_sign_posn)
			sprintf(retbuf, "(%s)", buf + count);
		else if (lc->n_sign_posn == 2)
			sprintf(retbuf, "%s%s", buf + count, lc->negative_sign);
		else
			sprintf(retbuf, "%s%s", lc->negative_sign, buf + count);
	}
	else
	{
		retbuf = palloc(CASH_BUFSZ + 2 - count);
		strcpy(retbuf, buf + count);
	}

	return retbuf;
}

/* convert a string to a long integer */
long
cash_in(const char *s)
{
	long			value = 0;
	long			dec = 0;
	long			sgn = 1;
	int				seen_dot = 0;
	struct lconv	*lc = localeconv();
	int				fpoint = lc->frac_digits;		/* int_frac_digits? */

	/* we need to add all sorts of checking here.  For now just */
	/* strip all leading whitespace and any leading dollar sign */
	while (isspace(*s) || *s == '$')
		s++;

	/* a leading minus or paren signifies a negative number */
	/* again, better heuristics needed */
	if (*s == '-' || *s == '(')
	{
		sgn = -1;
		s++;
	}
	else if (*s == '+')
		s++;

	/* frac_digits in the C locale seems to return CHAR_MAX */
	/* best guess is 2 in this case I think */
	if (fpoint == CHAR_MAX)
		fpoint = 2;

	for (; ; s++)
	{
		/* we look for digits as long as we have less */
		/* than the required number of decimal places */
		if (isdigit(*s) && dec < fpoint)
		{
			value = (value * 10) + *s - '0';

			if (seen_dot)
				dec++;
		}
		else if (*s == *lc->decimal_point && !seen_dot)
			seen_dot = 1;
		else
		{
			/* round off */
			if (isdigit(*s) && *s >= '5')
				value++;

			/* adjust for less than required decimal places */
			for (; dec < fpoint; dec++)
				value *= 10;

			return(value * sgn);
		}
	}
}


/* used by cash_words_out() below */
static const char *
num_word(int value)
{
	static char	buf[128];
	static const char	*small[] = {
		"zero", "one", "two", "three", "four", "five", "six", "seven",
		"eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
		"fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty",
		"thirty", "fourty", "fifty", "sixty", "seventy", "eighty", "ninety"
	};
	const char	**big = small + 18;
	int			tu = value % 100;

	/* deal with the simple cases first */
	if (value <= 20)
		return(small[value]);

	/* is it an even multiple of 100? */
	if (!tu)
	{
		sprintf(buf, "%s hundred", small[value/100]);
		return(buf);
	}

	/* more than 99? */
	if (value > 99)
	{
		/* is it an even multiple of 10 other than 10? */
		if (value % 10 == 0 && tu > 10)
			sprintf(buf, "%s hundred %s",
				small[value/100], big[tu/10]);
		else if (tu < 20)
			sprintf(buf, "%s hundred and %s",
				small[value/100], small[tu]);
		else
			sprintf(buf, "%s hundred %s %s",
				small[value/100], big[tu/10], small[tu % 10]);
	}
	else
	{
		/* is it an even multiple of 10 other than 10? */
		if (value % 10 == 0 && tu > 10)
			sprintf(buf, "%s", big[tu/10]);
		else if (tu < 20)
			sprintf(buf, "%s", small[tu]);
		else
			sprintf(buf, "%s %s", big[tu/10], small[tu % 10]);
	}

	return(buf);
}

/* this converts a long as well but to a representation using words */
/* obviously way North American centric - sorry */
const char *
cash_words_out(long value)
{
	static char	buf[128];
	char	*p = buf;
	long	m0;
	long	m1;
	long	m2;
	long	m3;

	/* work with positive numbers */
	if (value < 0)
	{
		value *= -1;
		strcpy(buf, "minus ");
		p += 6;
	}
	else
		*buf = 0;

	m0 = value % 100;				/* cents */
	m1 = (value/100) % 1000;		/* hundreds */
	m2 = (value/100000) % 1000;		/* thousands */
	m3 = value/100000000 % 1000;	/* millions */

	if (m3)
	{
		strcat(buf, num_word(m3));
		strcat(buf, " million ");
	}

	if (m2)
	{
		strcat(buf, num_word(m2));
		strcat(buf, " thousand ");
	}

	if (m1)
		strcat(buf, num_word(m1));

	if (!*p)
		strcat(buf, "zero");

	strcat(buf, (int)(value/100) == 1 ? " dollar and " : " dollars and ");
	strcat(buf, num_word(m0));
	strcat(buf, m0 == 1 ? " cent" : " cents");
	*buf = toupper(*buf);
	return(buf);
}

#include	<stdlib.h>

#ifdef	TEST_MAIN
int
main(void)
{
	char	p[64], *q;
	long	v;			/* the long value representing the amount */
	int		d;			/* number of decimal places - default 2 */

	while (fgets(p, sizeof(p), stdin) != NULL)
	{
		if ((q = strchr(p, '\n')) != NULL)
			*q = 0;

		for (q = p; *q && !isspace(*q); q++)
			;

		v = cash_in(p);
		d = *q ? atoi(q) : 2;

		printf("%12.12s %10ld ", p, v);
		printf("%12s %s\n", cash_out(v), cash_words_out(v));
	}

	return(0);
}
#endif	/* TEST_MAIN */
