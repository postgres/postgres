/*-------------------------------------------------------------------------
 *
 * builtins.h
 *	  Declarations for operations on built-in types.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: inet.h,v 1.6 2000/01/26 05:58:38 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef MAC_H
#define MAC_H

/*
 *	This is the internal storage format for IP addresses:
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


typedef struct manufacturer
{
	unsigned char a;
	unsigned char b;
	unsigned char c;
	char	   *name;
} manufacturer;

extern manufacturer manufacturers[];

#endif	 /* MAC_H */
