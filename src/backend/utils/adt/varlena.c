/*-------------------------------------------------------------------------
 *
 * varlena.c--
 *    Functions for the variable-length built-in types.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/varlena.c,v 1.11 1997/03/14 23:21:12 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <string.h>

#include "postgres.h"
#include "utils/palloc.h"
#include "utils/builtins.h"	/* where function declarations go */

/***************************************************************************** 
 *   USER I/O ROUTINES                                                       *
 *****************************************************************************/


#define	VAL(CH)		((CH) - '0')
#define	DIG(VAL)	((VAL) + '0')

/*
 *	byteain		- converts from printable representation of byte array
 *
 *	Non-printable characters must be passed as '\nnn' (octal) and are
 *	converted to internal form.  '\' must be passed as '\\'.
 *	elog(WARN, ...) if bad form.
 *
 *	BUGS:
 *		The input is scaned twice.
 *		The error checking of input is minimal.
 */
struct varlena *
byteain(char *inputText)
{
    register char	*tp;
    register char	*rp;
    register int	byte;
    struct varlena	*result;
    
    if (inputText == NULL)
	elog(WARN, "Bad input string for type bytea");
    
    for (byte = 0, tp = inputText; *tp != '\0'; byte++)
	if (*tp++ == '\\')
	    {
		if (*tp == '\\')
		    tp++;
		else if (!isdigit(*tp++) ||
			 !isdigit(*tp++) ||
			 !isdigit(*tp++))
		    elog(WARN, "Bad input string for type bytea");
	    }
    tp = inputText;
    byte += sizeof(int32);					/* varlena? */
    result = (struct varlena *) palloc(byte);
    result->vl_len = byte;					/* varlena? */
    rp = result->vl_dat;
    while (*tp != '\0')
	if (*tp != '\\' || *++tp == '\\')
	    *rp++ = *tp++;
	else {
	    byte = VAL(*tp++);
	    byte <<= 3;
	    byte += VAL(*tp++);
	    byte <<= 3;
	    *rp++ = byte + VAL(*tp++);
	}
    return(result);
}

/*
 * Shoves a bunch of memory pointed at by bytes into varlena.
 * BUGS:  Extremely unportable as things shoved can be string
 * representations of structs, etc.
 */
struct varlena *
shove_bytes(unsigned char *stuff, int len)
{
    struct varlena *result;
    
    result = (struct varlena *) palloc(len + sizeof(int32));
    result->vl_len = len;
    memmove(result->vl_dat, 
	    stuff + sizeof(int32),
	    len - sizeof(int32)); 
    return(result);
}



/*
 *	byteaout	- converts to printable representation of byte array
 *
 *	Non-printable characters are inserted as '\nnn' (octal) and '\' as
 *	'\\'.
 *
 *	NULL vlena should be an error--returning string with NULL for now.
 */
char *
byteaout(struct varlena	*vlena)
{
    register char	*vp;
    register char	*rp;
    register int	val;		/* holds unprintable chars */
    int		i;
    int		len;
    static	char	*result;
    
    if (vlena == NULL) {
	result = (char *) palloc(2);
	result[0] = '-';
	result[1] = '\0';
	return(result);
    }
    vp = vlena->vl_dat;
    len = 1;		/* empty string has 1 char */
    for (i = vlena->vl_len - sizeof(int32); i != 0; i--, vp++)	/* varlena? */
	if (*vp == '\\')
	    len += 2;
	else if (isascii(*vp) && isprint(*vp))
	    len++;
	else
	    len += 4;
    rp = result = (char *) palloc(len);
    vp = vlena->vl_dat;
    for (i = vlena->vl_len - sizeof(int32); i != 0; i--)	/* varlena? */
	if (*vp == '\\') {
	    vp++;
	    *rp++ = '\\';
	    *rp++ = '\\';
	} else if (isascii(*vp) && isprint(*vp))
	    *rp++ = *vp++;
	else {
	    val = *vp++;
	    *rp = '\\';
	    rp += 3;
	    *rp-- = DIG(val & 07);
	    val >>= 3;
	    *rp-- = DIG(val & 07);
	    val >>= 3;
	    *rp = DIG(val & 03);
	    rp += 3;
	}
    *rp = '\0';
    return(result);
}


/*
 *	textin		- converts "..." to internal representation
 */
