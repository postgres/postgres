/* $Id: isinf.c,v 1.10 1999/07/18 17:38:23 momjian Exp $ */

#include <math.h>

#include "config.h"

#ifdef NOT_USED

#if HAVE_IEEEFP_H
#include <ieeefp.h>
#endif
#if HAVE_FP_CLASS_H
#include <fp_class.h>
#endif

int
isinf(double d)
{
	fpclass_t	type = fpclass(d);

	switch (type)
	{
		case FP_NINF:
		case FP_PINF:
			return 1;
		default:
			break;
	}
	return 0;
}

int
isinf(x)
double		x;
{
#if HAVE_FP_CLASS
	int			fpclass = fp_class(x);

#else
	int			fpclass = fp_class_d(x);

#endif

	if (fpclass == FP_POS_INF)
		return 1;
	if (fpclass == FP_NEG_INF)
		return -1;
	return 0;
}

#endif
