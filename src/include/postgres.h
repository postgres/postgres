/*-------------------------------------------------------------------------
 *
 * postgres.h
 *	  definition of (and support for) postgres system types.
 * this file is included by almost every .c in the system
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * $Id: postgres.h,v 1.41 2000/06/13 07:35:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 NOTES
 *		this file will eventually contain the definitions for the
 *		following (and perhaps other) system types:
 *
 *				int2	   int4		  float4	   float8
 *				Oid		   regproc	  RegProcedure
 *				aclitem
 *				struct varlena
 *				int2vector	  oidvector
 *				bytea	   text
 *				NameData   Name
 *
 *	 TABLE OF CONTENTS
 *		1)		simple type definitions
 *		2)		varlena and array types
 *		3)		TransactionId and CommandId
 *		4)		genbki macros used by catalog/pg_xxx.h files
 *		5)		random stuff
 *
 * ----------------------------------------------------------------
 */
#ifndef POSTGRES_H
#define POSTGRES_H

#include "postgres_ext.h"
#include "c.h"
#include "utils/elog.h"
#include "utils/mcxt.h"
#include "utils/palloc.h"

/* ----------------------------------------------------------------
 *				Section 1:	simple type definitions
 * ----------------------------------------------------------------
 */

typedef int4 aclitem;

#define InvalidOid		((Oid) 0)
#define OidIsValid(objectId)  ((bool) ((objectId) != InvalidOid))

/* unfortunately, both regproc and RegProcedure are used */
typedef Oid regproc;
typedef Oid RegProcedure;

#define RegProcedureIsValid(p)	OidIsValid(p)

/* ----------------------------------------------------------------
 *				Section 2:	variable length and array types
 * ----------------------------------------------------------------
 */
/* ----------------
 *		struct varlena
 * ----------------
 */
struct varlena
{
	int32		vl_len;
	char		vl_dat[1];
};

#define VARSIZE(PTR)	(((struct varlena *)(PTR))->vl_len)
#define VARDATA(PTR)	(((struct varlena *)(PTR))->vl_dat)
#define VARHDRSZ		((int32) sizeof(int32))

/*
 * These widely-used datatypes are just a varlena header and the data bytes.
 * There is no terminating null or anything like that --- the data length is
 * always VARSIZE(ptr) - VARHDRSZ.
 */
typedef struct varlena bytea;
typedef struct varlena text;
typedef struct varlena BpChar;	/* blank-padded char, ie SQL char(n) */
typedef struct varlena VarChar;	/* var-length char, ie SQL varchar(n) */

/*
 * Proposed new layout for variable length attributes
 * DO NOT USE YET - Jan
 */
#undef TUPLE_TOASTER_ACTIVE
#undef TUPLE_TOASTER_ALL_TYPES

#ifdef TUPLE_TOASTER_ACTIVE
typedef struct varattrib
{
	int32		va_header;		/* External/compressed storage */
	/* flags and item size */
	union
	{
		struct
		{
			int32		va_rawsize;		/* Plain data size */
		}			va_compressed;		/* Compressed stored attribute */

		struct
		{
			int32		va_rawsize;		/* Plain data size */
			Oid			va_valueid;		/* Unique identifier of value */
			Oid			va_longrelid;	/* RelID where to find chunks */
			Oid			va_rowid;		/* Main tables row Oid */
			int16		va_attno;		/* Main tables attno */
		}			va_external;/* External stored attribute */

		char		va_data[1]; /* Plain stored attribute */
	}			va_content;
}			varattrib;

#define VARATT_FLAG_EXTERNAL	0x8000
#define VARATT_FLAG_COMPRESSED	0x4000
#define VARATT_MASK_FLAGS		0xc000
#define VARATT_MASK_SIZE		0x3fff

#define VARATT_SIZEP(_PTR)	(((varattrib *)(_PTR))->va_header)
#define VARATT_SIZE(PTR)	(VARATT_SIZEP(PTR) & VARATT_MASK_SIZE)
#define VARATT_DATA(PTR)	(((varattrib *)(PTR))->va_content.va_data)