struct varlena *
textin(char *inputText)
{
    struct varlena	*result;
    int		len;
    
    if (inputText == NULL)
	return(NULL);
    len = strlen(inputText) + VARHDRSZ;		
    result = (struct varlena *) palloc(len);
    VARSIZE(result) = len;
    memmove(VARDATA(result), inputText, len - VARHDRSZ);	
    return(result);
}

/*
 *	textout		- converts internal representation to "..."
 */
char *
textout(struct varlena *vlena)
{
    int	len;
    char *result;
    
    if (vlena == NULL) {
	result = (char *) palloc(2);
	result[0] = '-';
	result[1] = '\0';
	return(result);
    }
    len = VARSIZE(vlena) - VARHDRSZ;
    result = (char *) palloc(len + 1);
    memmove(result, VARDATA(vlena), len);
    result[len] = '\0';
    return(result);
}


/* ========== PUBLIC ROUTINES ========== */

/*
 * textlen -
 *    returns the actual length of a text* (which may be less than
 *    the VARSIZE of the text*)
 */

int textlen (text* t)
{
    int i = 0;
    int max = VARSIZE(t) - VARHDRSZ;
    char *ptr = VARDATA(t);
    while (i < max && *ptr++)
        i++;
    return i;
}

/*
 * textcat -
 *    takes two text* and returns a text* that is the concatentation of 
 *  the two
 */

/*
 * Rewrited by Sapa, sapa@hq.icb.chel.su. 8-Jul-96.
 */

text* 
textcat(text* t1, text* t2)
{
    int len1, len2, newlen;
    char *ptr;
    text* result;

    /* Check for NULL strings... */
    if (t1 == NULL) return t2;
    if (t2 == NULL) return t1;

    /* Check for ZERO-LENGTH strings... */
    /* I use <= instead of == , I know - it's paranoia, but... */
    if((len1 = VARSIZE(t1) - VARHDRSZ) <= 0) return t2;
    if((len2 = VARSIZE(t2) - VARHDRSZ) <= 0) return t1;

    result = (text *)palloc(newlen = len1 + len2 + VARHDRSZ);

    /* Fill data field of result string... */
    memcpy(ptr = VARDATA(result), VARDATA(t1), len1);
    memcpy(ptr + len1, VARDATA(t2), len2);

    /* Set size of result string... */
    VARSIZE(result) = newlen;

    return result;
}

/*
 *	texteq		- returns 1 iff arguments are equal
 *	textne		- returns 1 iff arguments are not equal
 */
bool
texteq(struct varlena *arg1, struct varlena *arg2)
{
    register int	len;
    register char	*a1p, *a2p;
    
    if (arg1 == NULL || arg2 == NULL)
	return((bool) NULL);
    if ((len = arg1->vl_len) != arg2->vl_len)
	return((bool) 0);
    a1p = arg1->vl_dat;
    a2p = arg2->vl_dat;
    /*
     * Varlenas are stored as the total size (data + size variable)
     * followed by the data.  The size variable is an int32 so the
     * length of the data is the total length less sizeof(int32)
     */
    len -= sizeof(int32);
    while (len-- != 0)
	if (*a1p++ != *a2p++)
	    return((bool) 0);
    return((bool) 1);
}

bool
textne(struct varlena *arg1, struct varlena *arg2) 
{
    return((bool) !texteq(arg1, arg2));
}

bool
text_lt(struct varlena *arg1, struct varlena *arg2) 
{
    int len;
#ifdef UNSIGNED_CHAR_TEXT
    unsigned
#endif
    char *a1p, *a2p;
    
    if (arg1 == NULL || arg2 == NULL)
	return((bool) 0);
    
    a1p = (unsigned char *)VARDATA(arg1);
    a2p = (unsigned char *)VARDATA(arg2);
    
    if ((len = arg1->vl_len) > arg2->vl_len)
	len = arg2->vl_len;
    len -= sizeof(int32);
    
    while (len != 0 && *a1p == *a2p)
	{
	    a1p++;
	    a2p++;
	    len--;
	}
    if (len)
	return (bool) (*a1p < *a2p);
    else
	return (bool) (arg1->vl_len < arg2->vl_len);
}

bool
text_le(struct varlena *arg1, struct varlena *arg2) 
{
    int len;
#ifdef UNSIGNED_CHAR_TEXT
    unsigned
#endif
    char *a1p, *a2p;
    
    if (arg1 == NULL || arg2 == NULL)
	return((bool) 0);
    
    a1p = (unsigned char *)VARDATA(arg1);
    a2p = (unsigned char *)VARDATA(arg2);
    
    if ((len = arg1->vl_len) > arg2->vl_len)
	len = arg2->vl_len;
    len -= sizeof(int32);					/* varlena! */
    
    while (len != 0 && *a1p == *a2p)
	{
	    a1p++;
	    a2p++;
	    len--;
	}
    if (len)
	return (bool) (*a1p < *a2p);
    else
	return ((bool) VARSIZE(arg1) <= VARSIZE(arg2));
}

