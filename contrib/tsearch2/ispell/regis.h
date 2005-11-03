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

int			RS_isRegis(const char *str);

int			RS_compile(Regis * r, int issuffix, const char *str);
void		RS_free(Regis * r);

/*возвращает 1 если матчится */
int			RS_execute(Regis * r, const char *str, int len);

#endif
