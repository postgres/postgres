/*
 * src/pl/plpython/plpy_exec.h
 */

#ifndef PLPY_EXEC_H
#define PLPY_EXEC_H

#include "plpy_procedure.h"

extern Datum PLy_exec_function(FunctionCallInfo fcinfo, PLyProcedureCache *pcache);
extern HeapTuple PLy_exec_trigger(FunctionCallInfo fcinfo, PLyProcedure *proc);
extern void PLy_exec_event_trigger(FunctionCallInfo fcinfo, PLyProcedure *proc);
extern void PLy_function_cleanup_srfstate(PLyProcedureCache *pcache);

#endif							/* PLPY_EXEC_H */
