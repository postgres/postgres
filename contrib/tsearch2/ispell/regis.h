#ifndef __REGIS_H__
#define __REGIS_H__

#include "postgres.h"

typedef struct RegisNode
{
	uint32
				type:2,
				len:16,
				unused:14;
	struct RegisNode *next;
	unsigned char data[1];
}	RegisNode;

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
}	Regis;

bool		RS_isRegis(const char *str);

void		RS_compile(Regis * r, bool issuffix, char *str);
void		RS_free(Regis * r);

/*returns true if matches */
bool		RS_execute(Regis * r, char *str);

#endif
