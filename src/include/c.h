/*-------------------------------------------------------------------------
 *
 * c.h
 *	  Fundamental C definitions.  This is included by every .c file in
 *	  postgres.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: c.h,v 1.74 2000/06/08 22:37:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 TABLE OF CONTENTS
 *
 *		When adding stuff to this file, please try to put stuff
 *		into the relevant section, or add new sections as appropriate.
 *
 *	  section	description
 *	  -------	------------------------------------------------
 *		1)		bool, true, false, TRUE, FALSE, NULL
 *		2)		non-ansi C definitions:
 *				type prefixes: const, signed, volatile, inline
 *				cpp magic macros
 *		3)		standard system types
 *		4)		datum type
 *		5)		IsValid macros for system types
 *		6)		offsetof, lengthof, endof
 *		7)		exception handling definitions, Assert, Trap, etc macros
 *		8)		Min, Max, Abs, StrNCpy macros
 *		9)		externs
 *		10)		Berkeley-specific defs
 *		11)		system-specific hacks
 *
 * ----------------------------------------------------------------
 */
#ifndef C_H
#define C_H

/* We have to include stdlib.h here because it defines many of these macros
   on some platforms, and we only want our definitions used if stdlib.h doesn't
   have its own.  The same goes for stddef and stdarg if present.
*/

#include "config.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#ifdef STDC_HEADERS
#include <stddef.h>
#include <stdarg.h>
#endif

#ifdef __CYGWIN32__
#include <errno.h>
#endif

/* ----------------------------------------------------------------
 *				Section 1:	bool, true, false, TRUE, FALSE, NULL
 * ----------------------------------------------------------------
 */
/*
 * bool
 *		Boolean value, either true or false.
 *
 */
#ifndef __cplusplus
#ifndef bool
typedef char bool;

#endif	 /* ndef bool */
#endif	 /* not C++ */
#ifndef true
#define true	((bool) 1)
#endif
#ifndef false
#define false	((bool) 0)
#endif
typedef bool *BoolPtr;

#ifndef TRUE
#define TRUE	1
#endif	 /* TRUE */

#ifndef FALSE
#define FALSE	0
#endif	 /* FALSE */

/*
 * NULL
 *		Null pointer.
 */
#ifndef NULL
#define NULL	((void *) 0)
#endif	 /* !defined(NULL) */

/* ----------------------------------------------------------------
 *				Section 2: non-ansi C definitions:
 *
 *				type prefixes: const, signed, volatile, inline
 *				cpp magic macros
 * ----------------------------------------------------------------
 */

/*
 * CppAsString
 *		Convert the argument to a string, using the C preprocessor.
 * CppConcat
 *		Concatenate two arguments together, using the C preprocessor.
 *
 * Note: the standard Autoconf macro AC_C_STRINGIZE actually only checks
 * whether #identifier works, but if we have that we likely have ## too.
 */
#if defined(HAVE_STRINGIZE)

#define CppAsString(identifier) #identifier
#define CppConcat(x, y)			x##y

#else							/* !HAVE_STRINGIZE */

#define CppAsString(identifier) "identifier"

/*
 * CppIdentity -- On Reiser based cpp's this is used to concatenate
 *		two tokens.  That is
 *				CppIdentity(A)B ==> AB
 *		We renamed it to _private_CppIdentity because it should not
 *		be referenced outside this file.  On other cpp's it
 *		produces  A  B.
 */
#define _priv_CppIdentity(x)x
#define CppConcat(x, y)			_priv_CppIdentity(x)y

#endif	 /* !HAVE_STRINGIZE */

/*
 * dummyret is used to set return values in macros that use ?: to make
 * assignments.  gcc wants these to be void, other compilers like char
 */
#ifdef __GNUC__					/* GNU cc */
#define dummyret	void
#else
#define dummyret	char
#endif

/* ----------------------------------------------------------------
 *				Section 3:	standard system types
 * ----------------------------------------------------------------
 */

/*
 * Pointer
 *		Variable holding address of any memory resident object.
 *
 *		XXX Pointer arithmetic is done with this, so it can't be void *
 *		under "true" ANSI compilers.
 */
typedef char *Pointer;

/*
 * intN
 *		Signed integer, EXACTLY N BITS IN SIZE,
 *		used for numerical computations and the
 *		frontend/backend protocol.
 */
typedef signed char int8;		/* == 8 bits */
typedef signed short int16;		/* == 16 bits */
typedef signed int int32;		/* == 32 bits */

/*
 * uintN
 *		Unsigned integer, EXACTLY N BITS IN SIZE,
 *		used for numerical computations and the
 *		frontend/backend protocol.
 */
