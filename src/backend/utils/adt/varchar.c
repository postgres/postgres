/*-------------------------------------------------------------------------
 *
 * char.c--
 *    Functions for the built-in type char() and varchar().
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/varchar.c,v 1.2 1996/07/15 19:11:23 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>		/* for sprintf() */
#include <string.h>
#include "postgres.h"
#include "utils/palloc.h"
#include "utils/elog.h"

/*
 * CHAR() and VARCHAR() types are part of the ANSI SQL standard. CHAR()
 * is for blank-padded string whose length is specified in CREATE TABLE.
 * VARCHAR is for storing string whose length is at most the length specified
 * at CREATE TABLE time.
 *
 * It's hard to implement these types because we cannot figure out what
 * the length of the type from the type itself. I change (hopefully all) the
 * fmgr calls that invoke input functions of a data type to supply the
 * length also. (eg. in INSERTs, we have the tupleDescriptor which contains
 * the length of the attributes and hence the exact length of the char() or
 * varchar(). We pass this to bpcharin() or varcharin().) In the case where
 * we cannot determine the length, we pass in -1 instead and the input string
 * must be null-terminated.
 *
 * We actually implement this as a varlena so that we don't have to pass in
 * the length for the comparison functions. (The difference between "text"
 * is that we truncate and possibly blank-pad the string at insertion time.)
 *
 *                                                            - ay 6/95
 */


/***************************************************************************** 
 *   bpchar - char()                                                         *
 *****************************************************************************/

/*
 * bpcharin -
 *    converts a string of char() type to the internal representation.
 *    len is the length specified in () plus 4 bytes. (XXX dummy is here
 *    because we pass typelem as the second argument for array_in.)
 */
char *
bpcharin(char *s, int dummy, int typlen)
{
    char *result, *r;
    int	len = typlen - 4;
    int i;
    
    if (s == NULL)
	return((char *) NULL);

    if (typlen == -1) {
	/*
	 * this is here because some functions can't supply the typlen
	 */
	len = strlen(s);
	typlen = len + 4;
    }
    
#ifndef OPENLINK_PATCHES
    if (len < 1 || len > 4096)
	elog(WARN, "bpcharin: length of char() must be between 1 and 4096");
#else
    if (len > 4096)
	elog(WARN, "bpcharin: length of char() must be less than 4096");
#endif
    
    result = (char *) palloc(typlen);
    *(int32*)result = typlen;
    r = result + 4;
    for(i=0; i < len; i++, r++, s++) {
	*r = *s;
	if (*r == '\0')
	    break;
    }
    /* blank pad the string if necessary */
    for(; i < len; i++) {
	*r++ = ' ';
    }
    return(result);
}

char *
bpcharout(char *s)
{
    char *result;
    int len;

    if (s == NULL) {
	result = (char *) palloc(2);
	result[0] = '-';
	result[1] = '\0';
    } else {
	len = *(int32*)s - 4;
	result = (char *) palloc(len+1);
	strncpy(result, s+4, len);	/* these are blank-padded */
	result[len] = '\0';
    }
    return(result);
}

/***************************************************************************** 
 *   varchar - varchar()                                                     *
 *****************************************************************************/

/*
 * vcharin -
 *    converts a string of varchar() type to the internal representation.
 *    len is the length specified in () plus 4 bytes. (XXX dummy is here
 *    because we pass typelem as the second argument for array_in.)
 */
char *
varcharin(char *s, int dummy, int typlen)
{
    char *result;
    int	len = typlen - 4;
    
    if (s == NULL)
	return((char *) NULL);

    if (typlen == -1) {
	/*
	 * this is here because some functions can't supply the typlen
	 */
	len = strlen(s);
	typlen = len + 4;
    }
    
#ifndef OPENLINK_PATCHES
    if (len < 1 || len > 4096)
	elog(WARN, "bpcharin: length of char() must be between 1 and 4096");
#else
    if (len > 4096)
	elog(WARN, "varcharin: length of char() must be less than 4096");
#endif
    
    result = (char *) palloc(typlen);
    *(int32*)result = typlen;
    memset(result+4, 0, len);
    (void) strncpy(result+4, s, len);

    return(result);
}

