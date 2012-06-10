/*
 * src/pl/plpython/plpy_exec.h
 */

#ifndef PLPY_EXEC_H
#define PLPY_EXEC_H

#include "plpy_procedure.h"

extern Datum PLy_exec_function(FunctionCallInfo fcinfo, PLyProcedure *proc);
extern HeapTuple PLy_exec_trigger(FunctionCallInfo fcinfo, PLyProcedure *proc);

#endif   /* PLPY_EXEC_H */
