#ifndef POSTGRES_H
#define POSTGRES_H

#include <stdio.h>

typedef char bool;
typedef signed char int8;
typedef signed short int16;
typedef signed int int32;

/*#define NULL ((void *) 0)*/
#define Min(x, y)           ((x) < (y) ? (x) : (y))
#define Max(x, y)           ((x) > (y) ? (x) : (y))
#define PointerIsValid(pointer) (bool)((void*)(pointer) != NULL)


typedef unsigned int Oid;
typedef int16 int2;
typedef int32 int4;
typedef float float4;
typedef double float8;
typedef unsigned char uint8;    /* == 8 bits */
typedef unsigned short uint16;  /* == 16 bits */
typedef unsigned int uint32;    /* == 32 bits */
typedef uint8 bits8;                    /* >= 8 bits */
typedef uint16 bits16;                  /* >= 16 bits */
typedef uint32 bits32;                  /* >= 32 bits */


typedef int4 aclitem;

#define InvalidOid		0
#define OidIsValid(objectId)  ((bool) (objectId != InvalidOid))

/* unfortunately, both regproc and RegProcedure are used */
typedef Oid regproc;
typedef Oid RegProcedure;

typedef char *((*func_ptr) ());


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
#define VARHDRSZ		sizeof(int32)

typedef struct varlena bytea;
typedef struct varlena text;

typedef int2 int28[8];
typedef Oid oid8[8];

#define ERROR stderr
#define elog fprintf

#define MaxAttrSize 10000

#define palloc malloc
#endif
