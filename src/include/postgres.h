/*-------------------------------------------------------------------------
 *
 * postgres.h
 *	  Primary include file for PostgreSQL server .c files
 *
 * This should be the first file included by PostgreSQL backend modules.
 * Client-side code should include postgres_fe.h instead.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * $Id: postgres.h,v 1.65.4.1 2008/03/25 19:31:53 tgl Exp $
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
 *		1)		variable-length datatypes (TOAST support)
 *		2)		datum type + support macros
 *		3)		exception handling definitions
 *		4)		genbki macros used by catalog/pg_xxx.h files
 *
 *	 NOTES
 *
 *	In general, this file should contain declarations that are widely needed
 *	in the backend environment, but are of no interest outside the backend.
 *
 *	Simple type definitions live in c.h, where they are shared with
 *	postgres_fe.h.	We do that since those type definitions are needed by
 *	frontend modules that want to deal with binary data transmission to or
 *	from the backend.  Type definitions in this file should be for
 *	representations that never escape the backend, such as Datum or
 *	TOASTed varlena objects.
 *
 *----------------------------------------------------------------
 */
#ifndef POSTGRES_H
#define POSTGRES_H

#include "c.h"
#include "utils/elog.h"
#include "utils/palloc.h"

/* ----------------------------------------------------------------
 *				Section 1:	variable-length datatypes (TOAST support)
 * ----------------------------------------------------------------
 */

/* ----------------
 * struct varattrib is the header of a varlena object that may have been
 * TOASTed.
 * ----------------
 */
#define TUPLE_TOASTER_ACTIVE

typedef struct varattrib
{
	int32		va_header;		/* External/compressed storage */
	/* flags and item size */
	union
	{
		struct
		{
			int32		va_rawsize;		/* Plain data size */
			char		va_data[1];		/* Compressed data */
		}			va_compressed;		/* Compressed stored attribute */

		struct
		{
			int32		va_rawsize;		/* Plain data size */
			int32		va_extsize;		/* External saved size */
			Oid			va_valueid;		/* Unique identifier of value */
			Oid			va_toastrelid;	/* RelID where to find chunks */
		}			va_external;	/* External stored attribute */

		char		va_data[1]; /* Plain stored attribute */
	}			va_content;
} varattrib;

#define VARATT_FLAG_EXTERNAL	0x80000000
#define VARATT_FLAG_COMPRESSED	0x40000000
#define VARATT_MASK_FLAGS		0xc0000000
#define VARATT_MASK_SIZE		0x3fffffff

#define VARATT_SIZEP(_PTR)	(((varattrib *)(_PTR))->va_header)
#define VARATT_SIZE(PTR)	(VARATT_SIZEP(PTR) & VARATT_MASK_SIZE)
#define VARATT_DATA(PTR)	(((varattrib *)(PTR))->va_content.va_data)
#define VARATT_CDATA(PTR)	(((varattrib *)(PTR))->va_content.va_compressed.va_data)

#define VARSIZE(__PTR)		VARATT_SIZE(__PTR)
#define VARDATA(__PTR)		VARATT_DATA(__PTR)

#define VARATT_IS_EXTENDED(PTR)		\
				((VARATT_SIZEP(PTR) & VARATT_MASK_FLAGS) != 0)
#define VARATT_IS_EXTERNAL(PTR)		\
				((VARATT_SIZEP(PTR) & VARATT_FLAG_EXTERNAL) != 0)
#define VARATT_IS_COMPRESSED(PTR)	\
				((VARATT_SIZEP(PTR) & VARATT_FLAG_COMPRESSED) != 0)


/* ----------------------------------------------------------------
 *				Section 2:	datum type + support macros
 * ----------------------------------------------------------------
 */

/*
 * Port Notes:
 *	Postgres makes the following assumption about machines:
 *
 *	sizeof(Datum) == sizeof(long) >= sizeof(void *) >= 4
 *
 *	Postgres also assumes that
 *
 *	sizeof(char) == 1
 *
 *	and that
 *
 *	sizeof(short) == 2
 *
 * When a type narrower than Datum is stored in a Datum, we place it in the
 * low-order bits and are careful that the DatumGetXXX macro for it discards
 * the unused high-order bits (as opposed to, say, assuming they are zero).
 * This is needed to support old-style user-defined functions, since depending
 * on architecture and compiler, the return value of a function returning char
 * or short may contain garbage when called as if it returned Datum.
 */

