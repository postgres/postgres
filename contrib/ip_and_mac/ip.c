/*
 *	PostgreSQL type definitions for IP addresses.
 */

#include <stdio.h>

#include <postgres.h>
#include <utils/palloc.h>

/*
 *	This is the internal storage format for IP addresses:
 */

typedef struct ipaddr {
  unsigned char a;
  unsigned char b;
  unsigned char c;
  unsigned char d;
  unsigned char w;
  unsigned char pad1;
  short pad2;
} ipaddr;

/*
 *	Various forward declarations:
 */

ipaddr *ipaddr_in(char *str);
char *ipaddr_out(ipaddr *addr);

bool ipaddr_lt(ipaddr *a1, ipaddr *a2);
bool ipaddr_le(ipaddr *a1, ipaddr *a2);
bool ipaddr_eq(ipaddr *a1, ipaddr *a2);
bool ipaddr_ge(ipaddr *a1, ipaddr *a2);
bool ipaddr_gt(ipaddr *a1, ipaddr *a2);

bool ipaddr_ne(ipaddr *a1, ipaddr *a2);
int4 ipaddr_cmp(ipaddr *a1, ipaddr *a2);
bool ipaddr_like(ipaddr *a1, ipaddr *a2);

/*
 *	A utility macro used for sorting addresses numerically:
 */

#define Mag(addr) \
  ((unsigned long)((addr->a<<24)|(addr->b<<16)|(addr->c<<8)|(addr->d)))

/*
 *	IP address reader.  Note how the count returned by sscanf()
 *	is used to determine whether the mask size was specified.
 */

ipaddr *ipaddr_in(char *str) {
  int a, b, c, d, w;
  ipaddr *result;
  int count;

  if (strlen(str) > 0) {

    count = sscanf(str, "%d.%d.%d.%d/%d", &a, &b, &c, &d, &w);

    if (count < 4) {
      elog(ERROR, "ipaddr_in: error in parsing \"%s\"", str);
      return(NULL);
    }

    if (count == 4)
      w = 32;

    if ((a < 0) || (a > 255) || (b < 0) || (b > 255) ||
	(c < 0) || (c > 255) || (d < 0) || (d > 255) ||
	(w < 0) || (w > 32)) {
      elog(ERROR, "ipaddr_in: illegal address \"%s\"", str);
      return(NULL);
    }
  } else {
    a = b = c = d = w = 0;	/* special case for missing address */
  }

  result = (ipaddr *)palloc(sizeof(ipaddr));

  result->a = a;
  result->b = b;
  result->c = c;
  result->d = d;
  result->w = w;

  return(result);
}

/*
 *	IP address output function.  Note mask size specification
 *	generated only for subnets, not for plain host addresses.
 */

char *ipaddr_out(ipaddr *addr) {
  char *result;

  if (addr == NULL)
    return(NULL);

  result = (char *)palloc(32);

  if (Mag(addr) > 0) {
    if (addr->w == 32)
      sprintf(result, "%d.%d.%d.%d",
	      addr->a, addr->b, addr->c, addr->d);
    else
      sprintf(result, "%d.%d.%d.%d/%d",
	      addr->a, addr->b, addr->c, addr->d, addr->w);
  } else {
    result[0] = 0;		/* special case for missing address */
  }
  return(result);
}

/*
 *	Boolean tests.  The Mag() macro was defined above.
 */

bool ipaddr_lt(ipaddr *a1, ipaddr *a2) {
  unsigned long a1mag, a2mag;
  a1mag = Mag(a1);
  a2mag = Mag(a2);
  return (a1mag < a2mag);
};

bool ipaddr_le(ipaddr *a1, ipaddr *a2) {
  unsigned long a1mag, a2mag;
  a1mag = Mag(a1);
  a2mag = Mag(a2);
  return (a1mag <= a2mag);
};

bool ipaddr_eq(ipaddr *a1, ipaddr *a2) {
  unsigned long a1mag, a2mag;
  a1mag = Mag(a1);
  a2mag = Mag(a2);
  return ((a1mag == a2mag) && (a1->w == a2->w));
};

bool ipaddr_ge(ipaddr *a1, ipaddr *a2) {
  unsigned long a1mag, a2mag;
  a1mag = Mag(a1);
  a2mag = Mag(a2);
  return (a1mag >= a2mag);
};

bool ipaddr_gt(ipaddr *a1, ipaddr *a2) {
  unsigned long a1mag, a2mag;
  a1mag = Mag(a1);
  a2mag = Mag(a2);
  return (a1mag > a2mag);
};

bool ipaddr_ne(ipaddr *a1, ipaddr *a2) {
  unsigned long a1mag, a2mag;
  a1mag = Mag(a1);
  a2mag = Mag(a2);
  return ((a1mag != a2mag) || (a1->w != a2->w));
};

/*
 *	Comparison function for sorting:
 */

int4 ipaddr_cmp(ipaddr *a1, ipaddr *a2) {
  unsigned long a1mag = Mag(a1), a2mag = Mag(a2);
  if (a1mag < a2mag)
    return -1;
  else if (a1mag > a2mag)
    return 1;
  else
    return 0;
}

/*
 *	Our "similarity" operator checks whether two addresses are
 *	either the same node address, or, failing that, whether one
 *	of them contains the other.  This will be true if they have
 *	the same high bits down as far as the shortest mask reaches.
 */

unsigned long build_mask(unsigned char bits) {
  unsigned long mask = 0;
  int i;
  for (i = 0; i < bits; i++)
    mask = (mask >> 1) | 0x80000000;
  return mask;
}

bool ipaddr_like(ipaddr *a1, ipaddr *a2) {
  unsigned long a1bits, a2bits, maskbits;
  if ((a1->w == 0) || (a2->w == 0))
    return FALSE;
  if ((a1->w == 32) && (a2->w == 32))
    return ipaddr_eq(a1, a2);
  a1bits = Mag(a1);
  a2bits = Mag(a2);
  if (a1->w > a2->w) {
    maskbits = build_mask(a2->w);
    return ((a1bits & maskbits) == (a2bits & maskbits));
  } else {
    maskbits = build_mask(a1->w);
    return ((a2bits & maskbits) == (a1bits & maskbits));
  }
  return FALSE;
}

/*
 *	eof
 */