#define VARATT_IS_EXTENDED(PTR)		\
				((VARATT_SIZEP(PTR) & VARATT_MASK_FLAGS) != 0)
#define VARATT_IS_EXTERNAL(PTR)		\
				((VARATT_SIZEP(PTR) & VARATT_FLAG_EXTERNAL) != 0)
#define VARATT_IS_COMPRESSED(PTR)	\
				((VARATT_SIZEP(PTR) & VARATT_FLAG_COMPRESSED) != 0)

/* ----------
 * This is regularly declared in access/tuptoaster.h,
 * but we don't want to include that into every source,
 * so we (evil evil evil) declare it here once more.
 * ----------
 */
extern varattrib *heap_tuple_untoast_attr(varattrib * attr);

#define VARATT_GETPLAIN(_ARG,_VAR) {								\
				if (VARATTR_IS_EXTENDED(_ARG))						\
					(_VAR) = (void *)heap_tuple_untoast_attr(_ARG); \
				else												\
					(_VAR) = (_ARG);								\
			}
#define VARATT_FREE(_ARG,VAR) do {									\
				if ((void *)(_VAR) != (void *)(_ARG))				\
					pfree((void *)(_VAR));							\
			} while (0)
#else							/* TUPLE_TOASTER_ACTIVE */
#define VARATT_SIZE(__PTR) VARSIZE(__PTR)
#define VARATT_SIZEP(__PTR) VARSIZE(__PTR)
#endif	 /* TUPLE_TOASTER_ACTIVE */


/* fixed-length array types (these are not varlena's!) */

typedef int2 int2vector[INDEX_MAX_KEYS];
typedef Oid oidvector[INDEX_MAX_KEYS];

/* We want NameData to have length NAMEDATALEN and int alignment,
 * because that's how the data type 'name' is defined in pg_type.
 * Use a union to make sure the compiler agrees.
 */
typedef union nameData
{
	char		data[NAMEDATALEN];
	int			alignmentDummy;
} NameData;
typedef NameData *Name;

#define NameStr(name)	((name).data)

/* ----------------------------------------------------------------
 *				Section 3: TransactionId and CommandId
 * ----------------------------------------------------------------
 */

typedef uint32 TransactionId;

#define InvalidTransactionId	0

typedef uint32 CommandId;

#define FirstCommandId	0

/* ----------------------------------------------------------------
 *				Section 4: genbki macros used by the
 *						   catalog/pg_xxx.h files
 * ----------------------------------------------------------------
 */
#define CATALOG(x) \
	typedef struct CppConcat(FormData_,x)

/* Huh? */
#define DATA(x) extern int errno
#define DESCR(x) extern int errno
#define DECLARE_INDEX(x) extern int errno
#define DECLARE_UNIQUE_INDEX(x) extern int errno

#define BUILD_INDICES
#define BOOTSTRAP

#define BKI_BEGIN
#define BKI_END

/* ----------------------------------------------------------------
 *				Section 5:	random stuff
 *							CSIGNBIT, STATUS...
 * ----------------------------------------------------------------
 */

/* msb for int/unsigned */
#define ISIGNBIT (0x80000000)
#define WSIGNBIT (0x8000)

/* msb for char */
#define CSIGNBIT (0x80)

#define STATUS_OK				(0)
#define STATUS_ERROR			(-1)
#define STATUS_NOT_FOUND		(-2)
#define STATUS_INVALID			(-3)
#define STATUS_UNCATALOGUED		(-4)
#define STATUS_REPLACED			(-5)
#define STATUS_NOT_DONE			(-6)
#define STATUS_BAD_PACKET		(-7)
#define STATUS_FOUND			(1)

/* ---------------
 * Cyrillic on the fly charsets recode
 * ---------------
 */
#ifdef CYR_RECODE
extern void SetCharSet(void);
#endif	 /* CYR_RECODE */

#endif	 /* POSTGRES_H */
