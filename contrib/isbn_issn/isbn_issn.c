/*
 *	PostgreSQL type definitions for ISBNs.
 *
 *	$Id: isbn_issn.c,v 1.6 2003/07/24 17:52:29 tgl Exp $
 */

#include "postgres.h"


/*
 *	This is the internal storage format for ISBNs.
 *	NB: This is an intentional type pun with builtin type `char16'.
 */

typedef struct isbn
{
	char		num[13];
	char		pad[3];
}	isbn;

/*
 *	Various forward declarations:
 */

isbn	   *isbn_in(char *str);
char	   *isbn_out(isbn * addr);

bool		isbn_lt(isbn * a1, isbn * a2);
bool		isbn_le(isbn * a1, isbn * a2);
bool		isbn_eq(isbn * a1, isbn * a2);
bool		isbn_ge(isbn * a1, isbn * a2);
bool		isbn_gt(isbn * a1, isbn * a2);

bool		isbn_ne(isbn * a1, isbn * a2);

int4		isbn_cmp(isbn * a1, isbn * a2);

int4		isbn_sum(char *str);

/*
 *	ISBN reader.
 */

isbn *
isbn_in(char *str)
{
	isbn	   *result;

	if (strlen(str) != 13)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid ISBN: \"%s\"", str),
				 errdetail("incorrect length")));

		return (NULL);
	}
	if (isbn_sum(str) != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid ISBN: \"%s\"", str),
				 errdetail("failed checksum")));
		return (NULL);
	}

	result = (isbn *) palloc(sizeof(isbn));

	strncpy(result->num, str, 13);
	memset(result->pad, ' ', 3);
	return (result);
}

/*
 * The ISBN checksum is defined as follows:
 *
 * Number the digits from 1 to 9 (call this N).
 * Compute the sum, S, of N * D_N.
 * The check digit, C, is the value which satisfies the equation
 *	S + 10*C === 0 (mod 11)
 * The value 10 for C is written as `X'.
 *
 * For our purposes, we want the complete sum including the check
 * digit; if this is zero, then the checksum passed.  We also check
 * the syntactic validity if the provided string, and return 12
 * if any errors are found.
 */
int4
isbn_sum(char *str)
{
	int4		sum = 0,
				dashes = 0,
				val;
	int			i;

	for (i = 0; str[i] && i < 13; i++)
	{
		switch (str[i])
		{
			case '-':
				if (++dashes > 3)
					return 12;
				continue;

			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				val = str[i] - '0';
				break;

			case 'X':
			case 'x':
				val = 10;
				break;

			default:
				return 12;
		}

		sum += val * (i + 1 - dashes);
	}
	return (sum % 11);
}

/*
 *	ISBN output function.
 */

char *
isbn_out(isbn * num)
{
	char	   *result;

	if (num == NULL)
		return (NULL);

	result = (char *) palloc(14);

	result[0] = '\0';
	strncat(result, num->num, 13);
	return (result);
}

/*
 *	Boolean tests for magnitude.
 */

bool
isbn_lt(isbn * a1, isbn * a2)
{
	return (strncmp(a1->num, a2->num, 13) < 0);
}

bool
isbn_le(isbn * a1, isbn * a2)
{
	return (strncmp(a1->num, a2->num, 13) <= 0);
}

bool
isbn_eq(isbn * a1, isbn * a2)
{
	return (strncmp(a1->num, a2->num, 13) == 0);
}

bool
isbn_ge(isbn * a1, isbn * a2)
{
	return (strncmp(a1->num, a2->num, 13) >= 0);
}

bool
isbn_gt(isbn * a1, isbn * a2)
{
	return (strncmp(a1->num, a2->num, 13) > 0);
}

bool
isbn_ne(isbn * a1, isbn * a2)
{
	return (strncmp(a1->num, a2->num, 13) != 0);
}

/*
 *	Comparison function for sorting:
 */

int4
isbn_cmp(isbn * a1, isbn * a2)
{
	return (strncmp(a1->num, a2->num, 13));
}


/* ----------------------------- ISSN --------------------------- */

/*
 *	This is the internal storage format for ISSNs.
 *	NB: This is an intentional type pun with builtin type `char16'.
 */

typedef struct issn
{
	char		num[9];
	char		pad[7];
}	issn;

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

	if (strlen(str) != 9)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid ISSN: \"%s\"", str),
				 errdetail("incorrect length")));

		return (NULL);
	}
	if (issn_sum(str) != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid ISSN: \"%s\"", str),
				 errdetail("failed checksum")));
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
	int4		sum = 0,
				dashes = 0,
				val;
	int			i;

	for (i = 0; str[i] && i < 9; i++)
	{
		switch (str[i])
		{
			case '-':
				if (++dashes > 1)
					return 12;
				continue;

			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				val = str[i] - '0';
				break;

			case 'X':
			case 'x':
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
}

bool
issn_le(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9) <= 0);
}

bool
issn_eq(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9) == 0);
}

bool
issn_ge(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9) >= 0);
}

bool
issn_gt(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9) > 0);
}

bool
issn_ne(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9) != 0);
}

/*
 *	Comparison function for sorting:
 */

int4
issn_cmp(issn * a1, issn * a2)
{
	return (strncmp(a1->num, a2->num, 9));
}
