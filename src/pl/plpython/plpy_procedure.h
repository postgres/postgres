/*
 * src/pl/plpython/plpy_procedure.h
 */

#ifndef PLPY_PROCEDURE_H
#define PLPY_PROCEDURE_H

#include "plpy_typeio.h"
#include "utils/funccache.h"


/*
 * Trigger type
 */
typedef enum PLyTrigType
{
	PLPY_TRIGGER,
	PLPY_EVENT_TRIGGER,
	PLPY_NOT_TRIGGER,
} PLyTrigType;

/* saved arguments for outer recursion level or set-returning function */
typedef struct PLySavedArgs
{
	struct PLySavedArgs *next;	/* linked-list pointer */
	PyObject   *args;			/* "args" element of globals dict */
	PyObject   *td;				/* "TD" element of globals dict, if trigger */
	int			nargs;			/* length of namedargs array */
	PyObject   *namedargs[FLEXIBLE_ARRAY_MEMBER];	/* named args */
} PLySavedArgs;

/* saved state for a set-returning function */
typedef struct PLySRFState
{
	PyObject   *iter;			/* Python iterator producing results */
	PLySavedArgs *savedargs;	/* function argument values */
} PLySRFState;

/*
 * Long-lived data for a PL/Python function.
 *
 * This struct is managed by funccache.c and can be shared across multiple
 * executions of the same function.  It must contain no execution-specific
 * state.  The CachedFunction struct must be first.
 */
typedef struct PLyProcedure
{
	CachedFunction cfunc;		/* fields managed by funccache.c */

	MemoryContext mcxt;			/* context holding this PLyProcedure and its
								 * subsidiary data */
	char	   *proname;		/* SQL name of procedure */
	char	   *pyname;			/* Python name of procedure */
	bool		fn_readonly;
	bool		is_setof;		/* true, if function returns result set */
	bool		is_procedure;
	PLyTrigType is_trigger;		/* called as trigger? */
	PLyObToDatum result;		/* Function result output conversion info */
	PLyDatumToOb result_in;		/* For converting input tuples in a trigger */
	char	   *src;			/* textual procedure code, after mangling */
	char	  **argnames;		/* Argument names */
	PLyDatumToOb *args;			/* Argument input conversion info */
	int			nargs;			/* Number of elements in above arrays */
	Oid			langid;			/* OID of plpython pg_language entry */
	List	   *trftypes;		/* OID list of transform types */
	PyObject   *code;			/* compiled procedure code */
	PyObject   *statics;		/* data saved across calls, local scope */
	PyObject   *globals;		/* data saved across calls, global scope */
	long		calldepth;		/* depth of recursive calls of function */
	PLySavedArgs *argstack;		/* stack of outer-level call arguments */
} PLyProcedure;

/*
 * Per-call-site cache for a PL/Python function.
 *
 * This struct is stored in fn_extra and holds execution-specific state,
 * including a pointer to the long-lived PLyProcedure.  The use_count in
 * the PLyProcedure is incremented while we hold a reference.
 */
typedef struct PLyProcedureCache
{
	PLyProcedure *proc;			/* long-lived hash entry */
	MemoryContext fcontext;		/* fn_mcxt - context holding this struct */
	PLySRFState *srfstate;		/* SRF execution state, NULL if not in SRF */
	bool		shutdown_reg;	/* true if registered shutdown callback */

	/* Callback to release resources when fcontext is reset or deleted */
	MemoryContextCallback mcb;
} PLyProcedureCache;

/* PLyProcedure manipulation */
extern char *PLy_procedure_name(PLyProcedure *proc);
extern PLyProcedureCache *PLy_procedure_get(FunctionCallInfo fcinfo, bool forValidator);
extern void PLy_procedure_compile(PLyProcedure *proc, const char *src);
extern void PLy_procedure_delete(PLyProcedure *proc);

#endif							/* PLPY_PROCEDURE_H */
