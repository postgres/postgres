/*
 *	PostgreSQL type definitions for IP addresses.
 *
 *	$Id: ip.c,v 1.4 1998/06/16 04:34:29 momjian Exp $
 */

#include <stdio.h>

#include <postgres.h>
#include <utils/palloc.h>

/*
 *	This is the internal storage format for IP addresses:
 */

typedef struct ipaddr
{
	uint32		address;
	int16		width;
}			ipaddr;

/*
 *	Various forward declarations:
 */

ipaddr	   *ipaddr_in(char *str);
char	   *ipaddr_out(ipaddr * addr);

bool		ipaddr_lt(ipaddr * a1, ipaddr * a2);
bool		ipaddr_le(ipaddr * a1, ipaddr * a2);
bool		ipaddr_eq(ipaddr * a1, ipaddr * a2);
bool		ipaddr_ge(ipaddr * a1, ipaddr * a2);
bool		ipaddr_gt(ipaddr * a1, ipaddr * a2);

bool		ipaddr_ne(ipaddr * a1, ipaddr * a2);

int4		ipaddr_cmp(ipaddr * a1, ipaddr * a2);

bool		ipaddr_in_net(ipaddr * a1, ipaddr * a2);
ipaddr	   *ipaddr_mask(ipaddr * a);
ipaddr	   *ipaddr_bcast(ipaddr * a);

/*
 *	Build a mask of a given width:
 */

unsigned long
build_mask(unsigned char bits)
{
	unsigned long mask = 0;
	int			i;

	for (i = 0; i < bits; i++)
		mask = (mask >> 1) | 0x80000000;
	return mask;
}

/*
 *	IP address reader.	Note how the count returned by sscanf()
 *	is used to determine whether the mask size was specified.
 */

ipaddr *
ipaddr_in(char *str)
{
	int			a,
				b,
				c,
				d,
				w;
	ipaddr	   *result;
	int			count;

	if (strlen(str) > 0)
	{

		count = sscanf(str, "%d.%d.%d.%d/%d", &a, &b, &c, &d, &w);

		if (count < 4)
		{
			elog(ERROR, "ipaddr_in: error in parsing \"%s\"", str);
			return (NULL);
		}
		if ( count == 3 ) {
		       d = 0;
		       count = 4;
		};
		if (count == 4)
		{
			w = 32;
			if ( a >= 192 && a < 224 && d == 0 ) w = 24;
			if ( a >= 128 && a < 192 && d == 0 && c == 0 ) w = 16;
			if ( a > 0    && a < 128 && c == 0 && b == 0 && a < 128 ) w = 8;
			if ( a == 0 && b == 0 && c == 0 && d == 0 ) w = 0;
		};
		if ((a < 0) || (a > 255) || (b < 0) || (b > 255) ||
			(c < 0) || (c > 255) || (d < 0) || (d > 255) ||
			(w < 0) || (w > 32))
		{
			elog(ERROR, "ipaddr_in: illegal address \"%s\"", str);
			return (NULL);
		}
	}
	else
	{
		a = b = c = d = w = 255;  /* special case for missing address */
	}

	result = (ipaddr *) palloc(sizeof(ipaddr));

	result->address = (uint32) ((a << 24) | (b << 16) | (c << 8) | d);
	result->width = w;

	return (result);
}

/*
 *	IP address output function.  Note mask size specification
 *	generated only for subnets, not for plain host addresses.
 */

char *
ipaddr_out(ipaddr * addr)
{
	char	   *result;
	int a, b, c, d, w;
	if (addr == NULL)
		return (NULL);

	result = (char *) palloc(32);

	w = addr->width;
	a = (addr->address >> 24) & 0xff;
	b = (addr->address >> 16) & 0xff;
	c = (addr->address >>  8) & 0xff;
	d = (addr->address >>  0) & 0xff;
	/* Check by missing address (w > 32 )  */
	if ( w >= 0 && w <= 32  )
	{
		/* In case of NATURAL network don't output the prefix */
		if ( (a == 0  && b == 0 && c == 0 && d == 0 && w ==  0 ) ||
		     (a < 128 && b == 0 && c == 0 && d == 0 && w ==  8 ) ||
		     (a < 192           && c == 0 && d == 0 && w == 16 ) ||
		     (a < 224                     && d == 0 && w == 24 ) ||
		     ( d != 0 ) ) w = -1;
		if (w == -1 )
			sprintf(result, "%d.%d.%d.%d",a,b,c,d);
		else
			sprintf(result, "%d.%d.%d.%d/%d",a,b,c,d,w);
	}
	else
	{
		result[0] = 0;			/* special case for missing address */
	}
	return (result);
}

/*
 * Print ipaddr by format
 * %A - address
 * %M - maska
 * %P - prefix
 * %B - negated maska
 */
# define TXT_LEN_0 4
text *
ipaddr_print(ipaddr * addr, text *fmt)
{
	text       *result;
	char *p, *op;
	uint32          aaa;
	int a, b, c, d;
	if (addr == NULL)
		return (NULL);

	result = (text  *) palloc( sizeof(text) + 64 );

	/* Check by missing address (w > 32 )  */
	for ( p = fmt->vl_dat, op = result->vl_dat; *p && (p - fmt->vl_dat) < (fmt->vl_len - TXT_LEN_0)  && (op - result->vl_dat) < 48; p++) {
	       if ( *p != '%' ) {
		       *op++ = *p;
		       continue;
	       };
	       p++;
	       if ( *p == 'A' )
	       {
		       aaa = addr->address;
		       goto pta;
	       };
	       if ( *p == 'M' ) {
		       aaa = build_mask(addr->width);
		       goto pta;
	       }
	       if ( *p == 'B' ) {
		       aaa = build_mask(32 - addr->width) >> addr->width;
		       goto pta;
	       }
	       if ( *p == 'P' ) {
		       sprintf(op,"%d",addr->width);
		       while ( *op) op++;
		       continue;
	       };
	       *op++ = *p;
	       continue;
pta:
	       a = (aaa >> 24) & 0xff;
	       b = (aaa >> 16) & 0xff;
	       c = (aaa >>  8) & 0xff;
	       d = (aaa >>  0) & 0xff;
	       sprintf(op, "%d.%d.%d.%d",a,b,c,d);
	       while ( *op ) op++;
	       continue;
	};
	*op = 0;
	result->vl_len = (op - result->vl_dat) + TXT_LEN_0;
	return (result);
}

