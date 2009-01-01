/*-------------------------------------------------------------------------
 *
 * regis.h
 *
 * Declarations for for fast regex subset, used by ISpell
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/tsearch/dicts/regis.h,v 1.6 2009/01/01 17:24:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef __REGIS_H__
#define __REGIS_H__

typedef struct RegisNode
{
	uint32
				type:2,
				len:16,
				unused:14;
	struct RegisNode *next;
	unsigned char data[1];
} RegisNode;

#define  RNHDRSZ	(offsetof(RegisNode,data))

#define RSF_ONEOF	1
#define RSF_NONEOF	2

typedef struct Regis
{
	RegisNode  *node;
	uint32
				issuffix:1,
				nchar:16,
				unused:15;
} Regis;

bool		RS_isRegis(const char *str);

void		RS_compile(Regis *r, bool issuffix, const char *str);
void		RS_free(Regis *r);

/*returns true if matches */
bool		RS_execute(Regis *r, char *str);

#endif
