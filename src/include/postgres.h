/*-------------------------------------------------------------------------
 *
 * postgres.h
 *	  Primary include file for PostgreSQL server .c files
 *
 * This should be the first file included by PostgreSQL backend modules.
 * Client-side code should include postgres_fe.h instead.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/include/postgres.h
 *
 *-------------------------------------------------------------------------
 */
/*
 *----------------------------------------------------------------
 *	 TABLE OF CONTENTS
 *
 *		When adding stuff to this file, please try to put stuff
 *		into the relevant section, or add new sections as appropriate.
 *
 *	  section	description
 *	  -------	------------------------------------------------
 *		1)		Datum type + support functions
 *		2)		miscellaneous
 *
 *	 NOTES
 *
 *	In general, this file should contain declarations that are widely needed
 *	in the backend environment, but are of no interest outside the backend.
 *
 *	Simple type definitions live in c.h, where they are shared with
 *	postgres_fe.h.  We do that since those type definitions are needed by
 *	frontend modules that want to deal with binary data transmission to or
 *	from the backend.  Type definitions in this file should be for
 *	representations that never escape the backend, such as Datum.
 *
 *----------------------------------------------------------------
 */
#ifndef POSTGRES_H
#define POSTGRES_H

#include "c.h"
#include "utils/elog.h"
#include "utils/palloc.h"

/* ----------------------------------------------------------------
 *				Section 1:	Datum type + support functions
 * ----------------------------------------------------------------
 */

/*
 * A Datum contains either a value of a pass-by-value type or a pointer to a
 * value of a pass-by-reference type.  Therefore, we require:
 *
 * sizeof(Datum) == sizeof(void *) == 4 or 8
 *
 * The functions below and the analogous functions for other types should be used to
 * convert between a Datum and the appropriate C type.
 */

typedef uintptr_t Datum;

/*
 * A NullableDatum is used in places where both a Datum and its nullness needs
 * to be stored. This can be more efficient than storing datums and nullness
 * in separate arrays, due to better spatial locality, even if more space may
 * be wasted due to padding.
 */
typedef struct NullableDatum
{
#define FIELDNO_NULLABLE_DATUM_DATUM 0
	Datum		value;
#define FIELDNO_NULLABLE_DATUM_ISNULL 1
	bool		isnull;
	/* due to alignment padding this could be used for flags for free */
} NullableDatum;

#define SIZEOF_DATUM SIZEOF_VOID_P

/*
 * DatumGetBool
 *		Returns boolean value of a datum.
 *
 * Note: any nonzero value will be considered true.
 */
static inline bool
DatumGetBool(Datum X)
{
	return (X != 0);
}

/*
 * BoolGetDatum
 *		Returns datum representation for a boolean.
 *
 * Note: any nonzero value will be considered true.
 */
static inline Datum
BoolGetDatum(bool X)
{
	return (Datum) (X ? 1 : 0);
}

/*
 * DatumGetChar
 *		Returns character value of a datum.
 */
static inline char
DatumGetChar(Datum X)
{
	return (char) X;
}

/*
 * CharGetDatum
 *		Returns datum representation for a character.
 */
static inline Datum
CharGetDatum(char X)
{
	return (Datum) X;
}

/*
 * Int8GetDatum
 *		Returns datum representation for an 8-bit integer.
 */
static inline Datum
Int8GetDatum(int8 X)
{
	return (Datum) X;
}

/*
 * DatumGetUInt8
 *		Returns 8-bit unsigned integer value of a datum.
 */
static inline uint8
DatumGetUInt8(Datum X)
{
	return (uint8) X;
}

/*
 * UInt8GetDatum
 *		Returns datum representation for an 8-bit unsigned integer.
 */
static inline Datum
UInt8GetDatum(uint8 X)
{
	return (Datum) X;
}

/*
 * DatumGetInt16
 *		Returns 16-bit integer value of a datum.
 */
static inline int16
DatumGetInt16(Datum X)
{
	return (int16) X;
}

/*
 * Int16GetDatum
 *		Returns datum representation for a 16-bit integer.
 */
static inline Datum
Int16GetDatum(int16 X)
{
	return (Datum) X;
}

/*
 * DatumGetUInt16
 *		Returns 16-bit unsigned integer value of a datum.
 */
static inline uint16
DatumGetUInt16(Datum X)
{
	return (uint16) X;
}

/*
 * UInt16GetDatum
 *		Returns datum representation for a 16-bit unsigned integer.
 */
