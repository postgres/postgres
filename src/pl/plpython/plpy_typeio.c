/*
 * transforming Datums to Python objects and vice versa
 *
 * src/pl/plpython/plpy_typeio.c
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "plpy_elog.h"
#include "plpy_main.h"
#include "plpy_typeio.h"
#include "plpy_util.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

/* conversion from Datums to Python objects */
static PyObject *PLyBool_FromBool(PLyDatumToOb *arg, Datum d);
static PyObject *PLyFloat_FromFloat4(PLyDatumToOb *arg, Datum d);
static PyObject *PLyFloat_FromFloat8(PLyDatumToOb *arg, Datum d);
static PyObject *PLyDecimal_FromNumeric(PLyDatumToOb *arg, Datum d);
static PyObject *PLyLong_FromInt16(PLyDatumToOb *arg, Datum d);
static PyObject *PLyLong_FromInt32(PLyDatumToOb *arg, Datum d);
static PyObject *PLyLong_FromInt64(PLyDatumToOb *arg, Datum d);
static PyObject *PLyLong_FromOid(PLyDatumToOb *arg, Datum d);
static PyObject *PLyBytes_FromBytea(PLyDatumToOb *arg, Datum d);
static PyObject *PLyUnicode_FromScalar(PLyDatumToOb *arg, Datum d);
static PyObject *PLyObject_FromTransform(PLyDatumToOb *arg, Datum d);
static PyObject *PLyList_FromArray(PLyDatumToOb *arg, Datum d);
static PyObject *PLyList_FromArray_recurse(PLyDatumToOb *elm, int *dims, int ndim, int dim,
										   char **dataptr_p, bits8 **bitmap_p, int *bitmask_p);
static PyObject *PLyDict_FromComposite(PLyDatumToOb *arg, Datum d);
static PyObject *PLyDict_FromTuple(PLyDatumToOb *arg, HeapTuple tuple, TupleDesc desc, bool include_generated);

/* conversion from Python objects to Datums */
static Datum PLyObject_ToBool(PLyObToDatum *arg, PyObject *plrv,
							  bool *isnull, bool inarray);
static Datum PLyObject_ToBytea(PLyObToDatum *arg, PyObject *plrv,
							   bool *isnull, bool inarray);
static Datum PLyObject_ToComposite(PLyObToDatum *arg, PyObject *plrv,
								   bool *isnull, bool inarray);
static Datum PLyObject_ToScalar(PLyObToDatum *arg, PyObject *plrv,
								bool *isnull, bool inarray);
static Datum PLyObject_ToDomain(PLyObToDatum *arg, PyObject *plrv,
								bool *isnull, bool inarray);
static Datum PLyObject_ToTransform(PLyObToDatum *arg, PyObject *plrv,
								   bool *isnull, bool inarray);
static Datum PLySequence_ToArray(PLyObToDatum *arg, PyObject *plrv,
								 bool *isnull, bool inarray);
static void PLySequence_ToArray_recurse(PyObject *obj,
										ArrayBuildState **astatep,
										int *ndims, int *dims, int cur_depth,
										PLyObToDatum *elm, Oid elmbasetype);

/* conversion from Python objects to composite Datums */
static Datum PLyUnicode_ToComposite(PLyObToDatum *arg, PyObject *string, bool inarray);
static Datum PLyMapping_ToComposite(PLyObToDatum *arg, TupleDesc desc, PyObject *mapping);
static Datum PLySequence_ToComposite(PLyObToDatum *arg, TupleDesc desc, PyObject *sequence);
static Datum PLyGenericObject_ToComposite(PLyObToDatum *arg, TupleDesc desc, PyObject *object, bool inarray);


/*
 * Conversion functions.  Remember output from Python is input to
 * PostgreSQL, and vice versa.
 */

/*
 * Perform input conversion, given correctly-set-up state information.
 *
 * This is the outer-level entry point for any input conversion.  Internally,
 * the conversion functions recurse directly to each other.
 */
PyObject *
PLy_input_convert(PLyDatumToOb *arg, Datum val)
{
	PyObject   *result;
	PLyExecutionContext *exec_ctx = PLy_current_execution_context();
	MemoryContext scratch_context = PLy_get_scratch_context(exec_ctx);
	MemoryContext oldcontext;

	/*
	 * Do the work in the scratch context to avoid leaking memory from the
	 * datatype output function calls.  (The individual PLyDatumToObFunc
	 * functions can't reset the scratch context, because they recurse and an
	 * inner one might clobber data an outer one still needs.  So we do it
	 * once at the outermost recursion level.)
	 *
	 * We reset the scratch context before, not after, each conversion cycle.
	 * This way we aren't on the hook to release a Python refcount on the
	 * result object in case MemoryContextReset throws an error.
	 */
	MemoryContextReset(scratch_context);

	oldcontext = MemoryContextSwitchTo(scratch_context);

	result = arg->func(arg, val);

	MemoryContextSwitchTo(oldcontext);

	return result;
}

/*
 * Perform output conversion, given correctly-set-up state information.
 *
 * This is the outer-level entry point for any output conversion.  Internally,
 * the conversion functions recurse directly to each other.
 *
 * The result, as well as any cruft generated along the way, are in the
 * current memory context.  Caller is responsible for cleanup.
 */
Datum
PLy_output_convert(PLyObToDatum *arg, PyObject *val, bool *isnull)
{
	/* at outer level, we are not considering an array element */
	return arg->func(arg, val, isnull, false);
}

/*
 * Transform a tuple into a Python dict object.
 *
 * Note: the tupdesc must match the one used to set up *arg.  We could
 * insist that this function lookup the tupdesc from what is in *arg,
 * but in practice all callers have the right tupdesc available.
 */
