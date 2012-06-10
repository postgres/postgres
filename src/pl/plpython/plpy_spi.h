/*
 * src/pl/plpython/plpy_spi.h
 */

#ifndef PLPY_SPI_H
#define PLPY_SPI_H

#include "utils/palloc.h"
#include "utils/resowner.h"

extern PyObject *PLy_spi_prepare(PyObject *self, PyObject *args);
extern PyObject *PLy_spi_execute(PyObject *self, PyObject *args);

typedef struct PLyExceptionEntry
{
	int			sqlstate;		/* hash key, must be first */
	PyObject   *exc;			/* corresponding exception */
} PLyExceptionEntry;

/* handling of SPI operations inside subtransactions */
extern void PLy_spi_subtransaction_begin(MemoryContext oldcontext, ResourceOwner oldowner);
extern void PLy_spi_subtransaction_commit(MemoryContext oldcontext, ResourceOwner oldowner);
extern void PLy_spi_subtransaction_abort(MemoryContext oldcontext, ResourceOwner oldowner);

#endif   /* PLPY_SPI_H */