bool
text_gt(struct varlena *arg1, struct varlena *arg2) 
{
    return ((bool) !text_le(arg1, arg2));
}

bool
text_ge(struct varlena *arg1, struct varlena *arg2) 
{
    return ((bool) !text_lt(arg1, arg2));
}

/*-------------------------------------------------------------
 * byteaGetSize
 *
 * get the number of bytes contained in an instance of type 'bytea'
 *-------------------------------------------------------------
 */
int32
byteaGetSize(struct varlena *v)
{
    register int len;
    
    len = v->vl_len - sizeof(v->vl_len);
    
    return(len);
}

/*-------------------------------------------------------------
 * byteaGetByte
 *
 * this routine treats "bytea" as an array of bytes.
 * It returns the Nth byte (a number between 0 and 255) or
 * it dies if the length of this array is less than n.
 *-------------------------------------------------------------
 */
int32
byteaGetByte(struct varlena *v, int32 n)
{
    int len;
    int byte;
    
    len = byteaGetSize(v);
    
    if (n>=len) {
	elog(WARN, "byteaGetByte: index (=%d) out of range [0..%d]",
	     n,len-1);
    }
    
    byte = (unsigned char) (v->vl_dat[n]);
    
    return((int32) byte);
}

/*-------------------------------------------------------------
 * byteaGetBit
 *
 * This routine treats a "bytea" type like an array of bits.
 * It returns the value of the Nth bit (0 or 1).
 * If 'n' is out of range, it dies!
 *
 *-------------------------------------------------------------
 */
int32
byteaGetBit(struct varlena *v, int32 n)
{
    int byteNo, bitNo;
    int byte;
    
    byteNo = n/8;
    bitNo = n%8;
    
    byte = byteaGetByte(v, byteNo);
    
    if (byte & (1<<bitNo)) {
	return((int32)1);
    } else {
	return((int32)0);
    }
}
/*-------------------------------------------------------------
 * byteaSetByte
 *
 * Given an instance of type 'bytea' creates a new one with
 * the Nth byte set to the given value.
 *
 *-------------------------------------------------------------
 */
struct varlena *
byteaSetByte(struct varlena *v, int32 n, int32 newByte)
{
    int len;
    struct varlena *res;
    
    len = byteaGetSize(v);
    
    if (n>=len) {
	elog(WARN,
	     "byteaSetByte: index (=%d) out of range [0..%d]",
	     n, len-1);
    }
    
    /*
     * Make a copy of the original varlena.
     */
    res = (struct varlena *) palloc(VARSIZE(v));
    if (res==NULL) {
	elog(WARN, "byteaSetByte: Out of memory (%d bytes requested)",
	     VARSIZE(v));
    }
    memmove((char *)res, (char *)v, VARSIZE(v));
    
    /*
     *  Now set the byte.
     */
    res->vl_dat[n] = newByte;
    
    return(res);
}

/*-------------------------------------------------------------
 * byteaSetBit
 *
 * Given an instance of type 'bytea' creates a new one with
 * the Nth bit set to the given value.
 *
 *-------------------------------------------------------------
 */
struct varlena *
byteaSetBit(struct varlena *v, int32 n, int32 newBit)
{
    struct varlena *res;
    int oldByte, newByte;
    int byteNo, bitNo;
    
    /*
     * sanity check!
     */
    if (newBit != 0 && newBit != 1) {
	elog(WARN, "byteaSetByte: new bit must be 0 or 1");
    }
    
    /*
     * get the byte where the bit we want is stored.
     */
    byteNo = n / 8;
    bitNo = n % 8;
    oldByte = byteaGetByte(v, byteNo);
    
    /*
     * calculate the new value for that byte
     */
    if (newBit == 0) {
	newByte = oldByte & (~(1<<bitNo));
    } else {
	newByte = oldByte | (1<<bitNo);
    }
    
    /*
     * NOTE: 'byteaSetByte' creates a copy of 'v' & sets the byte.
     */
    res = byteaSetByte(v, byteNo, newByte);
    
    return(res);
}
