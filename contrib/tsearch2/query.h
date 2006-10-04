#ifndef __QUERY_H__
#define __QUERY_H__
/*
#define BS_DEBUG
*/

#include "ts_locale.h"
/*
 * item in polish notation with back link
 * to left operand
 */
typedef struct ITEM
{
	int8		type;
	int8		weight;
	int2		left;
	int4		val;
	/* user-friendly value, must correlate with WordEntry */
	uint32
				istrue:1,		/* use for ranking in Cover */
				length:11,
				distance:20;
}	ITEM;

/*
 *Storage:
 *	(len)(size)(array of ITEM)(array of operand in user-friendly form)
 */
typedef struct
{
	int4		len;
	int4		size;
	char		data[1];
}	QUERYTYPE;

#define HDRSIZEQT	( 2 * sizeof(int4) )
#define COMPUTESIZE(size,lenofoperand)	( HDRSIZEQT + (size) * sizeof(ITEM) + (lenofoperand) )
#define GETQUERY(x)  (ITEM*)( (char*)(x)+HDRSIZEQT )
#define GETOPERAND(x)	( (char*)GETQUERY(x) + ((QUERYTYPE*)(x))->size * sizeof(ITEM) )

#define ISOPERATOR(x) (  pg_mblen(x)==1 && ( *(x)=='!' || *(x)=='&' || *(x)=='|' || *(x)=='(' || *(x)==')' ) )

#define END				0
#define ERR				1
#define VAL				2
#define OPR				3
#define OPEN			4
#define CLOSE			5
#define VALSTOP			6		/* for stop words */

bool TS_execute(ITEM * curitem, void *checkval,
		   bool calcnot, bool (*chkcond) (void *checkval, ITEM * val));

#endif