char *
varcharout(char *s)
{
    char *result;
    int len;

    if (s == NULL) {
	result = (char *) palloc(2);
	result[0] = '-';
	result[1] = '\0';
    } else {
	len = *(int32*)s - 4;
	result = (char *) palloc(len+1);
	memset(result, 0, len+1);
	strncpy(result, s+4, len);
    }
    return(result);
}

/*****************************************************************************
 *  Comparison Functions used for bpchar 
 *****************************************************************************/

static int
bcTruelen(char *arg)
{
    char *s = arg + 4;
    int i;
    int len;

    len = *(int32*)arg - 4;
    for(i=len-1; i >= 0; i--) {
	if (s[i] != ' ')
	    break;
    }
    return (i+1);
}

int32
bpchareq(char *arg1, char *arg2)
{
    int len1, len2;

    if (arg1 == NULL || arg2 == NULL)
	return((int32) 0);
    len1 = bcTruelen(arg1);
    len2 = bcTruelen(arg2);

    if (len1!=len2)
	return 0;
    
    return(strncmp(arg1+4, arg2+4, len1) == 0);
}

int32
bpcharne(char *arg1, char *arg2)
{
    int len1, len2;

    if (arg1 == NULL || arg2 == NULL)
	return((int32) 0);
    len1 = bcTruelen(arg1);
    len2 = bcTruelen(arg2);

    if (len1!=len2)
	return 1;

    return(strncmp(arg1+4, arg2+4, len1) != 0);
}

int32
bpcharlt(char *arg1, char *arg2)
{
    int len1, len2;
    int cmp;
    
    if (arg1 == NULL || arg2 == NULL)
	return((int32) 0);
    len1 = bcTruelen(arg1);
    len2 = bcTruelen(arg2);

    cmp = strncmp(arg1+4, arg2+4, Min(len1,len2));
    if (cmp == 0)
	return (len1<len2);
    else 
	return (cmp < 0);
}

int32
bpcharle(char *arg1, char *arg2)
{
    int len1, len2;

    if (arg1 == NULL || arg2 == NULL)
	return((int32) 0);
    len1 = bcTruelen(arg1);
    len2 = bcTruelen(arg2);

    return(strncmp(arg1+4, arg2+4, Min(len1,len2)) <= 0);
}

int32
bpchargt(char *arg1, char *arg2)
{
    int len1, len2;
    int cmp;

    if (arg1 == NULL || arg2 == NULL)
	return((int32) 0);
    len1 = bcTruelen(arg1);
    len2 = bcTruelen(arg2);

    cmp = strncmp(arg1+4, arg2+4, Min(len1,len2));
    if (cmp == 0)
	return (len1 > len2);
    else
	return (cmp > 0);
}

int32
bpcharge(char *arg1, char *arg2)
{
    int len1, len2;

    if (arg1 == NULL || arg2 == NULL)
	return((int32) 0);
    len1 = bcTruelen(arg1);
    len2 = bcTruelen(arg2);

    return(strncmp(arg1+4, arg2+4, Min(len1,len2)) >= 0);
}

int32
bpcharcmp(char *arg1, char *arg2)
{
    int len1, len2;

    len1 = bcTruelen(arg1);
    len2 = bcTruelen(arg2);

    return(strncmp(arg1+4, arg2+4, Min(len1,len2)));
}

/*****************************************************************************
 *  Comparison Functions used for varchar 
 *****************************************************************************/

static int
vcTruelen(char *arg)
{
    char *s = arg + 4;
    int i;
    int len;

    len = *(int32*)arg - 4;
    for(i=0; i < len; i++) {
	if (*s++ == '\0')
	    break;
    }
    return i;
}

int32
varchareq(char *arg1, char *arg2)
{
    int len1, len2;

    if (arg1 == NULL || arg2 == NULL)
	return((int32) 0);
    len1 = vcTruelen(arg1);
    len2 = vcTruelen(arg2);

    if (len1!=len2)
	return 0;
    
    return(strncmp(arg1+4, arg2+4, len1) == 0);
}