typedef unsigned long Datum;	/* XXX sizeof(long) >= sizeof(void *) */

#define SIZEOF_DATUM SIZEOF_UNSIGNED_LONG

typedef Datum *DatumPtr;

#define GET_1_BYTE(datum)	(((Datum) (datum)) & 0x000000ff)
#define GET_2_BYTES(datum)	(((Datum) (datum)) & 0x0000ffff)
#define GET_4_BYTES(datum)	(((Datum) (datum)) & 0xffffffff)
#define SET_1_BYTE(value)	(((Datum) (value)) & 0x000000ff)
#define SET_2_BYTES(value)	(((Datum) (value)) & 0x0000ffff)
#define SET_4_BYTES(value)	(((Datum) (value)) & 0xffffffff)

/*
 * DatumGetBool
 *		Returns boolean value of a datum.
 *
 * Note: any nonzero value will be considered TRUE, but we ignore bits to
 * the left of the width of bool, per comment above.
 */

#define DatumGetBool(X) ((bool) (((bool) (X)) != 0))

/*
 * BoolGetDatum
 *		Returns datum representation for a boolean.
 *
 * Note: any nonzero value will be considered TRUE.
 */

#define BoolGetDatum(X) ((Datum) ((X) ? 1 : 0))

/*
 * DatumGetChar
 *		Returns character value of a datum.
 */

#define DatumGetChar(X) ((char) GET_1_BYTE(X))

/*
 * CharGetDatum
 *		Returns datum representation for a character.
 */

#define CharGetDatum(X) ((Datum) SET_1_BYTE(X))

/*
 * Int8GetDatum
 *		Returns datum representation for an 8-bit integer.
 */

#define Int8GetDatum(X) ((Datum) SET_1_BYTE(X))

/*
 * DatumGetUInt8
 *		Returns 8-bit unsigned integer value of a datum.
 */

#define DatumGetUInt8(X) ((uint8) GET_1_BYTE(X))

/*
 * UInt8GetDatum
 *		Returns datum representation for an 8-bit unsigned integer.
 */

#define UInt8GetDatum(X) ((Datum) SET_1_BYTE(X))

/*
 * DatumGetInt16
 *		Returns 16-bit integer value of a datum.
 */

#define DatumGetInt16(X) ((int16) GET_2_BYTES(X))

/*
 * Int16GetDatum
 *		Returns datum representation for a 16-bit integer.
 */

#define Int16GetDatum(X) ((Datum) SET_2_BYTES(X))

/*
 * DatumGetUInt16
 *		Returns 16-bit unsigned integer value of a datum.
 */

#define DatumGetUInt16(X) ((uint16) GET_2_BYTES(X))

/*
 * UInt16GetDatum
 *		Returns datum representation for a 16-bit unsigned integer.
 */

#define UInt16GetDatum(X) ((Datum) SET_2_BYTES(X))

/*
 * DatumGetInt32
 *		Returns 32-bit integer value of a datum.
 */

#define DatumGetInt32(X) ((int32) GET_4_BYTES(X))

/*
 * Int32GetDatum
 *		Returns datum representation for a 32-bit integer.
 */

#define Int32GetDatum(X) ((Datum) SET_4_BYTES(X))

/*
 * DatumGetUInt32
 *		Returns 32-bit unsigned integer value of a datum.
 */

#define DatumGetUInt32(X) ((uint32) GET_4_BYTES(X))

/*
 * UInt32GetDatum
 *		Returns datum representation for a 32-bit unsigned integer.
 */

#define UInt32GetDatum(X) ((Datum) SET_4_BYTES(X))

/*
 * DatumGetObjectId
 *		Returns object identifier value of a datum.
 */

#define DatumGetObjectId(X) ((Oid) GET_4_BYTES(X))

/*
 * ObjectIdGetDatum
 *		Returns datum representation for an object identifier.
 */