PyObject *
PLy_input_from_tuple(PLyDatumToOb *arg, HeapTuple tuple, TupleDesc desc, bool include_generated)
{
	PyObject   *dict;
	PLyExecutionContext *exec_ctx = PLy_current_execution_context();
	MemoryContext scratch_context = PLy_get_scratch_context(exec_ctx);
	MemoryContext oldcontext;

	/*
	 * As in PLy_input_convert, do the work in the scratch context.
	 */
	MemoryContextReset(scratch_context);

	oldcontext = MemoryContextSwitchTo(scratch_context);

	dict = PLyDict_FromTuple(arg, tuple, desc, include_generated);

	MemoryContextSwitchTo(oldcontext);

	return dict;
}

/*
 * Initialize, or re-initialize, per-column input info for a composite type.
 *
 * This is separate from PLy_input_setup_func() because in cases involving
 * anonymous record types, we need to be passed the tupdesc explicitly.
 * It's caller's responsibility that the tupdesc has adequate lifespan
 * in such cases.  If the tupdesc is for a named composite or registered
 * record type, it does not need to be long-lived.
 */
void
PLy_input_setup_tuple(PLyDatumToOb *arg, TupleDesc desc, PLyProcedure *proc)
{
	int			i;

	/* We should be working on a previously-set-up struct */
	Assert(arg->func == PLyDict_FromComposite);

	/* Save pointer to tupdesc, but only if this is an anonymous record type */
	if (arg->typoid == RECORDOID && arg->typmod < 0)
		arg->u.tuple.recdesc = desc;

	/* (Re)allocate atts array as needed */
	if (arg->u.tuple.natts != desc->natts)
	{
		if (arg->u.tuple.atts)
			pfree(arg->u.tuple.atts);
		arg->u.tuple.natts = desc->natts;
		arg->u.tuple.atts = (PLyDatumToOb *)
			MemoryContextAllocZero(arg->mcxt,
								   desc->natts * sizeof(PLyDatumToOb));
	}

	/* Fill the atts entries, except for dropped columns */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		PLyDatumToOb *att = &arg->u.tuple.atts[i];

		if (attr->attisdropped)
			continue;

		if (att->typoid == attr->atttypid && att->typmod == attr->atttypmod)
			continue;			/* already set up this entry */

		PLy_input_setup_func(att, arg->mcxt,
							 attr->atttypid, attr->atttypmod,
							 proc);
	}
}

/*
 * Initialize, or re-initialize, per-column output info for a composite type.
 *
 * This is separate from PLy_output_setup_func() because in cases involving
 * anonymous record types, we need to be passed the tupdesc explicitly.
 * It's caller's responsibility that the tupdesc has adequate lifespan
 * in such cases.  If the tupdesc is for a named composite or registered
 * record type, it does not need to be long-lived.
 */
void
PLy_output_setup_tuple(PLyObToDatum *arg, TupleDesc desc, PLyProcedure *proc)
{
	int			i;

	/* We should be working on a previously-set-up struct */
	Assert(arg->func == PLyObject_ToComposite);

	/* Save pointer to tupdesc, but only if this is an anonymous record type */
	if (arg->typoid == RECORDOID && arg->typmod < 0)
		arg->u.tuple.recdesc = desc;

	/* (Re)allocate atts array as needed */
	if (arg->u.tuple.natts != desc->natts)
	{
		if (arg->u.tuple.atts)
			pfree(arg->u.tuple.atts);
		arg->u.tuple.natts = desc->natts;
		arg->u.tuple.atts = (PLyObToDatum *)
			MemoryContextAllocZero(arg->mcxt,
								   desc->natts * sizeof(PLyObToDatum));
	}

	/* Fill the atts entries, except for dropped columns */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);
		PLyObToDatum *att = &arg->u.tuple.atts[i];

		if (attr->attisdropped)
			continue;

		if (att->typoid == attr->atttypid && att->typmod == attr->atttypmod)
			continue;			/* already set up this entry */

		PLy_output_setup_func(att, arg->mcxt,
							  attr->atttypid, attr->atttypmod,
							  proc);
	}
}

/*
 * Set up output info for a PL/Python function returning record.
 *
 * Note: the given tupdesc is not necessarily long-lived.
 */
void
PLy_output_setup_record(PLyObToDatum *arg, TupleDesc desc, PLyProcedure *proc)
{
	/* Makes no sense unless RECORD */
	Assert(arg->typoid == RECORDOID);
	Assert(desc->tdtypeid == RECORDOID);

	/*
	 * Bless the record type if not already done.  We'd have to do this anyway
	 * to return a tuple, so we might as well force the issue so we can use
	 * the known-record-type code path.
	 */
	BlessTupleDesc(desc);

	/*
	 * Update arg->typmod, and clear the recdesc link if it's changed. The
	 * next call of PLyObject_ToComposite will look up a long-lived tupdesc
	 * for the record type.
	 */
	arg->typmod = desc->tdtypmod;
	if (arg->u.tuple.recdesc &&
		arg->u.tuple.recdesc->tdtypmod != arg->typmod)
		arg->u.tuple.recdesc = NULL;

	/* Update derived data if necessary */
	PLy_output_setup_tuple(arg, desc, proc);
}

/*
 * Recursively initialize the PLyObToDatum structure(s) needed to construct
 * a SQL value of the specified typeOid/typmod from a Python value.
 * (But note that at this point we may have RECORDOID/-1, ie, an indeterminate
 * record type.)
 * proc is used to look up transform functions.
 */
