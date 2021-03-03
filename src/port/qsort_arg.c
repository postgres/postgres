/*
 *	qsort_arg.c: qsort with a passthrough "void *" argument
 */

#include "c.h"

#define ST_SORT qsort_arg
#define ST_ELEMENT_TYPE_VOID
#define ST_COMPARATOR_TYPE_NAME qsort_arg_comparator
#define ST_COMPARE_RUNTIME_POINTER
#define ST_COMPARE_ARG_TYPE void
#define ST_SCOPE
#define ST_DEFINE
#include "lib/sort_template.h"