/*
 *	Boolean tests for magnitude.
 */

bool
ipaddr_lt(ipaddr * a1, ipaddr * a2)
{
	if ( a1->address == a2->address ) return(a1->width <  a2->width);
	return (a1->address < a2->address);
};

bool
ipaddr_le(ipaddr * a1, ipaddr * a2)
{
	if ( a1->address == a2->address ) return(a1->width <= a2->width);
	return (a1->address <= a2->address);
};

bool
ipaddr_eq(ipaddr * a1, ipaddr * a2)
{
	if ( a1->address == a2->address ) return(a1->width == a2->width);
	return (a1->address == a2->address);
};

bool
ipaddr_ge(ipaddr * a1, ipaddr * a2)
{
	if ( a1->address == a2->address ) return(a1->width >= a2->width);
	return (a1->address >= a2->address);
};

bool
ipaddr_gt(ipaddr * a1, ipaddr * a2)
{
	if ( a1->address == a2->address ) return(a1->width > a2->width);
	return (a1->address > a2->address);
};

bool
ipaddr_ne(ipaddr * a1, ipaddr * a2)
{
	if ( a1->address == a2->address ) return(a1->width != a2->width);
	return (a1->address != a2->address);
};

/*
 *	Comparison function for sorting:
 */

int4
ipaddr_cmp(ipaddr * a1, ipaddr * a2)
{
	if (a1->address < a2->address)
		return -1;
	else if (a1->address > a2->address)
		return 1;
	else
	{
	    if (a1->width < a2->width)
		    return -1;
	    else if (a1->width > a2->width)
		    return 1;
	}
	return 0;
}

/*
 *      The number of hosts in the network
 */
int4
ipaddr_len(ipaddr * a)
{
	if ( a->width > 32 || a->width < 0 ) return(0);
	return(1 << (32 - a->width));
}

/*
 *      The number of network bits
 */
int4
ipaddr_pref(ipaddr * a)
{
	if ( a->width > 32 || a->width < 0 ) return(0);
	return(a->width);
}

/*
 *      The host addr as an integer
 */
int4
ipaddr_integer(ipaddr * a)
{
	return(a->address);
}

/*
 *	Test whether an address is within a given subnet:
 */

bool
ipaddr_in_net(ipaddr * a1, ipaddr * a2)
{
	uint32		maskbits;

	if (a1->width < a2->width)
		return FALSE;
	if ((a1->width == 32) && (a2->width == 32))
		return ipaddr_eq(a1, a2);
	maskbits = build_mask(a2->width);
	if ((a1->address & maskbits) == (a2->address & maskbits))
		return TRUE;
	return FALSE;
}

/*
 *      Test whether an address is the network or a host in the network:
 */

bool
ipaddr_is_net(ipaddr * a)
{
	uint32		maskbits;

	if (a->width == 32)
		return FALSE;
	maskbits = build_mask(a->width);
	if ( (a->address & maskbits) == a->address )
		return TRUE;
	return FALSE;
}

/*
 *	Pick out just the mask of a network:
 */

ipaddr *
ipaddr_mask(ipaddr * a)
{
	ipaddr	   *result;

	result = (ipaddr *) palloc(sizeof(ipaddr));
	result->address = build_mask(a->width);
	result->width = 32;

	return result;
}

/*
 *	Return the broadcast address of a network:
 */

ipaddr *
ipaddr_bcast(ipaddr * a)
{
	ipaddr	   *result;

	result = (ipaddr *) palloc(sizeof(ipaddr));
	result->address = a->address;
	result->address |= (build_mask(32 - a->width) >> a->width);
	result->width = 32;

	return result;
}

/*
 *      Return the base network of the address/network:
 */

ipaddr *
ipaddr_net(ipaddr * a)
{
	ipaddr	   *result;

	result = (ipaddr *) palloc(sizeof(ipaddr));
	result->address = a->address;
	result->address &= build_mask(a->width);
	result->width = a->width;

	return result;
}

/*
 * Compose ipaddr from ADDR and PREFIX
 */
ipaddr *
ipaddr_compose(int4 addr, int4 pref)
{
	ipaddr	   *result;

	result = (ipaddr *) palloc(sizeof(ipaddr));
	if ( pref < 0 || pref > 32 ) {
	       pref = 255;
	       addr = 0;
	};
	result->address = addr;
	result->width = pref;
	return result;
}

/*
 *      Plus and Minus operators
 */
ipaddr *
ipaddr_plus(ipaddr * a, int4 i)
{
	ipaddr	   *result;

	result = (ipaddr *) palloc(sizeof(ipaddr));
	result->address = a->address + i;
	result->width = a->width;

	return result;
}

ipaddr *
ipaddr_minus(ipaddr * a, int4 i)
{
	ipaddr	   *result;

	result = (ipaddr *) palloc(sizeof(ipaddr));
	result->address = a->address - i;
	result->width = a->width;

	return result;
}

/*
 *	eof
 */