typedef unsigned char uint8;	/* == 8 bits */
typedef unsigned short uint16;	/* == 16 bits */
typedef unsigned int uint32;	/* == 32 bits */

/*
 * floatN
 *		Floating point number, AT LEAST N BITS IN SIZE,
 *		used for numerical computations.
 *
 *		Since sizeof(floatN) may be > sizeof(char *), always pass
 *		floatN by reference.
 */
typedef float float32data;
typedef double float64data;
typedef float *float32;
typedef double *float64;

/*
 * boolN
 *		Boolean value, AT LEAST N BITS IN SIZE.
 */
typedef uint8 bool8;			/* >= 8 bits */
typedef uint16 bool16;			/* >= 16 bits */
typedef uint32 bool32;			/* >= 32 bits */

/*
 * bitsN
 *		Unit of bitwise operation, AT LEAST N BITS IN SIZE.
 */
typedef uint8 bits8;			/* >= 8 bits */
typedef uint16 bits16;			/* >= 16 bits */
typedef uint32 bits32;			/* >= 32 bits */

/*
 * wordN
 *		Unit of storage, AT LEAST N BITS IN SIZE,
 *		used to fetch/store data.
 */
typedef uint8 word8;			/* >= 8 bits */
typedef uint16 word16;			/* >= 16 bits */
typedef uint32 word32;			/* >= 32 bits */

/*
 * Size
 *		Size of any memory resident object, as returned by sizeof.
 */
typedef size_t Size;

/*
 * Index
 *		Index into any memory resident array.
 *
 * Note:
 *		Indices are non negative.
 */
typedef unsigned int Index;

#define MAXDIM 6
typedef struct
{
	int			indx[MAXDIM];
} IntArray;

/*
 * Offset
 *		Offset into any memory resident array.
 *
 * Note:
 *		This differs from an Index in that an Index is always
 *		non negative, whereas Offset may be negative.
 */
typedef signed int Offset;

/*
 * Common Postgres datatypes.
 */
typedef int16 int2;
typedef int32 int4;
typedef float float4;
typedef double float8;

#ifdef HAVE_LONG_INT_64
/* Plain "long int" fits, use it */
typedef long int int64;
#else
#ifdef HAVE_LONG_LONG_INT_64
/* We have working support for "long long int", use that */
typedef long long int int64;
#else
/* Won't actually work, but fall back to long int so that code compiles */
typedef long int int64;
#define INT64_IS_BUSTED
#endif
#endif

/* ----------------------------------------------------------------
 *				Section 4:	datum type + support macros
 * ----------------------------------------------------------------
 */
/*
 * datum.h
 *		POSTGRES abstract data type datum representation definitions.
 *
 * Note:
 *
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
 *	If your machine meets these requirements, Datums should also be checked
 *	to see if the positioning is correct.
 */

typedef unsigned long Datum;	/* XXX sizeof(long) >= sizeof(void *) */
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
 * Note: any nonzero value will be considered TRUE.
 */

#define DatumGetBool(X) ((bool) (((Datum) (X)) != 0))

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
 * pass-by-reference type.  Someday it may be pass-by-value,
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
 * pass-by-reference type.  Someday it may be pass-by-value.
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
 * pass-by-reference type.  Someday it may be pass-by-value,
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

/* ----------------------------------------------------------------
 *				Section 5:	IsValid macros for system types
 * ----------------------------------------------------------------
 */
/*
 * BoolIsValid
 *		True iff bool is valid.
 */
#define BoolIsValid(boolean)	((boolean) == false || (boolean) == true)

/*
 * PointerIsValid
 *		True iff pointer is valid.
 */
#define PointerIsValid(pointer) ((void*)(pointer) != NULL)

/*
 * PointerIsAligned
 *		True iff pointer is properly aligned to point to the given type.
 */
#define PointerIsAligned(pointer, type) \
		(((long)(pointer) % (sizeof (type))) == 0)

/* ----------------------------------------------------------------
 *				Section 6:	offsetof, lengthof, endof
 * ----------------------------------------------------------------
 */
/*
 * offsetof
 *		Offset of a structure/union field within that structure/union.
 *
 *		XXX This is supposed to be part of stddef.h, but isn't on
 *		some systems (like SunOS 4).
 */
#ifndef offsetof
#define offsetof(type, field)	((long) &((type *)0)->field)
#endif	 /* offsetof */

/*
 * lengthof
 *		Number of elements in an array.
 */
#define lengthof(array) (sizeof (array) / sizeof ((array)[0]))

