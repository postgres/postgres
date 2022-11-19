/*
 * src/pl/plpython/plpy_typeio.h
 */

#ifndef PLPY_TYPEIO_H
#define PLPY_TYPEIO_H

#include "access/htup.h"
#include "fmgr.h"
#include "plpython.h"
#include "utils/typcache.h"

struct PLyProcedure;			/* avoid requiring plpy_procedure.h here */


/*
 * "Input" conversion from PostgreSQL Datum to a Python object.
 *
 * arg is the previously-set-up conversion data, val is the value to convert.
 * val mustn't be NULL.
 *
 * Note: the conversion data structs should be regarded as private to
 * plpy_typeio.c.  We declare them here only so that other modules can
 * define structs containing them.
 */
typedef struct PLyDatumToOb PLyDatumToOb;	/* forward reference */

typedef PyObject *(*PLyDatumToObFunc) (PLyDatumToOb *arg, Datum val);

typedef struct PLyScalarToOb
{
	FmgrInfo	typfunc;		/* lookup info for type's output function */
} PLyScalarToOb;

typedef struct PLyArrayToOb
{
	PLyDatumToOb *elm;			/* conversion info for array's element type */
} PLyArrayToOb;

typedef struct PLyTupleToOb
{
	/* If we're dealing with a RECORD type, actual descriptor is here: */
	TupleDesc	recdesc;
	/* If we're dealing with a named composite type, these fields are set: */
	TypeCacheEntry *typentry;	/* typcache entry for type */
	uint64		tupdescid;		/* last tupdesc identifier seen in typcache */
	/* These fields are NULL/0 if not yet set: */
	PLyDatumToOb *atts;			/* array of per-column conversion info */
	int			natts;			/* length of array */
} PLyTupleToOb;

typedef struct PLyTransformToOb
{
	FmgrInfo	typtransform;	/* lookup info for from-SQL transform func */
} PLyTransformToOb;

struct PLyDatumToOb
{
	PLyDatumToObFunc func;		/* conversion control function */
	Oid			typoid;			/* OID of the source type */
	int32		typmod;			/* typmod of the source type */
	bool		typbyval;		/* its physical representation details */
	int16		typlen;
	char		typalign;
	MemoryContext mcxt;			/* context this info is stored in */
	union						/* conversion-type-specific data */
	{
		PLyScalarToOb scalar;
		PLyArrayToOb array;
		PLyTupleToOb tuple;
		PLyTransformToOb transform;
	}			u;
};

/*
 * "Output" conversion from Python object to a PostgreSQL Datum.
 *
 * arg is the previously-set-up conversion data, val is the value to convert.
 *
 * *isnull is set to true if val is Py_None, false otherwise.
 * (The conversion function *must* be called even for Py_None,
 * so that domain constraints can be checked.)
 *
 * inarray is true if the converted value was in an array (Python list).
 * It is used to give a better error message in some cases.
 */
typedef struct PLyObToDatum PLyObToDatum;	/* forward reference */

typedef Datum (*PLyObToDatumFunc) (PLyObToDatum *arg, PyObject *val,
								   bool *isnull,
								   bool inarray);

typedef struct PLyObToScalar
{
	FmgrInfo	typfunc;		/* lookup info for type's input function */
	Oid			typioparam;		/* argument to pass to it */
} PLyObToScalar;

typedef struct PLyObToArray
{
	PLyObToDatum *elm;			/* conversion info for array's element type */
	Oid			elmbasetype;	/* element base type */
} PLyObToArray;

typedef struct PLyObToTuple
{
	/* If we're dealing with a RECORD type, actual descriptor is here: */
	TupleDesc	recdesc;
	/* If we're dealing with a named composite type, these fields are set: */
	TypeCacheEntry *typentry;	/* typcache entry for type */
	uint64		tupdescid;		/* last tupdesc identifier seen in typcache */
	/* These fields are NULL/0 if not yet set: */
	PLyObToDatum *atts;			/* array of per-column conversion info */
	int			natts;			/* length of array */
	/* We might need to convert using record_in(); if so, cache info here */
	FmgrInfo	recinfunc;		/* lookup info for record_in */
} PLyObToTuple;

typedef struct PLyObToDomain
{
	PLyObToDatum *base;			/* conversion info for domain's base type */
	void	   *domain_info;	/* cache space for domain_check() */
} PLyObToDomain;

typedef struct PLyObToTransform
{
	FmgrInfo	typtransform;	/* lookup info for to-SQL transform function */
} PLyObToTransform;

struct PLyObToDatum
{
	PLyObToDatumFunc func;		/* conversion control function */
	Oid			typoid;			/* OID of the target type */
	int32		typmod;			/* typmod of the target type */
	bool		typbyval;		/* its physical representation details */
	int16		typlen;
	char		typalign;
	MemoryContext mcxt;			/* context this info is stored in */
	union						/* conversion-type-specific data */
	{
		PLyObToScalar scalar;
		PLyObToArray array;
		PLyObToTuple tuple;
		PLyObToDomain domain;
		PLyObToTransform transform;
	}			u;
};


extern PGDLLEXPORT PyObject *PLy_input_convert(PLyDatumToOb *arg, Datum val);
extern PGDLLEXPORT Datum PLy_output_convert(PLyObToDatum *arg, PyObject *val,
											bool *isnull);

extern PGDLLEXPORT PyObject *PLy_input_from_tuple(PLyDatumToOb *arg, HeapTuple tuple,
												  TupleDesc desc, bool include_generated);

extern PGDLLEXPORT void PLy_input_setup_func(PLyDatumToOb *arg, MemoryContext arg_mcxt,
											 Oid typeOid, int32 typmod,
											 struct PLyProcedure *proc);
extern PGDLLEXPORT void PLy_output_setup_func(PLyObToDatum *arg, MemoryContext arg_mcxt,
											  Oid typeOid, int32 typmod,
											  struct PLyProcedure *proc);

extern PGDLLEXPORT void PLy_input_setup_tuple(PLyDatumToOb *arg, TupleDesc desc,
											  struct PLyProcedure *proc);
extern PGDLLEXPORT void PLy_output_setup_tuple(PLyObToDatum *arg, TupleDesc desc,
											   struct PLyProcedure *proc);

extern PGDLLEXPORT void PLy_output_setup_record(PLyObToDatum *arg, TupleDesc desc,
												struct PLyProcedure *proc);

/* conversion from Python objects to C strings --- exported for transforms */
extern PGDLLEXPORT char *PLyObject_AsString(PyObject *plrv);

#endif							/* PLPY_TYPEIO_H */
