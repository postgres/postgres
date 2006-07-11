/*-------------------------------------------------------------------------
 *
 * inet.h
 *	  Declarations for operations on INET datatypes.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/inet.h,v 1.24 2006/07/11 13:54:24 momjian Exp $
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
 * objects, ie, there is a varlena header (basically a length word) in front
 * of the struct type depicted above.
 *
 * Although these types are variable-length, the maximum length
 * is pretty short, so we make no provision for TOASTing them.
 */
typedef struct varlena inet;


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
#define DatumGetInetP(X)	((inet *) DatumGetPointer(X))
#define InetPGetDatum(X)	PointerGetDatum(X)
#define PG_GETARG_INET_P(n) DatumGetInetP(PG_GETARG_DATUM(n))
#define PG_RETURN_INET_P(x) return InetPGetDatum(x)
/* macaddr is a fixed-length pass-by-reference datatype */
#define DatumGetMacaddrP(X)    ((macaddr *) DatumGetPointer(X))
#define MacaddrPGetDatum(X)    PointerGetDatum(X)
#define PG_GETARG_MACADDR_P(n) DatumGetMacaddrP(PG_GETARG_DATUM(n))
#define PG_RETURN_MACADDR_P(x) return MacaddrPGetDatum(x)

#endif   /* INET_H */
