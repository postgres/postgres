/*-------------------------------------------------------------------------
 *
 * inet.h
 *	  Declarations for operations on INET datatypes.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: inet.h,v 1.10 2001/03/22 04:01:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INET_H
#define INET_H

/*
 *	This is the internal storage format for IP addresses
 *	(both INET and CIDR datatypes):
 */
typedef struct
{
	unsigned char family;
	unsigned char bits;
	unsigned char type;
	union
	{
		unsigned int ipv4_addr; /* network byte order */
		/* add IPV6 address type here */
	}			addr;
} inet_struct;

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


#endif	 /* INET_H */