void
PLy_output_setup_func(PLyObToDatum *arg, MemoryContext arg_mcxt,
					  Oid typeOid, int32 typmod,
					  PLyProcedure *proc)
{
	TypeCacheEntry *typentry;
	char		typtype;
	Oid			trfuncid;
	Oid			typinput;

	/* Since this is recursive, it could theoretically be driven to overflow */
	check_stack_depth();

	arg->typoid = typeOid;
	arg->typmod = typmod;
	arg->mcxt = arg_mcxt;

	/*
	 * Fetch typcache entry for the target type, asking for whatever info
	 * we'll need later.  RECORD is a special case: just treat it as composite
	 * without bothering with the typcache entry.
	 */
	if (typeOid != RECORDOID)
	{
		typentry = lookup_type_cache(typeOid, TYPECACHE_DOMAIN_BASE_INFO);
		typtype = typentry->typtype;
		arg->typbyval = typentry->typbyval;
		arg->typlen = typentry->typlen;
		arg->typalign = typentry->typalign;
	}
	else
	{
		typentry = NULL;
		typtype = TYPTYPE_COMPOSITE;
		/* hard-wired knowledge about type RECORD: */
		arg->typbyval = false;
		arg->typlen = -1;
		arg->typalign = TYPALIGN_DOUBLE;
	}

	/*
	 * Choose conversion method.  Note that transform functions are checked
	 * for composite and scalar types, but not for arrays or domains.  This is
	 * somewhat historical, but we'd have a problem allowing them on domains,
	 * since we drill down through all levels of a domain nest without looking
	 * at the intermediate levels at all.
	 */
	if (typtype == TYPTYPE_DOMAIN)
	{
		/* Domain */
		arg->func = PLyObject_ToDomain;
		arg->u.domain.domain_info = NULL;
		/* Recursively set up conversion info for the element type */
		arg->u.domain.base = (PLyObToDatum *)
			MemoryContextAllocZero(arg_mcxt, sizeof(PLyObToDatum));
		PLy_output_setup_func(arg->u.domain.base, arg_mcxt,
							  typentry->domainBaseType,
							  typentry->domainBaseTypmod,
							  proc);
	}
	else if (typentry &&
			 IsTrueArrayType(typentry))
	{
		/* Standard array */
		arg->func = PLySequence_ToArray;
		/* Get base type OID to insert into constructed array */
		/* (note this might not be the same as the immediate child type) */
		arg->u.array.elmbasetype = getBaseType(typentry->typelem);
		/* Recursively set up conversion info for the element type */
		arg->u.array.elm = (PLyObToDatum *)
			MemoryContextAllocZero(arg_mcxt, sizeof(PLyObToDatum));
		PLy_output_setup_func(arg->u.array.elm, arg_mcxt,
							  typentry->typelem, typmod,
							  proc);
	}
	else if ((trfuncid = get_transform_tosql(typeOid,
											 proc->langid,
											 proc->trftypes)))
	{
		arg->func = PLyObject_ToTransform;
		fmgr_info_cxt(trfuncid, &arg->u.transform.typtransform, arg_mcxt);
	}
	else if (typtype == TYPTYPE_COMPOSITE)
	{
		/* Named composite type, or RECORD */
		arg->func = PLyObject_ToComposite;
		/* We'll set up the per-field data later */
		arg->u.tuple.recdesc = NULL;
		arg->u.tuple.typentry = typentry;
		arg->u.tuple.tupdescid = INVALID_TUPLEDESC_IDENTIFIER;
		arg->u.tuple.atts = NULL;
		arg->u.tuple.natts = 0;
		/* Mark this invalid till needed, too */
		arg->u.tuple.recinfunc.fn_oid = InvalidOid;
	}
	else
	{
		/* Scalar type, but we have a couple of special cases */
		switch (typeOid)
		{
			case BOOLOID:
				arg->func = PLyObject_ToBool;
				break;
			case BYTEAOID:
				arg->func = PLyObject_ToBytea;
				break;
			default:
				arg->func = PLyObject_ToScalar;
				getTypeInputInfo(typeOid, &typinput, &arg->u.scalar.typioparam);
				fmgr_info_cxt(typinput, &arg->u.scalar.typfunc, arg_mcxt);
				break;
		}
	}
}

/*
 * Recursively initialize the PLyDatumToOb structure(s) needed to construct
 * a Python value from a SQL value of the specified typeOid/typmod.
 * (But note that at this point we may have RECORDOID/-1, ie, an indeterminate
 * record type.)
 * proc is used to look up transform functions.
 */
void
PLy_input_setup_func(PLyDatumToOb *arg, MemoryContext arg_mcxt,
					 Oid typeOid, int32 typmod,
					 PLyProcedure *proc)
{
	TypeCacheEntry *typentry;
	char		typtype;
	Oid			trfuncid;
	Oid			typoutput;
	bool		typisvarlena;

	/* Since this is recursive, it could theoretically be driven to overflow */
	check_stack_depth();

	arg->typoid = typeOid;
	arg->typmod = typmod;
	arg->mcxt = arg_mcxt;

	/*
	 * Fetch typcache entry for the target type, asking for whatever info
	 * we'll need later.  RECORD is a special case: just treat it as composite
	 * without bothering with the typcache entry.
	 */
	if (typeOid != RECORDOID)
	{
		typentry = lookup_type_cache(typeOid, TYPECACHE_DOMAIN_BASE_INFO);
		typtype = typentry->typtype;
		arg->typbyval = typentry->typbyval;
		arg->typlen = typentry->typlen;
		arg->typalign = typentry->typalign;
	}
	else
	{
		typentry = NULL;
		typtype = TYPTYPE_COMPOSITE;
		/* hard-wired knowledge about type RECORD: */
		arg->typbyval = false;
		arg->typlen = -1;
		arg->typalign = TYPALIGN_DOUBLE;
	}

	/*
	 * Choose conversion method.  Note that transform functions are checked
	 * for composite and scalar types, but not for arrays or domains.  This is
	 * somewhat historical, but we'd have a problem allowing them on domains,
	 * since we drill down through all levels of a domain nest without looking
	 * at the intermediate levels at all.
	 */
	if (typtype == TYPTYPE_DOMAIN)
	{
		/* Domain --- we don't care, just recurse down to the base type */
		PLy_input_setup_func(arg, arg_mcxt,
							 typentry->domainBaseType,
							 typentry->domainBaseTypmod,
							 proc);
	}
	else if (typentry &&
			 IsTrueArrayType(typentry))
	{
		/* Standard array */
		arg->func = PLyList_FromArray;
		/* Recursively set up conversion info for the element type */
		arg->u.array.elm = (PLyDatumToOb *)
			MemoryContextAllocZero(arg_mcxt, sizeof(PLyDatumToOb));
		PLy_input_setup_func(arg->u.array.elm, arg_mcxt,
							 typentry->typelem, typmod,
							 proc);
	}
	else if ((trfuncid = get_transform_fromsql(typeOid,
											   proc->langid,
											   proc->trftypes)))
	{
		arg->func = PLyObject_FromTransform;
		fmgr_info_cxt(trfuncid, &arg->u.transform.typtransform, arg_mcxt);
	}
	else if (typtype == TYPTYPE_COMPOSITE)
	{
		/* Named composite type, or RECORD */
		arg->func = PLyDict_FromComposite;
		/* We'll set up the per-field data later */
		arg->u.tuple.recdesc = NULL;
		arg->u.tuple.typentry = typentry;
		arg->u.tuple.tupdescid = INVALID_TUPLEDESC_IDENTIFIER;
		arg->u.tuple.atts = NULL;
		arg->u.tuple.natts = 0;
	}
	else
	{
		/* Scalar type, but we have a couple of special cases */
		switch (typeOid)
		{
			case BOOLOID:
				arg->func = PLyBool_FromBool;
				break;
			case FLOAT4OID:
				arg->func = PLyFloat_FromFloat4;
				break;
			case FLOAT8OID:
				arg->func = PLyFloat_FromFloat8;
				break;
			case NUMERICOID:
				arg->func = PLyDecimal_FromNumeric;
				break;
			case INT2OID:
				arg->func = PLyLong_FromInt16;
				break;
			case INT4OID:
				arg->func = PLyLong_FromInt32;
				break;
			case INT8OID:
				arg->func = PLyLong_FromInt64;
				break;
			case OIDOID:
				arg->func = PLyLong_FromOid;
				break;
			case BYTEAOID:
				arg->func = PLyBytes_FromBytea;
				break;
			default:
				arg->func = PLyUnicode_FromScalar;
				getTypeOutputInfo(typeOid, &typoutput, &typisvarlena);
				fmgr_info_cxt(typoutput, &arg->u.scalar.typfunc, arg_mcxt);
				break;
		}
	}
}