/*
 * endof
 *		Address of the element one past the last in an array.
 */
#define endof(array)	(&array[lengthof(array)])

/* ----------------------------------------------------------------
 *				Section 7:	exception handling definitions
 *							Assert, Trap, etc macros
 * ----------------------------------------------------------------
 */
/*
 * Exception Handling definitions
 */

typedef char *ExcMessage;
typedef struct Exception
{
	ExcMessage	message;
} Exception;

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
#define Trap(condition, exception) \
		do { \
			if ((assert_enabled) && (condition)) \
				ExceptionalCondition(CppAsString(condition), &(exception), \
						(char*)NULL, __FILE__, __LINE__); \
		} while (0)

/*
 *	TrapMacro is the same as Trap but it's intended for use in macros:
 *
 *		#define foo(x) (AssertM(x != 0) && bar(x))
 *
 *	Isn't CPP fun?
 */
#define TrapMacro(condition, exception) \
	((bool) ((! assert_enabled) || ! (condition) || \
			 (ExceptionalCondition(CppAsString(condition), \
								  &(exception), \
								  (char*) NULL, __FILE__, __LINE__))))

#ifndef USE_ASSERT_CHECKING
#define Assert(condition)
#define AssertMacro(condition)	((void)true)
#define AssertArg(condition)
#define AssertState(condition)
#define assert_enabled 0
#else
#define Assert(condition) \
		Trap(!(condition), FailedAssertion)

#define AssertMacro(condition) \
		((void) TrapMacro(!(condition), FailedAssertion))

#define AssertArg(condition) \
		Trap(!(condition), BadArg)

#define AssertState(condition) \
		Trap(!(condition), BadState)

extern int	assert_enabled;

#endif	 /* USE_ASSERT_CHECKING */

/*
 * LogTrap
 *		Generates an exception with a message if the given condition is true.
 *
 */
#define LogTrap(condition, exception, printArgs) \
		do { \
			if ((assert_enabled) && (condition)) \
				ExceptionalCondition(CppAsString(condition), &(exception), \
						vararg_format printArgs, __FILE__, __LINE__); \
		} while (0)

/*
 *	LogTrapMacro is the same as LogTrap but it's intended for use in macros:
 *
 *		#define foo(x) (LogAssertMacro(x != 0, "yow!") && bar(x))
 */
#define LogTrapMacro(condition, exception, printArgs) \
	((bool) ((! assert_enabled) || ! (condition) || \
			 (ExceptionalCondition(CppAsString(condition), \
								   &(exception), \
								   vararg_format printArgs, __FILE__, __LINE__))))

#ifndef USE_ASSERT_CHECKING
#define LogAssert(condition, printArgs)
#define LogAssertMacro(condition, printArgs) true
#define LogAssertArg(condition, printArgs)
#define LogAssertState(condition, printArgs)
#else
#define LogAssert(condition, printArgs) \
		LogTrap(!(condition), FailedAssertion, printArgs)

#define LogAssertMacro(condition, printArgs) \
		LogTrapMacro(!(condition), FailedAssertion, printArgs)

#define LogAssertArg(condition, printArgs) \
		LogTrap(!(condition), BadArg, printArgs)

#define LogAssertState(condition, printArgs) \
		LogTrap(!(condition), BadState, printArgs)

#ifdef ASSERT_CHECKING_TEST
extern int	assertTest(int val);

#endif
#endif	 /* USE_ASSERT_CHECKING */

/* ----------------------------------------------------------------
 *				Section 8:	Min, Max, Abs macros
 * ----------------------------------------------------------------
 */
/*
 * Max
 *		Return the maximum of two numbers.
 */
#define Max(x, y)		((x) > (y) ? (x) : (y))

/*
 * Min
 *		Return the minimum of two numbers.
 */
#define Min(x, y)		((x) < (y) ? (x) : (y))

/*
 * Abs
 *		Return the absolute value of the argument.
 */
#define Abs(x)			((x) >= 0 ? (x) : -(x))

/*
 * StrNCpy
 *		Like standard library function strncpy(), except that result string
 *		is guaranteed to be null-terminated --- that is, at most N-1 bytes
 *		of the source string will be kept.
 *		Also, the macro returns no result (too hard to do that without
 *		evaluating the arguments multiple times, which seems worse).
 */
#define StrNCpy(dst,src,len) \
	do \
	{ \
		char * _dst = (dst); \
		Size _len = (len); \
\
		if (_len > 0) \
		{ \
			strncpy(_dst, (src), _len); \
			_dst[_len-1] = '\0'; \
		} \
	} while (0)


