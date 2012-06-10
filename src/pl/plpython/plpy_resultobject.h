/*
 * src/pl/plpython/plpy_resultobject.h
 */

#ifndef PLPY_RESULTOBJECT_H
#define PLPY_RESULTOBJECT_H

#include "access/tupdesc.h"


typedef struct PLyResultObject
{
	PyObject_HEAD
	/* HeapTuple *tuples; */
	PyObject   *nrows;			/* number of rows returned by query */
	PyObject   *rows;			/* data rows, or empty list if no data
								 * returned */
	PyObject   *status;			/* query status, SPI_OK_*, or SPI_ERR_* */
	TupleDesc	tupdesc;
} PLyResultObject;

extern void PLy_result_init_type(void);
extern PyObject *PLy_result_new(void);

#endif   /* PLPY_RESULTOBJECT_H */