/*
 * Special-purpose input converters.
 */

static PyObject *
PLyBool_FromBool(PLyDatumToOb *arg, Datum d)
{
	if (DatumGetBool(d))
		Py_RETURN_TRUE;
	Py_RETURN_FALSE;
}

static PyObject *
PLyFloat_FromFloat4(PLyDatumToOb *arg, Datum d)
{
	return PyFloat_FromDouble(DatumGetFloat4(d));
}

static PyObject *
PLyFloat_FromFloat8(PLyDatumToOb *arg, Datum d)
{
	return PyFloat_FromDouble(DatumGetFloat8(d));
}

static PyObject *
PLyDecimal_FromNumeric(PLyDatumToOb *arg, Datum d)
{
	static PyObject *decimal_constructor;
	char	   *str;
	PyObject   *pyvalue;

	/* Try to import cdecimal.  If it doesn't exist, fall back to decimal. */
	if (!decimal_constructor)
	{
		PyObject   *decimal_module;

		decimal_module = PyImport_ImportModule("cdecimal");
		if (!decimal_module)
		{
			PyErr_Clear();
			decimal_module = PyImport_ImportModule("decimal");
		}
		if (!decimal_module)
			PLy_elog(ERROR, "could not import a module for Decimal constructor");

		decimal_constructor = PyObject_GetAttrString(decimal_module, "Decimal");
		if (!decimal_constructor)
			PLy_elog(ERROR, "no Decimal attribute in module");
	}

	str = DatumGetCString(DirectFunctionCall1(numeric_out, d));
	pyvalue = PyObject_CallFunction(decimal_constructor, "s", str);
	if (!pyvalue)
		PLy_elog(ERROR, "conversion from numeric to Decimal failed");

	return pyvalue;
}

static PyObject *
PLyLong_FromInt16(PLyDatumToOb *arg, Datum d)
{
	return PyLong_FromLong(DatumGetInt16(d));
}

static PyObject *
PLyLong_FromInt32(PLyDatumToOb *arg, Datum d)
{
	return PyLong_FromLong(DatumGetInt32(d));
}

static PyObject *
PLyLong_FromInt64(PLyDatumToOb *arg, Datum d)
{
	return PyLong_FromLongLong(DatumGetInt64(d));
}

static PyObject *
PLyLong_FromOid(PLyDatumToOb *arg, Datum d)
{
	return PyLong_FromUnsignedLong(DatumGetObjectId(d));
}

static PyObject *
PLyBytes_FromBytea(PLyDatumToOb *arg, Datum d)
{
	text	   *txt = DatumGetByteaPP(d);
	char	   *str = VARDATA_ANY(txt);
	size_t		size = VARSIZE_ANY_EXHDR(txt);

	return PyBytes_FromStringAndSize(str, size);
}


/*
 * Generic input conversion using a SQL type's output function.
 */
static PyObject *
PLyUnicode_FromScalar(PLyDatumToOb *arg, Datum d)
{
	char	   *x = OutputFunctionCall(&arg->u.scalar.typfunc, d);
	PyObject   *r = PLyUnicode_FromString(x);

	pfree(x);
	return r;
}

/*
 * Convert using a from-SQL transform function.
 */
static PyObject *
PLyObject_FromTransform(PLyDatumToOb *arg, Datum d)
{
	Datum		t;

	t = FunctionCall1(&arg->u.transform.typtransform, d);
	return (PyObject *) DatumGetPointer(t);
}

/*
 * Convert a SQL array to a Python list.
 */