int32
varcharne(char *arg1, char *arg2)
{
    int len1, len2;

    if (arg1 == NULL || arg2 == NULL)
	return((int32) 0);
    len1 = vcTruelen(arg1);
    len2 = vcTruelen(arg2);

    if (len1!=len2)
	return 1;

    return(strncmp(arg1+4, arg2+4, len1) != 0);
}

int32
varcharlt(char *arg1, char *arg2)
{
    int len1, len2;
    int cmp;
    
    if (arg1 == NULL || arg2 == NULL)
	return((int32) 0);
    len1 = vcTruelen(arg1);
    len2 = vcTruelen(arg2);

    cmp = strncmp(arg1+4, arg2+4, Min(len1,len2));
    if (cmp == 0)
	return (len1<len2);
    else 
	return (cmp < 0);
}

int32
varcharle(char *arg1, char *arg2)
{
    int len1, len2;

    if (arg1 == NULL || arg2 == NULL)
	return((int32) 0);
    len1 = vcTruelen(arg1);
    len2 = vcTruelen(arg2);

    return(strncmp(arg1+4, arg2+4, Min(len1,len2)) <= 0);
}

int32
varchargt(char *arg1, char *arg2)
{
    int len1, len2;
    int cmp;

    if (arg1 == NULL || arg2 == NULL)
	return((int32) 0);
    len1 = vcTruelen(arg1);
    len2 = vcTruelen(arg2);

    cmp = strncmp(arg1+4, arg2+4, Min(len1,len2));
    if (cmp == 0)
	return (len1 > len2);
    else
	return (cmp > 0);
}

int32
varcharge(char *arg1, char *arg2)
{
    int len1, len2;

    if (arg1 == NULL || arg2 == NULL)
	return((int32) 0);
    len1 = vcTruelen(arg1);
    len2 = vcTruelen(arg2);

    return(strncmp(arg1+4, arg2+4, Min(len1,len2)) >= 0);
}

int32
varcharcmp(char *arg1, char *arg2)
{
    int len1, len2;

    len1 = vcTruelen(arg1);
    len2 = vcTruelen(arg2);

    return(strncmp(arg1+4, arg2+4, Min(len1,len2)));
}

/*****************************************************************************
 * Hash functions (modified from hashtext in access/hash/hashfunc.c)
 *****************************************************************************/

uint32 hashbpchar(struct varlena *key)
{
    int keylen;
    char *keydata;
    uint32 n;
    int loop;

    keydata = VARDATA(key);
    keylen = bcTruelen((char*)key);

#define HASHC   n = *keydata++ + 65599 * n

    n = 0;
    if (keylen > 0) {
	loop = (keylen + 8 - 1) >> 3;
	
	switch (keylen & (8 - 1)) {
	case 0:
	    do {	/* All fall throughs */
		HASHC;
	    case 7:
		HASHC;
	    case 6:
		HASHC;
	    case 5:
		HASHC;
	    case 4:
		HASHC;
	    case 3:
		HASHC;
	    case 2:
		HASHC;
	    case 1:
		HASHC;
	    } while (--loop);
	}
    }
    return (n);
}	

uint32 hashvarchar(struct varlena *key)
{
    int keylen;
    char *keydata;
    uint32 n;
    int loop;

    keydata = VARDATA(key);
    keylen = vcTruelen((char*)key);

#define HASHC   n = *keydata++ + 65599 * n

    n = 0;
    if (keylen > 0) {
	loop = (keylen + 8 - 1) >> 3;
	
	switch (keylen & (8 - 1)) {
	case 0:
	    do {	/* All fall throughs */
		HASHC;
	    case 7:
		HASHC;
	    case 6:
		HASHC;
	    case 5:
		HASHC;
	    case 4:
		HASHC;
	    case 3:
		HASHC;
	    case 2:
		HASHC;
	    case 1:
		HASHC;
	    } while (--loop);
	}
    }
    return (n);
}	

