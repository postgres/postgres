/*
 *	PostgreSQL type definitions for ISSNs.
 *
 *	$Id: issn.c,v 1.1 1998/08/17 03:35:05 scrappy Exp $
 */

#include <stdio.h>

#include <postgres.h>
#include <utils/palloc.h>

/*
 *	This is the internal storage format for ISSNs.
 *	NB: This is an intentional type pun with builtin type `char16'.
 */

typedef struct issn
{
	char	num[9];
	char	pad[7];
}			issn;

/*
 *	Various forward declarations:
 */

issn	   *issn_in(char *str);
char	   *issn_out(issn * addr);

bool		issn_lt(issn * a1, issn * a2);
bool		issn_le(issn * a1, issn * a2);
bool		issn_eq(issn * a1, issn * a2);
bool		issn_ge(issn * a1, issn * a2);
bool		issn_gt(issn * a1, issn * a2);

bool		issn_ne(issn * a1, issn * a2);

int4		issn_cmp(issn * a1, issn * a2);

int4		issn_sum(char *str);

/*
 *	ISSN reader.
 */

issn *
issn_in(char *str)
{
	issn	   *result;
	char	   *cp;
	int			count;

	if (strlen(str) != 9) {
		elog(ERROR, "issn_in: invalid ISSN \"%s\"", str);
		return (NULL);
	}
	if (issn_sum(str) != 0) {
		elog(ERROR, "issn_in: purported ISSN \"%s\" failed checksum",
		     str);
		return (NULL);
	}

	result = (issn *) palloc(sizeof(issn));

	strncpy(result->num, str, 9);
	memset(result->pad, ' ', 7);
	return (result);
}

/*
 * The ISSN checksum works just like the ISBN sum, only different
 * (of course!).
 * Here, the weights start at 8 and decrease.
 */
int4
issn_sum(char *str)
{
	int4 sum = 0, dashes = 0, val;
	int i;

	for (i = 0; str[i] && i < 9; i++) {
		switch(str[i]) {
		case '-':
			if (++dashes > 1)
				return 12;
			continue;

		case '0': case '1': case '2': case '3':
		case '4': case '5': case '6': case '7':
		case '8': case '9':
			val = str[i] - '0';
			break;

		case 'X': case 'x':
			val = 10;
			break;

		default:
			return 12;
		}

		sum += val * (8 - (i - dashes));
	}
	return (sum % 11);
}

/*
 *	ISSN output function.
 */

char *
issn_out(issn * num)
{
	char	   *result;

	if (num == NULL)
		return (NULL);

	result = (char *) palloc(14);

	result[0] = '\0';
	strncat(result, num->num, 9);
	return (result);
}

/*
 *	Boolean tests for magnitude.
 */

bool
issn_lt(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9) < 0);
};

bool
issn_le(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9) <= 0);
};

bool
issn_eq(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9) == 0);
};

bool
issn_ge(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9) >= 0);
};

bool
issn_gt(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9) > 0);
};

bool
issn_ne(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9) != 0);
};

/*
 *	Comparison function for sorting:
 */

int4
issn_cmp(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9));
}

/*
 *	eof
 */