/* Get a bit mask of the bits set in non-int32 aligned addresses */
#define INT_ALIGN_MASK (sizeof(int32) - 1)

/*
 * MemSet
 *	Exactly the same as standard library function memset(), but considerably
 *	faster for zeroing small word-aligned structures (such as parsetree nodes).
 *	This has to be a macro because the main point is to avoid function-call
 *	overhead.
 *
 *	We got the 64 number by testing this against the stock memset() on
 *	BSD/OS 3.0. Larger values were slower.	bjm 1997/09/11
 *
 *	I think the crossover point could be a good deal higher for
 *	most platforms, actually.  tgl 2000-03-19
 */
#define MemSet(start, val, len) \
	do \
	{ \
		int32 * _start = (int32 *) (start); \
		int		_val = (val); \
		Size	_len = (len); \
\
		if ((((long) _start) & INT_ALIGN_MASK) == 0 && \
			(_len & INT_ALIGN_MASK) == 0 && \
			_val == 0 && \
			_len <= MEMSET_LOOP_LIMIT) \
		{ \
			int32 * _stop = (int32 *) ((char *) _start + _len); \
			while (_start < _stop) \
				*_start++ = 0; \
		} \
		else \
			memset((char *) _start, _val, _len); \
	} while (0)

#define MEMSET_LOOP_LIMIT  64


/* ----------------------------------------------------------------
 *				Section 9: externs
 * ----------------------------------------------------------------
 */

extern Exception FailedAssertion;
extern Exception BadArg;
extern Exception BadState;

/* in utils/error/assert.c */
extern int ExceptionalCondition(char *conditionName,
					 Exception *exceptionP, char *details,
					 char *fileName, int lineNumber);


/* ----------------
 *		vararg_format is used by assert and the exception handling stuff
 * ----------------
 */
extern char *vararg_format(const char *fmt,...);



/* ----------------------------------------------------------------
 *				Section 10: berkeley-specific configuration
 *
 * this section contains settings which are only relevant to the UC Berkeley
 * sites.  Other sites can ignore this
 * ----------------------------------------------------------------
 */

/* ----------------
 *		storage managers
 *
 *		These are experimental and are not supported in the code that
 *		we distribute to other sites.
 * ----------------
 */
#ifdef NOT_USED
#define STABLE_MEMORY_STORAGE
#endif



/* ----------------------------------------------------------------
 *				Section 11: system-specific hacks
 *
 *		This should be limited to things that absolutely have to be
 *		included in every source file.	The changes should be factored
 *		into a separate file so that changes to one port don't require
 *		changes to c.h (and everyone recompiling their whole system).
 * ----------------------------------------------------------------
 */

#ifdef __CYGWIN32__
#define PG_BINARY	O_BINARY
#define	PG_BINARY_R	"rb"
#define	PG_BINARY_W	"wb"
#else
#define	PG_BINARY	0
#define	PG_BINARY_R	"r"
#define	PG_BINARY_W	"w"
#endif

#ifdef FIXADE
#if defined(hpux)
#include "port/hpux/fixade.h"	/* for unaligned access fixup */
#endif	 /* hpux */
#endif

#if defined(sun) && defined(__sparc__) && !defined(__SVR4)
#define memmove(d, s, l)		bcopy(s, d, l)
#include <unistd.h>
#include <varargs.h>
#endif

/* These are for things that are one way on Unix and another on NT */
#define NULL_DEV		"/dev/null"
#define SEP_CHAR		'/'

/* defines for dynamic linking on Win32 platform */
#ifdef __CYGWIN32__
#if __GNUC__ && ! defined (__declspec)
#error You need egcs 1.1 or newer for compiling!
#endif
#ifdef BUILDING_DLL
#define DLLIMPORT __declspec (dllexport)
#else							/* not BUILDING_DLL */
#define DLLIMPORT __declspec (dllimport)
#endif
#else							/* not CYGWIN */
#define DLLIMPORT
#endif

/* Provide prototypes for routines not present in a particular machine's
 * standard C library.	It'd be better to put these in config.h, but
 * in config.h we haven't yet included anything that defines size_t...
 */

#ifndef HAVE_SNPRINTF_DECL
extern int	snprintf(char *str, size_t count, const char *fmt,...);

#endif

#ifndef HAVE_VSNPRINTF_DECL
extern int	vsnprintf(char *str, size_t count, const char *fmt, va_list args);

#endif

#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#endif

/* ----------------
 *		end of c.h
 * ----------------
 */
#endif	 /* C_H */
