/*-------------------------------------------------------------------------
 *
 * name.c
 *	  Functions for the built-in type "name".
 * name replaces char16 and is carefully implemented so that it
 * is a string of length NAMEDATALEN.  DO NOT use hard-coded constants anywhere
 * always use NAMEDATALEN as the symbolic constant!   - jolly 8/21/95
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/name.c,v 1.27 2000/01/26 05:57:14 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "utils/builtins.h"

/*****************************************************************************
 *	 USER I/O ROUTINES (none)												 *
 *****************************************************************************/


/*
 *		namein	- converts "..." to internal representation
 *
 *		Note:
 *				[Old] Currently if strlen(s) < NAMEDATALEN, the extra chars are nulls
 *				Now, always NULL terminated
 */
NameData   *
namein(const char *s)
{
	NameData   *result;
	int			len;

	if (s == NULL)
		return NULL;
	result = (NameData *) palloc(NAMEDATALEN);
	/* always keep it null-padded */
	StrNCpy(NameStr(*result), s, NAMEDATALEN);
	len = strlen(NameStr(*result));
	while (len < NAMEDATALEN)
	{
		*(NameStr(*result) + len) = '\0';
		len++;
	}
	return result;
}

/*
 *		nameout - converts internal reprsentation to "..."
 */
char *
nameout(const NameData *s)
{
	if (s == NULL)
		return "-";
	else
		return pstrdup(NameStr(*s));
}


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

/*
 *		nameeq	- returns 1 iff arguments are equal
 *		namene	- returns 1 iff arguments are not equal
 *
 *		BUGS:
 *				Assumes that "xy\0\0a" should be equal to "xy\0b".
 *				If not, can do the comparison backwards for efficiency.
 *
 *		namelt	- returns 1 iff a < b
 *		namele	- returns 1 iff a <= b
 *		namegt	- returns 1 iff a < b
 *		namege	- returns 1 iff a <= b
 *
 */
bool
nameeq(const NameData *arg1, const NameData *arg2)
{
	if (!arg1 || !arg2)
		return 0;
	else
		return (bool) (strncmp(NameStr(*arg1), NameStr(*arg2), NAMEDATALEN) == 0);
}

bool
namene(const NameData *arg1, const NameData *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return (bool) 0;
	return (bool) (strncmp(NameStr(*arg1), NameStr(*arg2), NAMEDATALEN) != 0);
}

bool
namelt(const NameData *arg1, const NameData *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return (bool) 0;
	return (bool) (strncmp(NameStr(*arg1), NameStr(*arg2), NAMEDATALEN) < 0);
}

bool
namele(const NameData *arg1, const NameData *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return (bool) 0;
	return (bool) (strncmp(NameStr(*arg1), NameStr(*arg2), NAMEDATALEN) <= 0);
}

bool
namegt(const NameData *arg1, const NameData *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return (bool) 0;

	return (bool) (strncmp(NameStr(*arg1), NameStr(*arg2), NAMEDATALEN) > 0);
}

bool
namege(const NameData *arg1, const NameData *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return (bool) 0;

	return (bool) (strncmp(NameStr(*arg1), NameStr(*arg2), NAMEDATALEN) >= 0);
}


/* (see char.c for comparison/operation routines) */

int
namecpy(Name n1, Name n2)
{
	if (!n1 || !n2)
		return -1;
	strncpy(NameStr(*n1), NameStr(*n2), NAMEDATALEN);
	return 0;
}

#ifdef NOT_USED
int
namecat(Name n1, Name n2)
{
	return namestrcat(n1, NameStr(*n2));	/* n2 can't be any longer than n1 */
}

#endif

#ifdef NOT_USED
int
namecmp(Name n1, Name n2)
{
	return strncmp(NameStr(*n1), NameStr(*n2), NAMEDATALEN);
}

#endif

int
namestrcpy(Name name, const char *str)
{
	if (!name || !str)
		return -1;
	StrNCpy(NameStr(*name), str, NAMEDATALEN);
	return 0;
}

#ifdef NOT_USED
int
namestrcat(Name name, const char *str)
{
	int			i;
	char	   *p,
			   *q;

	if (!name || !str)
		return -1;
	for (i = 0, p = NameStr(*name); i < NAMEDATALEN && *p; ++i, ++p)
		;
	for (q = str; i < NAMEDATALEN; ++i, ++p, ++q)
	{
		*p = *q;
		if (!*q)
			break;
	}
	return 0;
}

#endif

int
namestrcmp(Name name, const char *str)
{
	if (!name && !str)
		return 0;
	if (!name)
		return -1;				/* NULL < anything */
	if (!str)
		return 1;				/* NULL < anything */
	return strncmp(NameStr(*name), str, NAMEDATALEN);
}

/*****************************************************************************
 *	 PRIVATE ROUTINES														 *
 *****************************************************************************/

#ifdef NOT_USED
uint32
NameComputeLength(Name name)
{
	char	   *charP;
	int			length;

	for (length = 0, charP = NameStr(*name);
		 length < NAMEDATALEN && *charP != '\0';
		 length++, charP++)
		;
	return (uint32) length;
}

#endif
