/*
 * src/pl/plpython/plpy_procedure.h
 */

#ifndef PLPY_PROCEDURE_H
#define PLPY_PROCEDURE_H

#include "plpy_typeio.h"


extern void init_procedure_caches(void);


/* saved arguments for outer recursion level or set-returning function */
typedef struct PLySavedArgs
{
	struct PLySavedArgs *next;	/* linked-list pointer */
	PyObject   *args;			/* "args" element of globals dict */
	int			nargs;			/* length of namedargs array */
	PyObject   *namedargs[FLEXIBLE_ARRAY_MEMBER];		/* named args */
} PLySavedArgs;

/* cached procedure data */
typedef struct PLyProcedure
{
	MemoryContext mcxt;			/* context holding this PLyProcedure and its
								 * subsidiary data */
	char	   *proname;		/* SQL name of procedure */
	char	   *pyname;			/* Python name of procedure */
	TransactionId fn_xmin;
	ItemPointerData fn_tid;
	bool		fn_readonly;
	bool		is_setof;		/* true, if procedure returns result set */
	PLyTypeInfo result;			/* also used to store info for trigger tuple
								 * type */
	char	   *src;			/* textual procedure code, after mangling */
	char	  **argnames;		/* Argument names */
	PLyTypeInfo args[FUNC_MAX_ARGS];
	int			nargs;
	Oid			langid;			/* OID of plpython pg_language entry */
	List	   *trftypes;		/* OID list of transform types */
	PyObject   *code;			/* compiled procedure code */
	PyObject   *statics;		/* data saved across calls, local scope */
	PyObject   *globals;		/* data saved across calls, global scope */
	long		calldepth;		/* depth of recursive calls of function */
	PLySavedArgs *argstack;		/* stack of outer-level call arguments */
} PLyProcedure;

/* the procedure cache key */
typedef struct PLyProcedureKey
{
	Oid			fn_oid;			/* function OID */
	Oid			fn_rel;			/* triggered-on relation or InvalidOid */
} PLyProcedureKey;

/* the procedure cache entry */
typedef struct PLyProcedureEntry
{
	PLyProcedureKey key;		/* hash key */
	PLyProcedure *proc;
} PLyProcedureEntry;

/* PLyProcedure manipulation */
extern char *PLy_procedure_name(PLyProcedure *proc);
extern PLyProcedure *PLy_procedure_get(Oid fn_oid, Oid fn_rel, bool is_trigger);
extern void PLy_procedure_compile(PLyProcedure *proc, const char *src);
extern void PLy_procedure_delete(PLyProcedure *proc);

#endif   /* PLPY_PROCEDURE_H */
