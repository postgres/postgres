/*-------------------------------------------------------------------------
 *
 * name.c--
 *	  Functions for the built-in type "name".
 * name replaces char16 and is carefully implemented so that it
 * is a string of length NAMEDATALEN.  DO NOT use hard-coded constants anywhere
 * always use NAMEDATALEN as the symbolic constant!   - jolly 8/21/95
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/name.c,v 1.16 1998/10/08 18:30:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "utils/builtins.h"		/* where the declarations go */
#include "utils/palloc.h"		/* where the declarations go */

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
namein(char *s)
{
	NameData   *result;
	int			len;

	if (s == NULL)
		return NULL;
	result = (NameData *) palloc(NAMEDATALEN);
	/* always keep it null-padded */
	StrNCpy(result->data, s, NAMEDATALEN);
	len = strlen(result->data);
	while (len < NAMEDATALEN)
	{
		*(result->data + len) = '\0';
		len++;
	}
	return result;
}

/*
 *		nameout - converts internal reprsentation to "..."
 */
char *
nameout(NameData *s)
{
	if (s == NULL)
		return "-";
	else
		return pstrdup(s->data);
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
nameeq(NameData *arg1, NameData *arg2)
{
	if (!arg1 || !arg2)
		return 0;
	else
		return (bool) strncmp(arg1->data, arg2->data, NAMEDATALEN) == 0;
}

bool
namene(NameData *arg1, NameData *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return (bool) 0;
	return (bool) (strncmp(arg1->data, arg2->data, NAMEDATALEN) != 0);
}

bool
namelt(NameData *arg1, NameData *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return (bool) 0;
	return (bool) (strncmp(arg1->data, arg2->data, NAMEDATALEN) < 0);
}

bool
namele(NameData *arg1, NameData *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return (bool) 0;
	return (bool) (strncmp(arg1->data, arg2->data, NAMEDATALEN) <= 0);
}

bool
namegt(NameData *arg1, NameData *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return (bool) 0;

	return (bool) (strncmp(arg1->data, arg2->data, NAMEDATALEN) > 0);
}

bool
namege(NameData *arg1, NameData *arg2)
{
	if (arg1 == NULL || arg2 == NULL)
		return (bool) 0;

	return (bool) (strncmp(arg1->data, arg2->data, NAMEDATALEN) >= 0);
}


/* (see char.c for comparison/operation routines) */

int
namecpy(Name n1, Name n2)
{
	if (!n1 || !n2)
		return -1;
	strncpy(n1->data, n2->data, NAMEDATALEN);
	return 0;
}

#ifdef NOT_USED
int
namecat(Name n1, Name n2)
{
	return namestrcat(n1, n2->data);	/* n2 can't be any longer than n1 */
}

#endif

#ifdef NOT_USED
int
namecmp(Name n1, Name n2)
{
	return strncmp(n1->data, n2->data, NAMEDATALEN);
}
#endif

int
namestrcpy(Name name, char *str)
{
	if (!name || !str)
		return -1;
	StrNCpy(name->data, str, NAMEDATALEN);
	return 0;
}

#ifdef NOT_USED
int
namestrcat(Name name, char *str)
{
	int			i;
	char	   *p,
			   *q;

	if (!name || !str)
		return -1;
	for (i = 0, p = name->data; i < NAMEDATALEN && *p; ++i, ++p)
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
namestrcmp(Name name, char *str)
{
	if (!name && !str)
		return 0;
	if (!name)
		return -1;				/* NULL < anything */
	if (!str)
		return 1;				/* NULL < anything */
	return strncmp(name->data, str, NAMEDATALEN);
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

	for (length = 0, charP = name->data;
		 length < NAMEDATALEN && *charP != '\0';
		 length++, charP++)
		;
	return (uint32) length;
}

#endif
