/*-------------------------------------------------------------------------
 *
 * builtins.h--
 *	  Declarations for operations on built-in types.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: mac.h,v 1.1 1998/10/03 05:41:01 momjian Exp $
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
	union
	{
		u_int32_t	ipv4_addr;	/* network byte order */
		/* add IPV6 address type here */
	}			addr;
}			ipaddr_struct;

typedef struct varlena ipaddr;

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
}			macaddr;


typedef struct manufacturer
{
	unsigned char a;
	unsigned char b;
	unsigned char c;
	char	   *name;
} manufacturer;

extern manufacturer manufacturers[];

#endif	 /* MAC_H */

