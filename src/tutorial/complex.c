/******************************************************************************
  This file contains routines that can be bound to a Postgres backend and
  called by the backend in the process of processing queries.  The calling
  format for these routines is dictated by Postgres architecture.
******************************************************************************/

#include "postgres.h"

typedef struct Complex
{
	double		x;
	double		y;
}	Complex;

/* These prototypes declare the requirements that Postgres places on these
   user written functions.
*/
Complex    *complex_in(char *str);
char	   *complex_out(Complex * complex);
Complex    *complex_add(Complex * a, Complex * b);
bool		complex_abs_lt(Complex * a, Complex * b);
bool		complex_abs_le(Complex * a, Complex * b);
bool		complex_abs_eq(Complex * a, Complex * b);
bool		complex_abs_ge(Complex * a, Complex * b);
bool		complex_abs_gt(Complex * a, Complex * b);
int4		complex_abs_cmp(Complex * a, Complex * b);


/*****************************************************************************
 * Input/Output functions
 *****************************************************************************/

Complex *
complex_in(char *str)
{
	double		x,
				y;
	Complex    *result;

	if (sscanf(str, " ( %lf , %lf )", &x, &y) != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for complex: \"%s\"", str)));

	result = (Complex *) palloc(sizeof(Complex));
	result->x = x;
	result->y = y;
	return result;
}

char *
complex_out(Complex * complex)
{
	char	   *result;

	if (complex == NULL)
		return NULL;

	result = (char *) palloc(100);
	snprintf(result, 100, "(%g,%g)", complex->x, complex->y);
	return result;
}

/*****************************************************************************
 * New Operators
 *****************************************************************************/

Complex *
complex_add(Complex * a, Complex * b)
{
	Complex    *result;

	result = (Complex *) palloc(sizeof(Complex));
	result->x = a->x + b->x;
	result->y = a->y + b->y;
	return result;
}


/*****************************************************************************
 * Operator class for defining B-tree index
 *****************************************************************************/

#define Mag(c)	((c)->x*(c)->x + (c)->y*(c)->y)

bool
complex_abs_lt(Complex * a, Complex * b)
{
	double		amag = Mag(a),
				bmag = Mag(b);

	return amag < bmag;
}

bool
complex_abs_le(Complex * a, Complex * b)
{
	double		amag = Mag(a),
				bmag = Mag(b);

	return amag <= bmag;
}

bool
complex_abs_eq(Complex * a, Complex * b)
{
	double		amag = Mag(a),
				bmag = Mag(b);

	return amag == bmag;
}

bool
complex_abs_ge(Complex * a, Complex * b)
{
	double		amag = Mag(a),
				bmag = Mag(b);

	return amag >= bmag;
}

bool
complex_abs_gt(Complex * a, Complex * b)
{
	double		amag = Mag(a),
				bmag = Mag(b);

	return amag > bmag;
}

int4
complex_abs_cmp(Complex * a, Complex * b)
{
	double		amag = Mag(a),
				bmag = Mag(b);

	if (amag < bmag)
		return -1;
	else if (amag > bmag)
		return 1;
	else
		return 0;
}

/*****************************************************************************
 * test code
 *****************************************************************************/

/*
 * You should always test your code separately. Trust me, using POSTGRES to
 * debug your C function will be very painful and unproductive. In case of
 * POSTGRES crashing, it is impossible to tell whether the bug is in your
 * code or POSTGRES's.
 */
void		test_main(void);
void
test_main()
{
	Complex    *a;
	Complex    *b;

	a = complex_in("(4.01, 3.77 )");
	printf("a = %s\n", complex_out(a));
	b = complex_in("(1.0,2.0)");
	printf("b = %s\n", complex_out(b));
	printf("a +  b = %s\n", complex_out(complex_add(a, b)));
	printf("a <  b = %d\n", complex_abs_lt(a, b));
	printf("a <= b = %d\n", complex_abs_le(a, b));
	printf("a =  b = %d\n", complex_abs_eq(a, b));
	printf("a >= b = %d\n", complex_abs_ge(a, b));
	printf("a >  b = %d\n", complex_abs_gt(a, b));
}
