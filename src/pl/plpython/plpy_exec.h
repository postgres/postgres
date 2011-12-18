/*
 * src/pl/plpython/plpy_exec.h
 */

#ifndef PLPY_EXEC_H
#define PLPY_EXEC_H

#include "plpy_procedure.h"

extern Datum PLy_exec_function(FunctionCallInfo, PLyProcedure *);
extern HeapTuple PLy_exec_trigger(FunctionCallInfo, PLyProcedure *);

#endif	/* PLPY_EXEC_H */