static PyObject *
PLyList_FromArray(PLyDatumToOb *arg, Datum d)
{
	ArrayType  *array = DatumGetArrayTypeP(d);
	PLyDatumToOb *elm = arg->u.array.elm;
	int			ndim;
	int		   *dims;
	char	   *dataptr;
	bits8	   *bitmap;
	int			bitmask;

	if (ARR_NDIM(array) == 0)
		return PyList_New(0);

	/* Array dimensions and left bounds */
	ndim = ARR_NDIM(array);
	dims = ARR_DIMS(array);
	Assert(ndim <= MAXDIM);

	/*
	 * We iterate the SQL array in the physical order it's stored in the
	 * datum. For example, for a 3-dimensional array the order of iteration
	 * would be the following: [0,0,0] elements through [0,0,k], then [0,1,0]
	 * through [0,1,k] till [0,m,k], then [1,0,0] through [1,0,k] till
	 * [1,m,k], and so on.
	 *
	 * In Python, there are no multi-dimensional lists as such, but they are
	 * represented as a list of lists. So a 3-d array of [n,m,k] elements is a
	 * list of n m-element arrays, each element of which is k-element array.
	 * PLyList_FromArray_recurse() builds the Python list for a single
	 * dimension, and recurses for the next inner dimension.
	 */
	dataptr = ARR_DATA_PTR(array);
	bitmap = ARR_NULLBITMAP(array);
	bitmask = 1;

	return PLyList_FromArray_recurse(elm, dims, ndim, 0,
									 &dataptr, &bitmap, &bitmask);
}

static PyObject *
PLyList_FromArray_recurse(PLyDatumToOb *elm, int *dims, int ndim, int dim,
						  char **dataptr_p, bits8 **bitmap_p, int *bitmask_p)
{
	int			i;
	PyObject   *list;

	list = PyList_New(dims[dim]);
	if (!list)
		return NULL;

	if (dim < ndim - 1)
	{
		/* Outer dimension. Recurse for each inner slice. */
		for (i = 0; i < dims[dim]; i++)
		{
			PyObject   *sublist;

			sublist = PLyList_FromArray_recurse(elm, dims, ndim, dim + 1,
												dataptr_p, bitmap_p, bitmask_p);
			PyList_SetItem(list, i, sublist);
		}
	}
	else
	{
		/*
		 * Innermost dimension. Fill the list with the values from the array
		 * for this slice.
		 */
		char	   *dataptr = *dataptr_p;
		bits8	   *bitmap = *bitmap_p;
		int			bitmask = *bitmask_p;

		for (i = 0; i < dims[dim]; i++)
		{
			/* checking for NULL */
			if (bitmap && (*bitmap & bitmask) == 0)
			{
				Py_INCREF(Py_None);
				PyList_SetItem(list, i, Py_None);
			}
			else
			{
				Datum		itemvalue;

				itemvalue = fetch_att(dataptr, elm->typbyval, elm->typlen);
				PyList_SetItem(list, i, elm->func(elm, itemvalue));
				dataptr = att_addlength_pointer(dataptr, elm->typlen, dataptr);
				dataptr = (char *) att_align_nominal(dataptr, elm->typalign);
			}

			/* advance bitmap pointer if any */
			if (bitmap)
			{
				bitmask <<= 1;
				if (bitmask == 0x100 /* (1<<8) */ )
				{
					bitmap++;
					bitmask = 1;
				}
			}
		}

		*dataptr_p = dataptr;
		*bitmap_p = bitmap;
		*bitmask_p = bitmask;
	}

	return list;
}

/*
 * Convert a composite SQL value to a Python dict.
 */
static PyObject *
PLyDict_FromComposite(PLyDatumToOb *arg, Datum d)
{
	PyObject   *dict;
	HeapTupleHeader td;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tmptup;

	td = DatumGetHeapTupleHeader(d);
	/* Extract rowtype info and find a tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	/* Set up I/O funcs if not done yet */
	PLy_input_setup_tuple(arg, tupdesc,
						  PLy_current_execution_context()->curr_proc);

	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	tmptup.t_data = td;

	dict = PLyDict_FromTuple(arg, &tmptup, tupdesc, true);

	ReleaseTupleDesc(tupdesc);

	return dict;
}

/*
 * Transform a tuple into a Python dict object.
 */
