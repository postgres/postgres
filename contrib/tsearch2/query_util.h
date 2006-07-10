#ifndef __QUERY_UTIL_H__
#define __QUERY_UTIL_H__

#include "postgres.h"
#include "utils/memutils.h"

#include "query.h"
#include "executor/spi.h"

typedef struct QTNode
{
	ITEM	   *valnode;
	uint32		flags;
	int4		nchild;
	char	   *word;
	uint32		sign;
	struct QTNode **child;
}	QTNode;

#define QTN_NEEDFREE	0x01
#define QTN_NOCHANGE	0x02
#define QTN_WORDFREE	0x04

typedef enum
{
	PlainMemory,
	SPIMemory,
	AggMemory
}	MemoryType;

QTNode	   *QT2QTN(ITEM * in, char *operand);
QUERYTYPE  *QTN2QT(QTNode * in, MemoryType memtype);
void		QTNFree(QTNode * in);
void		QTNSort(QTNode * in);
void		QTNTernary(QTNode * in);
void		QTNBinary(QTNode * in);
int			QTNodeCompare(QTNode * an, QTNode * bn);
QTNode	   *QTNCopy(QTNode * in, MemoryType memtype);
bool		QTNEq(QTNode * a, QTNode * b);


extern MemoryContext AggregateContext;

#define MEMALLOC(us, s)			( ((us)==SPIMemory) ? SPI_palloc(s) : ( ( (us)==PlainMemory ) ? palloc(s) : MemoryContextAlloc(AggregateContext, (s)) ) )
#define MEMFREE(us, p)			( ((us)==SPIMemory) ? SPI_pfree(p) : pfree(p) )

#endif
