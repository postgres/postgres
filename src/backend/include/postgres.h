/*-------------------------------------------------------------------------
 *
 * postgres.h--
 *    definition of (and support for) postgres system types.
 * this file is included by almost every .c in the system
 *
 * Copyright (c) 1995, Regents of the University of California
 *
 * $Id: postgres.h,v 1.2 1996/08/27 22:00:21 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *   NOTES
 *	this file will eventually contain the definitions for the
 *	following (and perhaps other) system types:
 *
 *		int2       int4       float4       float8
 *              Oid        regproc    RegProcedure
 *              aclitem
 *              struct varlena
 *              char8      char16      int28      oid8
 *              bytea      text
 *		NameData   Name	  
 *              oidint4    oidint2    oidname
 *
 *   TABLE OF CONTENTS
 *	1)	simple type definitions
 *	2)	varlena and array types
 *	3)	TransactionId and CommandId
 *	4)	genbki macros used by catalog/pg_xxx.h files
 *	5)	random SIGNBIT, MAXPGPATH, STATUS macros
 *
 * ----------------------------------------------------------------
 */
#ifndef POSTGRES_H
#define POSTGRES_H

#include "config.h"
#include "c.h"

/* ----------------------------------------------------------------
 *		Section 1:  simple type definitions
 * ----------------------------------------------------------------
 */

typedef	int16	int2;
typedef int32	int4;
typedef float	float4;
typedef double	float8;

typedef int4 aclitem;


typedef uint32	Oid;
#define InvalidOid	0
#define OidIsValid(objectId)  ((bool) (objectId != InvalidOid))

/* unfortunately, both regproc and RegProcedure are used */
typedef Oid regproc; 
typedef Oid RegProcedure;

/* ptr to func returning (char *) */
typedef char *	((*func_ptr)());	


#define RegProcedureIsValid(p)  OidIsValid(p)

/* ----------------------------------------------------------------
 *		Section 2:  variable length and array types
 * ----------------------------------------------------------------
 */
/* ----------------
 *	struct varlena
 * ----------------
 */
struct varlena {
	int32	vl_len;
	char	vl_dat[1];
};

#define	VARSIZE(PTR)	(((struct varlena *)(PTR))->vl_len)
#define	VARDATA(PTR)    (((struct varlena *)(PTR))->vl_dat)
#define	VARHDRSZ	sizeof(int32)

typedef struct varlena bytea;
typedef struct varlena text;

typedef struct char8 {
	char	data[8];
} char8;

/* ----------------
 *	char16
 * ----------------
 */
typedef struct char16 {
	char	data[16];
} char16;

typedef char16	*Char16;

typedef int2 int28[8];
typedef Oid oid8[8];

/* char16 is distinct from Name.
   now, you can truly change the max length of system names
   by altering the NAMEDATALEN define below.
   don't set the value too high because tuples are still constrained
   to be less than 8K 
*/

 /* NAMEDATALEN is the maximum string length (counting terminating null) 
    of a Name */ 
/* defined in Makefile.global */
/* if you change the value of NAMEDATALEN, you may need to change the
    alignment of the 'name' type in pg_type.h */
#ifndef NAMEDATALEN
#define NAMEDATALEN 16
#endif /* NAMEDATALEN */
/* OIDNAMELEN should be NAMEDATALEN + sizeof(Oid) */
#ifndef OIDNAMELEN
#define OIDNAMELEN 20
#endif /* OIDNAMELEN */

typedef struct nameData {
    char data[NAMEDATALEN];
} NameData;
typedef NameData	*Name;

/* ----------------
 *	oidint4
 *
 *	this is a new system type used by the file interface.
 * ----------------
 */
typedef struct OidInt4Data {
	Oid	oi_oid;
	int32		oi_int4;
} OidInt4Data;

typedef struct OidInt4Data	*OidInt4;

/* ----------------
 *	oidint2
 *
 *	this is a new system type used to define indices on two attrs.
 * ----------------
 */
typedef struct OidInt2Data {
	Oid	oi_oid;
	int16		oi_int2;
} OidInt2Data;

typedef struct OidInt2Data	*OidInt2;

/* ----------------
 *	oidname
 *
 *	this is a new system type used to define indices on two attrs.
 * ----------------
 */
typedef struct OidNameData {
	Oid	id;
	NameData name;
} OidNameData;

typedef struct OidNameData	*OidName;

/* ----------------------------------------------------------------
 *		Section 3: TransactionId and CommandId
 * ----------------------------------------------------------------
 */

typedef uint32			TransactionId;
#define InvalidTransactionId	0
typedef uint16			CommandId;
#define FirstCommandId	0

/* ----------------------------------------------------------------
 *		Section 4: genbki macros used by the
 *			   catalog/pg_xxx.h files
 * ----------------------------------------------------------------
 */
#define CATALOG(x) \
    typedef struct CppConcat(FormData_,x)

#define DATA(x) extern int errno
#define DECLARE_INDEX(x) extern int errno

#define BUILD_INDICES
#define BOOTSTRAP

#define BKI_BEGIN
#define BKI_END

/* ----------------------------------------------------------------
 *		Section 5:  random stuff
 *			    SIGNBIT, MAXPGPATH, STATUS...
 * ----------------------------------------------------------------
 */

/* msb for int/unsigned */
#define	SIGNBIT	(0x8000)

/* msb for char */
#define	CSIGNBIT (1 << 7)

/* ----------------
 *	global variables which should probably go someplace else.
 * ----------------
 */
#define	MAXPGPATH	128

#define STATUS_OK               (0)
#define STATUS_ERROR            (-1)
#define STATUS_NOT_FOUND        (-2)
#define STATUS_INVALID          (-3)
#define STATUS_UNCATALOGUED     (-4)
#define STATUS_REPLACED         (-5)
#define STATUS_NOT_DONE		(-6)
#define STATUS_BAD_PACKET	(-7)
#define STATUS_FOUND            (1)

#endif /* POSTGRES_H */