static PyObject *
PLyDict_FromTuple(PLyDatumToOb *arg, HeapTuple tuple, TupleDesc desc, bool include_generated)
{
	PyObject   *volatile dict;

	/* Simple sanity check that desc matches */
	Assert(desc->natts == arg->u.tuple.natts);

	dict = PyDict_New();
	if (dict == NULL)
		return NULL;

	PG_TRY();
	{
		int			i;

		for (i = 0; i < arg->u.tuple.natts; i++)
		{
			PLyDatumToOb *att = &arg->u.tuple.atts[i];
			Form_pg_attribute attr = TupleDescAttr(desc, i);
			char	   *key;
			Datum		vattr;
			bool		is_null;
			PyObject   *value;

			if (attr->attisdropped)
				continue;

			if (attr->attgenerated)
			{
				/* don't include unless requested */
				if (!include_generated)
					continue;
				/* never include virtual columns */
				if (attr->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
					continue;
			}

			key = NameStr(attr->attname);
			vattr = heap_getattr(tuple, (i + 1), desc, &is_null);

			if (is_null)
				PyDict_SetItemString(dict, key, Py_None);
			else
			{
				value = att->func(att, vattr);
				PyDict_SetItemString(dict, key, value);
				Py_DECREF(value);
			}
		}
	}
	PG_CATCH();
	{
		Py_DECREF(dict);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return dict;
}

/*
 * Convert a Python object to a PostgreSQL bool datum.  This can't go
 * through the generic conversion function, because Python attaches a
 * Boolean value to everything, more things than the PostgreSQL bool
 * type can parse.
 */
static Datum
PLyObject_ToBool(PLyObToDatum *arg, PyObject *plrv,
				 bool *isnull, bool inarray)
{
	if (plrv == Py_None)
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;
	return BoolGetDatum(PyObject_IsTrue(plrv));
}

/*
 * Convert a Python object to a PostgreSQL bytea datum.  This doesn't
 * go through the generic conversion function to circumvent problems
 * with embedded nulls.  And it's faster this way.
 */
static Datum
PLyObject_ToBytea(PLyObToDatum *arg, PyObject *plrv,
				  bool *isnull, bool inarray)
{
	PyObject   *volatile plrv_so = NULL;
	Datum		rv = (Datum) 0;

	if (plrv == Py_None)
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;

	plrv_so = PyObject_Bytes(plrv);
	if (!plrv_so)
		PLy_elog(ERROR, "could not create bytes representation of Python object");

	PG_TRY();
	{
		char	   *plrv_sc = PyBytes_AsString(plrv_so);
		size_t		len = PyBytes_Size(plrv_so);
		size_t		size = len + VARHDRSZ;
		bytea	   *result = palloc(size);

		SET_VARSIZE(result, size);
		memcpy(VARDATA(result), plrv_sc, len);
		rv = PointerGetDatum(result);
	}
	PG_FINALLY();
	{
		Py_XDECREF(plrv_so);
	}
	PG_END_TRY();

	return rv;
}


/*
 * Convert a Python object to a composite type. First look up the type's
 * description, then route the Python object through the conversion function
 * for obtaining PostgreSQL tuples.
 */
static Datum
PLyObject_ToComposite(PLyObToDatum *arg, PyObject *plrv,
					  bool *isnull, bool inarray)
{
	Datum		rv;
	TupleDesc	desc;

	if (plrv == Py_None)
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;

	/*
	 * The string conversion case doesn't require a tupdesc, nor per-field
	 * conversion data, so just go for it if that's the case to use.
	 */
	if (PyUnicode_Check(plrv))
		return PLyUnicode_ToComposite(arg, plrv, inarray);

	/*
	 * If we're dealing with a named composite type, we must look up the
	 * tupdesc every time, to protect against possible changes to the type.
	 * RECORD types can't change between calls; but we must still be willing
	 * to set up the info the first time, if nobody did yet.
	 */
	if (arg->typoid != RECORDOID)
	{
		desc = lookup_rowtype_tupdesc(arg->typoid, arg->typmod);
		/* We should have the descriptor of the type's typcache entry */
		Assert(desc == arg->u.tuple.typentry->tupDesc);
		/* Detect change of descriptor, update cache if needed */
		if (arg->u.tuple.tupdescid != arg->u.tuple.typentry->tupDesc_identifier)
		{
			PLy_output_setup_tuple(arg, desc,
								   PLy_current_execution_context()->curr_proc);
			arg->u.tuple.tupdescid = arg->u.tuple.typentry->tupDesc_identifier;
		}
	}
	else
	{
		desc = arg->u.tuple.recdesc;
		if (desc == NULL)
		{
			desc = lookup_rowtype_tupdesc(arg->typoid, arg->typmod);
			arg->u.tuple.recdesc = desc;
		}
		else
		{
			/* Pin descriptor to match unpin below */
			PinTupleDesc(desc);
		}
	}

	/* Simple sanity check on our caching */
	Assert(desc->natts == arg->u.tuple.natts);

	/*
	 * Convert, using the appropriate method depending on the type of the
	 * supplied Python object.
	 */
	if (PySequence_Check(plrv))
		/* composite type as sequence (tuple, list etc) */
		rv = PLySequence_ToComposite(arg, desc, plrv);
	else if (PyMapping_Check(plrv))
		/* composite type as mapping (currently only dict) */
		rv = PLyMapping_ToComposite(arg, desc, plrv);
	else
		/* returned as smth, must provide method __getattr__(name) */
		rv = PLyGenericObject_ToComposite(arg, desc, plrv, inarray);

	ReleaseTupleDesc(desc);

	return rv;
}


/*
 * Convert Python object to C string in server encoding.
 *
 * Note: this is exported for use by add-on transform modules.
 */
char *
PLyObject_AsString(PyObject *plrv)
{
	PyObject   *plrv_bo;
	char	   *plrv_sc;
	size_t		plen;
	size_t		slen;

	if (PyUnicode_Check(plrv))
		plrv_bo = PLyUnicode_Bytes(plrv);
	else if (PyFloat_Check(plrv))
	{
		/* use repr() for floats, str() is lossy */
		PyObject   *s = PyObject_Repr(plrv);

		plrv_bo = PLyUnicode_Bytes(s);
		Py_XDECREF(s);
	}
	else
	{
		PyObject   *s = PyObject_Str(plrv);

		plrv_bo = PLyUnicode_Bytes(s);
		Py_XDECREF(s);
	}
	if (!plrv_bo)
		PLy_elog(ERROR, "could not create string representation of Python object");

	plrv_sc = pstrdup(PyBytes_AsString(plrv_bo));
	plen = PyBytes_Size(plrv_bo);
	slen = strlen(plrv_sc);

	Py_XDECREF(plrv_bo);

	if (slen < plen)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("could not convert Python object into cstring: Python string representation appears to contain null bytes")));
	else if (slen > plen)
		elog(ERROR, "could not convert Python object into cstring: Python string longer than reported length");
	pg_verifymbstr(plrv_sc, slen, false);

	return plrv_sc;
}


/*
 * Generic output conversion function: convert PyObject to cstring and
 * cstring into PostgreSQL type.
 */
static Datum
PLyObject_ToScalar(PLyObToDatum *arg, PyObject *plrv,
				   bool *isnull, bool inarray)
{
	char	   *str;

	if (plrv == Py_None)
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;

	str = PLyObject_AsString(plrv);

	return InputFunctionCall(&arg->u.scalar.typfunc,
							 str,
							 arg->u.scalar.typioparam,
							 arg->typmod);
}


/*
 * Convert to a domain type.
 */
static Datum
PLyObject_ToDomain(PLyObToDatum *arg, PyObject *plrv,
				   bool *isnull, bool inarray)
{
	Datum		result;
	PLyObToDatum *base = arg->u.domain.base;

	result = base->func(base, plrv, isnull, inarray);
	domain_check(result, *isnull, arg->typoid,
				 &arg->u.domain.domain_info, arg->mcxt);
	return result;
}


/*
 * Convert using a to-SQL transform function.
 */
