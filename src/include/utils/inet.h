/*-------------------------------------------------------------------------
 *
 * inet.h
 *	  Declarations for operations on INET datatypes.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/inet.h,v 1.31 2010/01/02 16:58:10 momjian Exp $
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
 * Referencing all of the non-AF_INET types to AF_INET lets us work on
 * machines which may not have the appropriate address family (like
 * inet6 addresses when AF_INET6 isn't present) but doesn't cause a
 * dump/reload requirement.  Existing databases used AF_INET for the family
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
 * fmgr interface macros
 */
#define DatumGetInetP(X)	((inet *) PG_DETOAST_DATUM(X))
#define DatumGetInetPP(X)	((inet *) PG_DETOAST_DATUM_PACKED(X))
#define InetPGetDatum(X)	PointerGetDatum(X)
#define PG_GETARG_INET_P(n) DatumGetInetP(PG_GETARG_DATUM(n))
#define PG_GETARG_INET_PP(n) DatumGetInetPP(PG_GETARG_DATUM(n))
#define PG_RETURN_INET_P(x) return InetPGetDatum(x)
/* macaddr is a fixed-length pass-by-reference datatype */
#define DatumGetMacaddrP(X)    ((macaddr *) DatumGetPointer(X))
#define MacaddrPGetDatum(X)    PointerGetDatum(X)
#define PG_GETARG_MACADDR_P(n) DatumGetMacaddrP(PG_GETARG_DATUM(n))
#define PG_RETURN_MACADDR_P(x) return MacaddrPGetDatum(x)

#endif   /* INET_H */
