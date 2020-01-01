/*-------------------------------------------------------------------------
 *
 * inet.h
 *	  Declarations for operations on INET datatypes.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/inet.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INET_H
#define INET_H

#include "fmgr.h"

/*
 *	This is the internal storage format for IP addresses
 *	(both INET and CIDR datatypes):
 */
typedef struct
{
	unsigned char family;		/* PGSQL_AF_INET or PGSQL_AF_INET6 */
	unsigned char bits;			/* number of bits in netmask */
	unsigned char ipaddr[16];	/* up to 128 bits of address */
} inet_struct;

/*
 * We use these values for the "family" field.
 *
 * Referencing all of the non-AF_INET types to AF_INET lets us work on
 * machines which may not have the appropriate address family (like
 * inet6 addresses when AF_INET6 isn't present) but doesn't cause a
 * dump/reload requirement.  Pre-7.4 databases used AF_INET for the family
 * type on disk.
 */
#define PGSQL_AF_INET	(AF_INET + 0)
#define PGSQL_AF_INET6	(AF_INET + 1)

/*
 * Both INET and CIDR addresses are represented within Postgres as varlena
 * objects, ie, there is a varlena header in front of the struct type
 * depicted above.  This struct depicts what we actually have in memory
 * in "uncompressed" cases.  Note that since the maximum data size is only
 * 18 bytes, INET/CIDR will invariably be stored into tuples using the
 * 1-byte-header varlena format.  However, we have to be prepared to cope
 * with the 4-byte-header format too, because various code may helpfully
 * try to "decompress" 1-byte-header datums.
 */
typedef struct
{
	char		vl_len_[4];		/* Do not touch this field directly! */
	inet_struct inet_data;
} inet;

/*
 *	Access macros.  We use VARDATA_ANY so that we can process short-header
 *	varlena values without detoasting them.  This requires a trick:
 *	VARDATA_ANY assumes the varlena header is already filled in, which is
 *	not the case when constructing a new value (until SET_INET_VARSIZE is
 *	called, which we typically can't do till the end).  Therefore, we
 *	always initialize the newly-allocated value to zeroes (using palloc0).
 *	A zero length word will look like the not-1-byte case to VARDATA_ANY,
 *	and so we correctly construct an uncompressed value.
 *
 *	Note that ip_addrsize(), ip_maxbits(), and SET_INET_VARSIZE() require
 *	the family field to be set correctly.
 */
#define ip_family(inetptr) \
	(((inet_struct *) VARDATA_ANY(inetptr))->family)

#define ip_bits(inetptr) \
	(((inet_struct *) VARDATA_ANY(inetptr))->bits)

#define ip_addr(inetptr) \
	(((inet_struct *) VARDATA_ANY(inetptr))->ipaddr)

#define ip_addrsize(inetptr) \
	(ip_family(inetptr) == PGSQL_AF_INET ? 4 : 16)

#define ip_maxbits(inetptr) \
	(ip_family(inetptr) == PGSQL_AF_INET ? 32 : 128)

#define SET_INET_VARSIZE(dst) \
	SET_VARSIZE(dst, VARHDRSZ + offsetof(inet_struct, ipaddr) + \
				ip_addrsize(dst))


/*
 *	This is the internal storage format for MAC addresses:
 */
typedef struct macaddr
{
	unsigned char a;
	unsigned char b;
	unsigned char c;
	unsigned char d;
	unsigned char e;
	unsigned char f;
} macaddr;

/*
 *	This is the internal storage format for MAC8 addresses:
 */
typedef struct macaddr8
{
	unsigned char a;
	unsigned char b;
	unsigned char c;
	unsigned char d;
	unsigned char e;
	unsigned char f;
	unsigned char g;
	unsigned char h;
} macaddr8;

/*
 * fmgr interface macros
 */
#define DatumGetInetPP(X)	((inet *) PG_DETOAST_DATUM_PACKED(X))
#define InetPGetDatum(X)	PointerGetDatum(X)
#define PG_GETARG_INET_PP(n) DatumGetInetPP(PG_GETARG_DATUM(n))
#define PG_RETURN_INET_P(x) return InetPGetDatum(x)
/* obsolescent variants */
#define DatumGetInetP(X)	((inet *) PG_DETOAST_DATUM(X))
#define PG_GETARG_INET_P(n) DatumGetInetP(PG_GETARG_DATUM(n))

/* macaddr is a fixed-length pass-by-reference datatype */
#define DatumGetMacaddrP(X)    ((macaddr *) DatumGetPointer(X))
#define MacaddrPGetDatum(X)    PointerGetDatum(X)
#define PG_GETARG_MACADDR_P(n) DatumGetMacaddrP(PG_GETARG_DATUM(n))
#define PG_RETURN_MACADDR_P(x) return MacaddrPGetDatum(x)

/* macaddr8 is a fixed-length pass-by-reference datatype */
#define DatumGetMacaddr8P(X)	((macaddr8 *) DatumGetPointer(X))
#define Macaddr8PGetDatum(X)	PointerGetDatum(X)
#define PG_GETARG_MACADDR8_P(n) DatumGetMacaddr8P(PG_GETARG_DATUM(n))
#define PG_RETURN_MACADDR8_P(x) return Macaddr8PGetDatum(x)

/*
 * Support functions in network.c
 */
extern inet *cidr_set_masklen_internal(const inet *src, int bits);
extern int	bitncmp(const unsigned char *l, const unsigned char *r, int n);
extern int	bitncommon(const unsigned char *l, const unsigned char *r, int n);

#endif							/* INET_H */