static Datum
PLyObject_ToTransform(PLyObToDatum *arg, PyObject *plrv,
					  bool *isnull, bool inarray)
{
	if (plrv == Py_None)
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;
	return FunctionCall1(&arg->u.transform.typtransform, PointerGetDatum(plrv));
}


/*
 * Convert Python sequence (or list of lists) to SQL array.
 */
static Datum
PLySequence_ToArray(PLyObToDatum *arg, PyObject *plrv,
					bool *isnull, bool inarray)
{
	ArrayBuildState *astate = NULL;
	int			ndims = 1;
	int			dims[MAXDIM];
	int			lbs[MAXDIM];

	if (plrv == Py_None)
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;

	/*
	 * For historical reasons, we allow any sequence (not only a list) at the
	 * top level when converting a Python object to a SQL array.  However, a
	 * multi-dimensional array is recognized only when the object contains
	 * true lists.
	 */
	if (!PySequence_Check(plrv))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("return value of function with array return type is not a Python sequence")));

	/* Initialize dimensionality info with first-level dimension */
	memset(dims, 0, sizeof(dims));
	dims[0] = PySequence_Length(plrv);

	/*
	 * Traverse the Python lists, in depth-first order, and collect all the
	 * elements at the bottom level into an ArrayBuildState.
	 */
	PLySequence_ToArray_recurse(plrv, &astate,
								&ndims, dims, 1,
								arg->u.array.elm,
								arg->u.array.elmbasetype);

	/* ensure we get zero-D array for no inputs, as per PG convention */
	if (astate == NULL)
		return PointerGetDatum(construct_empty_array(arg->u.array.elmbasetype));

	for (int i = 0; i < ndims; i++)
		lbs[i] = 1;

	return makeMdArrayResult(astate, ndims, dims, lbs,
							 CurrentMemoryContext, true);
}

/*
 * Helper function for PLySequence_ToArray. Traverse a Python list of lists in
 * depth-first order, storing the elements in *astatep.
 *
 * The ArrayBuildState is created only when we first find a scalar element;
 * if we didn't do it like that, we'd need some other convention for knowing
 * whether we'd already found any scalars (and thus the number of dimensions
 * is frozen).
 */
static void
PLySequence_ToArray_recurse(PyObject *obj, ArrayBuildState **astatep,
							int *ndims, int *dims, int cur_depth,
							PLyObToDatum *elm, Oid elmbasetype)
{
	int			i;
	int			len = PySequence_Length(obj);

	/* We should not get here with a non-sequence object */
	if (len < 0)
		PLy_elog(ERROR, "could not determine sequence length for function return value");

	for (i = 0; i < len; i++)
	{
		/* fetch the array element */
		PyObject   *subobj = PySequence_GetItem(obj, i);

		/* need PG_TRY to ensure we release the subobj's refcount */
		PG_TRY();
		{
			/* multi-dimensional array? */
			if (PyList_Check(subobj))
			{
				/* set size when at first element in this level, else compare */
				if (i == 0 && *ndims == cur_depth)
				{
					/* array after some scalars at same level? */
					if (*astatep != NULL)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("multidimensional arrays must have array expressions with matching dimensions")));
					/* too many dimensions? */
					if (cur_depth >= MAXDIM)
						ereport(ERROR,
								(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								 errmsg("number of array dimensions exceeds the maximum allowed (%d)",
										MAXDIM)));
					/* OK, add a dimension */
					dims[*ndims] = PySequence_Length(subobj);
					(*ndims)++;
				}
				else if (cur_depth >= *ndims ||
						 PySequence_Length(subobj) != dims[cur_depth])
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("multidimensional arrays must have array expressions with matching dimensions")));

				/* recurse to fetch elements of this sub-array */
				PLySequence_ToArray_recurse(subobj, astatep,
											ndims, dims, cur_depth + 1,
											elm, elmbasetype);
			}
			else
			{
				Datum		dat;
				bool		isnull;

				/* scalar after some sub-arrays at same level? */
				if (*ndims != cur_depth)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("multidimensional arrays must have array expressions with matching dimensions")));

				/* convert non-list object to Datum */
				dat = elm->func(elm, subobj, &isnull, true);

				/* create ArrayBuildState if we didn't already */
				if (*astatep == NULL)
					*astatep = initArrayResult(elmbasetype,
											   CurrentMemoryContext, true);

				/* ... and save the element value in it */
				(void) accumArrayResult(*astatep, dat, isnull,
										elmbasetype, CurrentMemoryContext);
			}
		}
		PG_FINALLY();
		{
			Py_XDECREF(subobj);
		}
		PG_END_TRY();
	}
}


/*
 * Convert a Python string to composite, using record_in.
 */
static Datum
PLyUnicode_ToComposite(PLyObToDatum *arg, PyObject *string, bool inarray)
{
	char	   *str;

	/*
	 * Set up call data for record_in, if we didn't already.  (We can't just
	 * use DirectFunctionCall, because record_in needs a fn_extra field.)
	 */
	if (!OidIsValid(arg->u.tuple.recinfunc.fn_oid))
		fmgr_info_cxt(F_RECORD_IN, &arg->u.tuple.recinfunc, arg->mcxt);

	str = PLyObject_AsString(string);

	/*
	 * If we are parsing a composite type within an array, and the string
	 * isn't a valid record literal, there's a high chance that the function
	 * did something like:
	 *
	 * CREATE FUNCTION .. RETURNS comptype[] AS $$ return [['foo', 'bar']] $$
	 * LANGUAGE plpython;
	 *
	 * Before PostgreSQL 10, that was interpreted as a single-dimensional
	 * array, containing record ('foo', 'bar'). PostgreSQL 10 added support
	 * for multi-dimensional arrays, and it is now interpreted as a
	 * two-dimensional array, containing two records, 'foo', and 'bar'.
	 * record_in() will throw an error, because "foo" is not a valid record
	 * literal.
	 *
	 * To make that less confusing to users who are upgrading from older
	 * versions, try to give a hint in the typical instances of that. If we
	 * are parsing an array of composite types, and we see a string literal
	 * that is not a valid record literal, give a hint. We only want to give
	 * the hint in the narrow case of a malformed string literal, not any
	 * error from record_in(), so check for that case here specifically.
	 *
	 * This check better match the one in record_in(), so that we don't forbid
	 * literals that are actually valid!
	 */
	if (inarray)
	{
		char	   *ptr = str;

		/* Allow leading whitespace */
		while (*ptr && isspace((unsigned char) *ptr))
			ptr++;
		if (*ptr++ != '(')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed record literal: \"%s\"", str),
					 errdetail("Missing left parenthesis."),
					 errhint("To return a composite type in an array, return the composite type as a Python tuple, e.g., \"[('foo',)]\".")));
	}

	return InputFunctionCall(&arg->u.tuple.recinfunc,
							 str,
							 arg->typoid,
							 arg->typmod);
}


