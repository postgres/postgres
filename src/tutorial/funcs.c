/******************************************************************************
  These are user-defined functions that can be bound to a Postgres backend
  and called by Postgres to execute SQL functions of the same name.

  The calling format for these functions is defined by the CREATE FUNCTION
  SQL statement that binds them to the backend.
*****************************************************************************/

#include "postgres.h"			/* for variable length type */
#include "executor/executor.h"	/* for GetAttributeByName() */
#include "utils/geo_decls.h"	/* for point type */

/* The following prototypes declare what we assume the user declares to
   Postgres in his CREATE FUNCTION statement.
*/

int			add_one(int arg);
Point	   *makepoint(Point *pointx, Point *pointy);
text	   *copytext(text *t);

bool c_overpaid(TupleTableSlot *t,	/* the current instance of EMP */
		   int4 limit);



int
add_one(int arg)
{
	return arg + 1;
}

Point *
makepoint(Point *pointx, Point *pointy)
{
	Point	   *new_point = (Point *) palloc(sizeof(Point));

	new_point->x = pointx->x;
	new_point->y = pointy->y;

	return new_point;
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
c_overpaid(TupleTableSlot *t,	/* the current instance of EMP */
		   int4 limit)
{
	bool		isnull = false;
	int4		salary;

	salary = (int4) GetAttributeByName(t, "salary", &isnull);

	if (isnull)
		return false;
	return salary > limit;
}
