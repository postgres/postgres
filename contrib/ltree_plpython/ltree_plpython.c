#include "postgres.h"
#include "fmgr.h"
#include "plpython.h"
#include "ltree.h"

PG_MODULE_MAGIC;


PG_FUNCTION_INFO_V1(ltree_to_plpython);

Datum
ltree_to_plpython(PG_FUNCTION_ARGS)
{
	ltree	   *in = PG_GETARG_LTREE(0);
	int			i;
	PyObject   *list;
	ltree_level *curlevel;

	list = PyList_New(in->numlevel);

	curlevel = LTREE_FIRST(in);
	for (i = 0; i < in->numlevel; i++)
	{
		PyList_SetItem(list, i, PyString_FromStringAndSize(curlevel->name, curlevel->len));
		curlevel = LEVEL_NEXT(curlevel);
	}

	PG_FREE_IF_COPY(in, 0);

	return PointerGetDatum(list);
}