static Datum
PLyMapping_ToComposite(PLyObToDatum *arg, TupleDesc desc, PyObject *mapping)
{
	Datum		result;
	HeapTuple	tuple;
	Datum	   *values;
	bool	   *nulls;
	volatile int i;

	Assert(PyMapping_Check(mapping));

	/* Build tuple */
	values = palloc(sizeof(Datum) * desc->natts);
	nulls = palloc(sizeof(bool) * desc->natts);
	for (i = 0; i < desc->natts; ++i)
	{
		char	   *key;
		PyObject   *volatile value;
		PLyObToDatum *att;
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (attr->attisdropped)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
			continue;
		}

		key = NameStr(attr->attname);
		value = NULL;
		att = &arg->u.tuple.atts[i];
		PG_TRY();
		{
			value = PyMapping_GetItemString(mapping, key);
			if (!value)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("key \"%s\" not found in mapping", key),
						 errhint("To return null in a column, "
								 "add the value None to the mapping with the key named after the column.")));

			values[i] = att->func(att, value, &nulls[i], false);

			Py_XDECREF(value);
			value = NULL;
		}
		PG_CATCH();
		{
			Py_XDECREF(value);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	tuple = heap_form_tuple(desc, values, nulls);
	result = heap_copy_tuple_as_datum(tuple, desc);
	heap_freetuple(tuple);

	pfree(values);
	pfree(nulls);

	return result;
}


static Datum
PLySequence_ToComposite(PLyObToDatum *arg, TupleDesc desc, PyObject *sequence)
{
	Datum		result;
	HeapTuple	tuple;
	Datum	   *values;
	bool	   *nulls;
	volatile int idx;
	volatile int i;

	Assert(PySequence_Check(sequence));

	/*
	 * Check that sequence length is exactly same as PG tuple's. We actually
	 * can ignore exceeding items or assume missing ones as null but to avoid
	 * plpython developer's errors we are strict here
	 */
	idx = 0;
	for (i = 0; i < desc->natts; i++)
	{
		if (!TupleDescAttr(desc, i)->attisdropped)
			idx++;
	}
	if (PySequence_Length(sequence) != idx)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("length of returned sequence did not match number of columns in row")));

	/* Build tuple */
	values = palloc(sizeof(Datum) * desc->natts);
	nulls = palloc(sizeof(bool) * desc->natts);
	idx = 0;
	for (i = 0; i < desc->natts; ++i)
	{
		PyObject   *volatile value;
		PLyObToDatum *att;

		if (TupleDescAttr(desc, i)->attisdropped)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
			continue;
		}

		value = NULL;
		att = &arg->u.tuple.atts[i];
		PG_TRY();
		{
			value = PySequence_GetItem(sequence, idx);
			Assert(value);

			values[i] = att->func(att, value, &nulls[i], false);

			Py_XDECREF(value);
			value = NULL;
		}
		PG_CATCH();
		{
			Py_XDECREF(value);
			PG_RE_THROW();
		}
		PG_END_TRY();

		idx++;
	}

	tuple = heap_form_tuple(desc, values, nulls);
	result = heap_copy_tuple_as_datum(tuple, desc);
	heap_freetuple(tuple);

	pfree(values);
	pfree(nulls);

	return result;
}


static Datum
PLyGenericObject_ToComposite(PLyObToDatum *arg, TupleDesc desc, PyObject *object, bool inarray)
{
	Datum		result;
	HeapTuple	tuple;
	Datum	   *values;
	bool	   *nulls;
	volatile int i;

	/* Build tuple */
	values = palloc(sizeof(Datum) * desc->natts);
	nulls = palloc(sizeof(bool) * desc->natts);
	for (i = 0; i < desc->natts; ++i)
	{
		char	   *key;
		PyObject   *volatile value;
		PLyObToDatum *att;
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (attr->attisdropped)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
			continue;
		}

		key = NameStr(attr->attname);
		value = NULL;
		att = &arg->u.tuple.atts[i];
		PG_TRY();
		{
			value = PyObject_GetAttrString(object, key);
			if (!value)
			{
				/*
				 * No attribute for this column in the object.
				 *
				 * If we are parsing a composite type in an array, a likely
				 * cause is that the function contained something like "[[123,
				 * 'foo']]". Before PostgreSQL 10, that was interpreted as an
				 * array, with a composite type (123, 'foo') in it. But now
				 * it's interpreted as a two-dimensional array, and we try to
				 * interpret "123" as the composite type. See also similar
				 * heuristic in PLyObject_ToScalar().
				 */
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("attribute \"%s\" does not exist in Python object", key),
						 inarray ?
						 errhint("To return a composite type in an array, return the composite type as a Python tuple, e.g., \"[('foo',)]\".") :
						 errhint("To return null in a column, let the returned object have an attribute named after column with value None.")));
			}

			values[i] = att->func(att, value, &nulls[i], false);

			Py_XDECREF(value);
			value = NULL;
		}
		PG_CATCH();
		{
			Py_XDECREF(value);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	tuple = heap_form_tuple(desc, values, nulls);
	result = heap_copy_tuple_as_datum(tuple, desc);
	heap_freetuple(tuple);

	pfree(values);
	pfree(nulls);

	return result;
}
