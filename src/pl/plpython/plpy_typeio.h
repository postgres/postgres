/*
 * src/pl/plpython/plpy_typeio.h
 */

#ifndef PLPY_TYPEIO_H
#define PLPY_TYPEIO_H

#include "access/htup.h"
#include "access/tupdesc.h"
#include "fmgr.h"
#include "storage/itemptr.h"

struct PLyDatumToOb;
typedef PyObject *(*PLyDatumToObFunc) (struct PLyDatumToOb *, Datum);

typedef struct PLyDatumToOb
{
	PLyDatumToObFunc func;
	FmgrInfo	typfunc;		/* The type's output function */
	Oid			typoid;			/* The OID of the type */
	int32		typmod;			/* The typmod of the type */
	Oid			typioparam;
	bool		typbyval;
	int16		typlen;
	char		typalign;
	struct PLyDatumToOb *elm;
} PLyDatumToOb;

typedef struct PLyTupleToOb
{
	PLyDatumToOb *atts;
	int			natts;
} PLyTupleToOb;

typedef union PLyTypeInput
{
	PLyDatumToOb d;
	PLyTupleToOb r;
} PLyTypeInput;

/* convert PyObject to a Postgresql Datum or tuple.
 * output from Python
 */
struct PLyObToDatum;
typedef Datum (*PLyObToDatumFunc) (struct PLyObToDatum *, int32, PyObject *);

typedef struct PLyObToDatum
{
	PLyObToDatumFunc func;
	FmgrInfo	typfunc;		/* The type's input function */
	Oid			typoid;			/* The OID of the type */
	int32		typmod;			/* The typmod of the type */
	Oid			typioparam;
	bool		typbyval;
	int16		typlen;
	char		typalign;
	struct PLyObToDatum *elm;
} PLyObToDatum;

typedef struct PLyObToTuple
{
	PLyObToDatum *atts;
	int			natts;
} PLyObToTuple;

typedef union PLyTypeOutput
{
	PLyObToDatum d;
	PLyObToTuple r;
} PLyTypeOutput;

/* all we need to move Postgresql data to Python objects,
 * and vice versa
 */
typedef struct PLyTypeInfo
{
	PLyTypeInput in;
	PLyTypeOutput out;

	/*
	 * is_rowtype can be: -1 = not known yet (initial state); 0 = scalar
	 * datatype; 1 = rowtype; 2 = rowtype, but I/O functions not set up yet
	 */
	int			is_rowtype;
	/* used to check if the type has been modified */
	Oid			typ_relid;
	TransactionId typrel_xmin;
	ItemPointerData typrel_tid;
} PLyTypeInfo;

extern void PLy_typeinfo_init(PLyTypeInfo *arg);
extern void PLy_typeinfo_dealloc(PLyTypeInfo *arg);

extern void PLy_input_datum_func(PLyTypeInfo *arg, Oid typeOid, HeapTuple typeTup);
extern void PLy_output_datum_func(PLyTypeInfo *arg, HeapTuple typeTup);

extern void PLy_input_tuple_funcs(PLyTypeInfo *arg, TupleDesc desc);
extern void PLy_output_tuple_funcs(PLyTypeInfo *arg, TupleDesc desc);

extern void PLy_output_record_funcs(PLyTypeInfo *arg, TupleDesc desc);

/* conversion from Python objects to composite Datums */
extern Datum PLyObject_ToCompositeDatum(PLyTypeInfo *info, TupleDesc desc, PyObject *plrv);

/* conversion from heap tuples to Python dictionaries */
extern PyObject *PLyDict_FromTuple(PLyTypeInfo *info, HeapTuple tuple, TupleDesc desc);

#endif   /* PLPY_TYPEIO_H */
