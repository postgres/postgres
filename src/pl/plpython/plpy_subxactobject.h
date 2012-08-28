/*
 * src/pl/plpython/plpy_subxactobject.h
 */

#ifndef PLPY_SUBXACTOBJECT
#define PLPY_SUBXACTOBJECT

#include "nodes/pg_list.h"
#include "utils/resowner.h"

/* a list of nested explicit subtransactions */
extern List *explicit_subtransactions;


typedef struct PLySubtransactionObject
{
	PyObject_HEAD
	bool		started;
	bool		exited;
} PLySubtransactionObject;

/* explicit subtransaction data */
typedef struct PLySubtransactionData
{
	MemoryContext oldcontext;
	ResourceOwner oldowner;
} PLySubtransactionData;

extern void PLy_subtransaction_init_type(void);
extern PyObject *PLy_subtransaction_new(PyObject *self, PyObject *unused);

#endif   /* PLPY_SUBXACTOBJECT */
