/******************************************************************************
  These are user-defined functions that can be bound to a Postgres backend
  and called by Postgres to execute SQL functions of the same name.

  The calling format for these functions is defined by the CREATE FUNCTION
  SQL statement that binds them to the backend.
*****************************************************************************/

#include <string.h>
#include <stdio.h>
#include "postgres.h"			/* for char16, etc. */
#include "utils/palloc.h"		/* for palloc */
#include "libpq-fe.h"			/* for TUPLE */
#include "executor/executor.h"	/* for GetAttributeByName() */

/* The following prototypes declare what we assume the user declares to
   Postgres in his CREATE FUNCTION statement.
*/

int			add_one(int arg);
char16	   *concat16(char16 *arg1, char16 *arg2);
text	   *copytext(text *t);

bool
c_overpaid(TUPLE t,				/* the current instance of EMP */
		   int4 limit);



int
add_one(int arg)
{
	return arg + 1;
}

char16 *
concat16(char16 *arg1, char16 *arg2)
{
	char16	   *new_c16 = (char16 *) palloc(sizeof(char16));

	MemSet(new_c16, 0, sizeof(char16));
	strncpy((char *) new_c16, (char *) arg1, 16);
	return (char16 *) (strncat((char *) new_c16, (char *) arg2, 16));
}

text *
copytext(text *t)
{

	/*
	 * VARSIZE is the total size of the struct in bytes.
	 */
	text	   *new_t = (text *) palloc(VARSIZE(t));

	MemSet(new_t, 0, VARSIZE(t));

	VARSIZE(new_t) = VARSIZE(t);

	/*
	 * VARDATA is a pointer to the data region of the struct.
	 */
	memcpy((void *) VARDATA(new_t),		/* destination */
		   (void *) VARDATA(t), /* source */
		   VARSIZE(t) - VARHDRSZ);		/* how many bytes */

	return new_t;
}

bool
c_overpaid(TUPLE t,				/* the current instance of EMP */
		   int4 limit)
{
	bool		isnull = false;
	int4		salary;

	salary = (int4) GetAttributeByName(t, "salary", &isnull);

	if (isnull)
		return false;
	return salary > limit;
}