static inline Datum
UInt16GetDatum(uint16 X)
{
	return (Datum) X;
}

/*
 * DatumGetInt32
 *		Returns 32-bit integer value of a datum.
 */
static inline int32
DatumGetInt32(Datum X)
{
	return (int32) X;
}

/*
 * Int32GetDatum
 *		Returns datum representation for a 32-bit integer.
 */
static inline Datum
Int32GetDatum(int32 X)
{
	return (Datum) X;
}

/*
 * DatumGetUInt32
 *		Returns 32-bit unsigned integer value of a datum.
 */
static inline uint32
DatumGetUInt32(Datum X)
{
	return (uint32) X;
}

/*
 * UInt32GetDatum
 *		Returns datum representation for a 32-bit unsigned integer.
 */
static inline Datum
UInt32GetDatum(uint32 X)
{
	return (Datum) X;
}

/*
 * DatumGetObjectId
 *		Returns object identifier value of a datum.
 */
static inline Oid
DatumGetObjectId(Datum X)
{
	return (Oid) X;
}

/*
 * ObjectIdGetDatum
 *		Returns datum representation for an object identifier.
 */
static inline Datum
ObjectIdGetDatum(Oid X)
{
	return (Datum) X;
}

/*
 * DatumGetTransactionId
 *		Returns transaction identifier value of a datum.
 */
static inline TransactionId
DatumGetTransactionId(Datum X)
{
	return (TransactionId) X;
}

/*
 * TransactionIdGetDatum
 *		Returns datum representation for a transaction identifier.
 */
static inline Datum
TransactionIdGetDatum(TransactionId X)
{
	return (Datum) X;
}

/*
 * MultiXactIdGetDatum
 *		Returns datum representation for a multixact identifier.
 */
static inline Datum
MultiXactIdGetDatum(MultiXactId X)
{
	return (Datum) X;
}

/*
 * DatumGetCommandId
 *		Returns command identifier value of a datum.
 */
static inline CommandId
DatumGetCommandId(Datum X)
{
	return (CommandId) X;
}

/*
 * CommandIdGetDatum
 *		Returns datum representation for a command identifier.
 */
static inline Datum
CommandIdGetDatum(CommandId X)
{
	return (Datum) X;
}

/*
 * DatumGetPointer
 *		Returns pointer value of a datum.
 */
static inline Pointer
DatumGetPointer(Datum X)
{
	return (Pointer) X;
}

/*
 * PointerGetDatum
 *		Returns datum representation for a pointer.
 */
static inline Datum
PointerGetDatum(const void *X)
{
	return (Datum) X;
}

/*
 * DatumGetCString
 *		Returns C string (null-terminated string) value of a datum.
 *
 * Note: C string is not a full-fledged Postgres type at present,
 * but type input functions use this conversion for their inputs.
 */
static inline char *
DatumGetCString(Datum X)
{
	return (char *) DatumGetPointer(X);
}

/*
 * CStringGetDatum
 *		Returns datum representation for a C string (null-terminated string).
 *
 * Note: C string is not a full-fledged Postgres type at present,
 * but type output functions use this conversion for their outputs.
 * Note: CString is pass-by-reference; caller must ensure the pointed-to
 * value has adequate lifetime.
 */
static inline Datum
CStringGetDatum(const char *X)
{
	return PointerGetDatum(X);
}

/*
 * DatumGetName
 *		Returns name value of a datum.
 */
static inline Name
DatumGetName(Datum X)
{
	return (Name) DatumGetPointer(X);
}

/*
 * NameGetDatum
 *		Returns datum representation for a name.
 *
 * Note: Name is pass-by-reference; caller must ensure the pointed-to
 * value has adequate lifetime.
 */
static inline Datum
NameGetDatum(const NameData *X)
{
	return CStringGetDatum(NameStr(*X));
}

/*
 * DatumGetInt64
 *		Returns 64-bit integer value of a datum.
 *
 * Note: this function hides whether int64 is pass by value or by reference.
 */
static inline int64
DatumGetInt64(Datum X)
{
#ifdef USE_FLOAT8_BYVAL
	return (int64) X;
#else
	return *((int64 *) DatumGetPointer(X));
#endif
}

/*
 * Int64GetDatum
 *		Returns datum representation for a 64-bit integer.
 *
 * Note: if int64 is pass by reference, this function returns a reference
 * to palloc'd space.
 */
#ifdef USE_FLOAT8_BYVAL
static inline Datum
Int64GetDatum(int64 X)
{
	return (Datum) X;
}
#else
extern Datum Int64GetDatum(int64 X);
#endif


