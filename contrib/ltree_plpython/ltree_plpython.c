#include "postgres.h"

#include "fmgr.h"
#include "ltree/ltree.h"
#include "plpython.h"

PG_MODULE_MAGIC;

extern void _PG_init(void);

/* Linkage to functions in plpython module */
typedef PyObject *(*PLyUnicode_FromStringAndSize_t) (const char *s, Py_ssize_t size);
static PLyUnicode_FromStringAndSize_t PLyUnicode_FromStringAndSize_p;


/*
 * Module initialize function: fetch function pointers for cross-module calls.
 */
void
_PG_init(void)
{
	/* Asserts verify that typedefs above match original declarations */
	AssertVariableIsOfType(&PLyUnicode_FromStringAndSize, PLyUnicode_FromStringAndSize_t);
	PLyUnicode_FromStringAndSize_p = (PLyUnicode_FromStringAndSize_t)
		load_external_function("$libdir/" PLPYTHON_LIBNAME, "PLyUnicode_FromStringAndSize",
							   true, NULL);
}


/* These defines must be after the module init function */
#define PLyUnicode_FromStringAndSize PLyUnicode_FromStringAndSize_p


PG_FUNCTION_INFO_V1(ltree_to_plpython);

Datum
ltree_to_plpython(PG_FUNCTION_ARGS)
{
	ltree	   *in = PG_GETARG_LTREE_P(0);
	int			i;
	PyObject   *list;
	ltree_level *curlevel;

	list = PyList_New(in->numlevel);
	if (!list)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	curlevel = LTREE_FIRST(in);
	for (i = 0; i < in->numlevel; i++)
	{
		PyList_SetItem(list, i, PLyUnicode_FromStringAndSize(curlevel->name, curlevel->len));
		curlevel = LEVEL_NEXT(curlevel);
	}

	PG_FREE_IF_COPY(in, 0);

	return PointerGetDatum(list);
}