#define ObjectIdGetDatum(X) ((Datum) SET_4_BYTES(X))

/*
 * DatumGetTransactionId
 *		Returns transaction identifier value of a datum.
 */

#define DatumGetTransactionId(X) ((TransactionId) GET_4_BYTES(X))

/*
 * TransactionIdGetDatum
 *		Returns datum representation for a transaction identifier.
 */

#define TransactionIdGetDatum(X) ((Datum) SET_4_BYTES((X)))

/*
 * DatumGetCommandId
 *		Returns command identifier value of a datum.
 */

#define DatumGetCommandId(X) ((CommandId) GET_4_BYTES(X))

/*
 * CommandIdGetDatum
 *		Returns datum representation for a command identifier.
 */

#define CommandIdGetDatum(X) ((Datum) SET_4_BYTES(X))

/*
 * DatumGetPointer
 *		Returns pointer value of a datum.
 */

#define DatumGetPointer(X) ((Pointer) (X))

/*
 * PointerGetDatum
 *		Returns datum representation for a pointer.
 */

#define PointerGetDatum(X) ((Datum) (X))

/*
 * DatumGetCString
 *		Returns C string (null-terminated string) value of a datum.
 *
 * Note: C string is not a full-fledged Postgres type at present,
 * but type input functions use this conversion for their inputs.
 */

#define DatumGetCString(X) ((char *) DatumGetPointer(X))

/*
 * CStringGetDatum
 *		Returns datum representation for a C string (null-terminated string).
 *
 * Note: C string is not a full-fledged Postgres type at present,
 * but type output functions use this conversion for their outputs.
 * Note: CString is pass-by-reference; caller must ensure the pointed-to
 * value has adequate lifetime.
 */

#define CStringGetDatum(X) PointerGetDatum(X)

/*
 * DatumGetName
 *		Returns name value of a datum.
 */

#define DatumGetName(X) ((Name) DatumGetPointer(X))

/*
 * NameGetDatum
 *		Returns datum representation for a name.
 *
 * Note: Name is pass-by-reference; caller must ensure the pointed-to
 * value has adequate lifetime.
 */

#define NameGetDatum(X) PointerGetDatum(X)

/*
 * DatumGetInt64
 *		Returns 64-bit integer value of a datum.
 *
 * Note: this macro hides the fact that int64 is currently a
 * pass-by-reference type.	Someday it may be pass-by-value,
 * at least on some platforms.
 */

#define DatumGetInt64(X) (* ((int64 *) DatumGetPointer(X)))

/*
 * Int64GetDatum
 *		Returns datum representation for a 64-bit integer.
 *
 * Note: this routine returns a reference to palloc'd space.
 */

extern Datum Int64GetDatum(int64 X);

/*
 * DatumGetFloat4
 *		Returns 4-byte floating point value of a datum.
 *
 * Note: this macro hides the fact that float4 is currently a
 * pass-by-reference type.	Someday it may be pass-by-value.
 */

#define DatumGetFloat4(X) (* ((float4 *) DatumGetPointer(X)))

/*
 * Float4GetDatum
 *		Returns datum representation for a 4-byte floating point number.
 *
 * Note: this routine returns a reference to palloc'd space.
 */

extern Datum Float4GetDatum(float4 X);

/*
 * DatumGetFloat8
 *		Returns 8-byte floating point value of a datum.
 *
 * Note: this macro hides the fact that float8 is currently a
 * pass-by-reference type.	Someday it may be pass-by-value,
 * at least on some platforms.
 */

#define DatumGetFloat8(X) (* ((float8 *) DatumGetPointer(X)))

/*
 * Float8GetDatum
 *		Returns datum representation for an 8-byte floating point number.
 *
 * Note: this routine returns a reference to palloc'd space.
 */

extern Datum Float8GetDatum(float8 X);


/*
 * DatumGetFloat32
 *		Returns 32-bit floating point value of a datum.
 *		This is really a pointer, of course.
 *
 * XXX: this macro is now deprecated in favor of DatumGetFloat4.
 * It will eventually go away.
 */

#define DatumGetFloat32(X) ((float32) DatumGetPointer(X))

