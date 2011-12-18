/*
 * src/pl/plpython/plpy_resultobject.h
 */

#ifndef PLPY_RESULTOBJECT_H
#define PLPY_RESULTOBJECT_H

typedef struct PLyResultObject
{
	PyObject_HEAD
	/* HeapTuple *tuples; */
	PyObject   *nrows;			/* number of rows returned by query */
	PyObject   *rows;			/* data rows, or None if no data returned */
	PyObject   *status;			/* query status, SPI_OK_*, or SPI_ERR_* */
} PLyResultObject;

extern void PLy_result_init_type(void);
extern PyObject *PLy_result_new(void);

#endif	/* PLPY_RESULTOBJECT_H */