/*
 * DatumGetUInt64
 *		Returns 64-bit unsigned integer value of a datum.
 *
 * Note: this function hides whether int64 is pass by value or by reference.
 */
static inline uint64
DatumGetUInt64(Datum X)
{
#ifdef USE_FLOAT8_BYVAL
	return (uint64) X;
#else
	return *((uint64 *) DatumGetPointer(X));
#endif
}

/*
 * UInt64GetDatum
 *		Returns datum representation for a 64-bit unsigned integer.
 *
 * Note: if int64 is pass by reference, this function returns a reference
 * to palloc'd space.
 */
static inline Datum
UInt64GetDatum(uint64 X)
{
#ifdef USE_FLOAT8_BYVAL
	return (Datum) X;
#else
	return Int64GetDatum((int64) X);
#endif
}

/*
 * Float <-> Datum conversions
 *
 * These have to be implemented as inline functions rather than macros, when
 * passing by value, because many machines pass int and float function
 * parameters/results differently; so we need to play weird games with unions.
 */

/*
 * DatumGetFloat4
 *		Returns 4-byte floating point value of a datum.
 */
static inline float4
DatumGetFloat4(Datum X)
{
	union
	{
		int32		value;
		float4		retval;
	}			myunion;

	myunion.value = DatumGetInt32(X);
	return myunion.retval;
}

/*
 * Float4GetDatum
 *		Returns datum representation for a 4-byte floating point number.
 */
static inline Datum
Float4GetDatum(float4 X)
{
	union
	{
		float4		value;
		int32		retval;
	}			myunion;

	myunion.value = X;
	return Int32GetDatum(myunion.retval);
}

/*
 * DatumGetFloat8
 *		Returns 8-byte floating point value of a datum.
 *
 * Note: this function hides whether float8 is pass by value or by reference.
 */
static inline float8
DatumGetFloat8(Datum X)
{
#ifdef USE_FLOAT8_BYVAL
	union
	{
		int64		value;
		float8		retval;
	}			myunion;

	myunion.value = DatumGetInt64(X);
	return myunion.retval;
#else
	return *((float8 *) DatumGetPointer(X));
#endif
}

/*
 * Float8GetDatum
 *		Returns datum representation for an 8-byte floating point number.
 *
 * Note: if float8 is pass by reference, this function returns a reference
 * to palloc'd space.
 */
#ifdef USE_FLOAT8_BYVAL
static inline Datum
Float8GetDatum(float8 X)
{
	union
	{
		float8		value;
		int64		retval;
	}			myunion;

	myunion.value = X;
	return Int64GetDatum(myunion.retval);
}
#else
extern Datum Float8GetDatum(float8 X);
#endif


/*
 * Int64GetDatumFast
 * Float8GetDatumFast
 *
 * These macros are intended to allow writing code that does not depend on
 * whether int64 and float8 are pass-by-reference types, while not
 * sacrificing performance when they are.  The argument must be a variable
 * that will exist and have the same value for as long as the Datum is needed.
 * In the pass-by-ref case, the address of the variable is taken to use as
 * the Datum.  In the pass-by-val case, these are the same as the non-Fast
 * functions, except for asserting that the variable is of the correct type.
 */

#ifdef USE_FLOAT8_BYVAL
#define Int64GetDatumFast(X) \
	(AssertVariableIsOfTypeMacro(X, int64), Int64GetDatum(X))
#define Float8GetDatumFast(X) \
	(AssertVariableIsOfTypeMacro(X, double), Float8GetDatum(X))
#else
#define Int64GetDatumFast(X) \
	(AssertVariableIsOfTypeMacro(X, int64), PointerGetDatum(&(X)))
#define Float8GetDatumFast(X) \
	(AssertVariableIsOfTypeMacro(X, double), PointerGetDatum(&(X)))
#endif


/* ----------------------------------------------------------------
 *				Section 2:	miscellaneous
 * ----------------------------------------------------------------
 */

/*
 * NON_EXEC_STATIC: It's sometimes useful to define a variable or function
 * that is normally static but extern when using EXEC_BACKEND (see
 * pg_config_manual.h).  There would then typically be some code in
 * postmaster.c that uses those extern symbols to transfer state between
 * processes or do whatever other things it needs to do in EXEC_BACKEND mode.
 */
#ifdef EXEC_BACKEND
#define NON_EXEC_STATIC
#else
#define NON_EXEC_STATIC static
#endif

#endif							/* POSTGRES_H */