/*
 * Float32GetDatum
 *		Returns datum representation for a 32-bit floating point number.
 *		This is really a pointer, of course.
 *
 * XXX: this macro is now deprecated in favor of Float4GetDatum.
 * It will eventually go away.
 */

#define Float32GetDatum(X) PointerGetDatum(X)

/*
 * DatumGetFloat64
 *		Returns 64-bit floating point value of a datum.
 *		This is really a pointer, of course.
 *
 * XXX: this macro is now deprecated in favor of DatumGetFloat8.
 * It will eventually go away.
 */

#define DatumGetFloat64(X) ((float64) DatumGetPointer(X))

/*
 * Float64GetDatum
 *		Returns datum representation for a 64-bit floating point number.
 *		This is really a pointer, of course.
 *
 * XXX: this macro is now deprecated in favor of Float8GetDatum.
 * It will eventually go away.
 */

#define Float64GetDatum(X) PointerGetDatum(X)

/*
 * Int64GetDatumFast
 * Float4GetDatumFast
 * Float8GetDatumFast
 *
 * These macros are intended to allow writing code that does not depend on
 * whether int64, float4, float8 are pass-by-reference types, while not
 * sacrificing performance when they are.  The argument must be a variable
 * that will exist and have the same value for as long as the Datum is needed.
 * In the pass-by-ref case, the address of the variable is taken to use as
 * the Datum.  In the pass-by-val case, these will be the same as the non-Fast
 * macros.
 */

#define Int64GetDatumFast(X)  PointerGetDatum(&(X))
#define Float4GetDatumFast(X) PointerGetDatum(&(X))
#define Float8GetDatumFast(X) PointerGetDatum(&(X))


/* ----------------------------------------------------------------
 *				Section 3:	exception handling definitions
 *							Assert, Trap, etc macros
 * ----------------------------------------------------------------
 */

extern DLLIMPORT bool assert_enabled;

/*
 * USE_ASSERT_CHECKING, if defined, turns on all the assertions.
 * - plai  9/5/90
 *
 * It should _NOT_ be defined in releases or in benchmark copies
 */

/*
 * Trap
 *		Generates an exception if the given condition is true.
 *
 */
#define Trap(condition, errorType) \
		do { \
			if ((assert_enabled) && (condition)) \
				ExceptionalCondition(CppAsString(condition), (errorType), \
									 __FILE__, __LINE__); \
		} while (0)

/*
 *	TrapMacro is the same as Trap but it's intended for use in macros:
 *
 *		#define foo(x) (AssertMacro(x != 0) && bar(x))
 *
 *	Isn't CPP fun?
 */
#define TrapMacro(condition, errorType) \
	((bool) ((! assert_enabled) || ! (condition) || \
			 (ExceptionalCondition(CppAsString(condition), (errorType), \
								   __FILE__, __LINE__))))

#ifndef USE_ASSERT_CHECKING
#define Assert(condition)
#define AssertMacro(condition)	((void)true)
#define AssertArg(condition)
#define AssertState(condition)
#define assert_enabled 0
#else
#define Assert(condition) \
		Trap(!(condition), "FailedAssertion")

#define AssertMacro(condition) \
		((void) TrapMacro(!(condition), "FailedAssertion"))

#define AssertArg(condition) \
		Trap(!(condition), "BadArgument")

#define AssertState(condition) \
		Trap(!(condition), "BadState")
#endif   /* USE_ASSERT_CHECKING */

extern int ExceptionalCondition(char *conditionName, char *errorType,
					 char *fileName, int lineNumber);

/* ----------------------------------------------------------------
 *				Section 4: genbki macros used by catalog/pg_xxx.h files
 * ----------------------------------------------------------------
 */
#define CATALOG(x)	typedef struct CppConcat(FormData_,x)

#define BOOTSTRAP
#define BKI_SHARED_RELATION
#define BKI_WITHOUT_OIDS

/* these need to expand into some harmless, repeatable declaration */
#define DATA(x)   extern int no_such_variable
#define DESCR(x)  extern int no_such_variable

#define BKI_BEGIN
#define BKI_END

typedef int4 aclitem;			/* PHONY definition for catalog use only */

#endif   /* POSTGRES_H */
