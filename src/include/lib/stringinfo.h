/*-------------------------------------------------------------------------
 *
 * stringinfo.h--
 *	  Declarations/definitons for "string" functions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: stringinfo.h,v 1.8 1998/12/14 08:11:17 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef STRINGINFO_H
#define STRINGINFO_H


/*-------------------------
 * StringInfoData holds information about a string.
 *		'data' is the string.
 *		'len' is the current string length (as returned by 'strlen')
 *		'maxlen' is the size in bytes of 'data', i.e. the maximum string
 *				size (including the terminating '\0' char) that we can
 *				currently store in 'data' without having to reallocate
 *				more space.
 */
typedef struct StringInfoData
{
	char	   *data;
	int			maxlen;
	int			len;
} StringInfoData;

typedef StringInfoData *StringInfo;

/*------------------------
 * makeStringInfo
 * create a 'StringInfoData' & return a pointer to it.
 */
extern StringInfo makeStringInfo(void);

/*------------------------
 * appendStringInfo
 * similar to 'strcat' but reallocates more space if necessary...
 */
extern void appendStringInfo(StringInfo str, const char *fmt,...);

#endif	 /* STRINGINFO_H */
