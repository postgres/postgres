/*-------------------------------------------------------------------------
 *
 * spi.h--
 *    
 *
 *-------------------------------------------------------------------------
 */
#ifndef	SPI_H
#define SPI_H

#include <string.h>
#include "postgres.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "catalog/pg_proc.h"
#include "parser/parse_query.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "tcop/dest.h"
#include "nodes/params.h"
#include "utils/fcache.h"
#include "utils/datum.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/syscache.h"
#include "utils/mcxt.h"
#include "utils/portal.h"
#include "catalog/pg_language.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "executor/execdefs.h"

typedef struct {
    uint32	alloced;	/* # of alloced vals */
    uint32	free;		/* # of free vals */
    TupleDesc	tupdesc;	/* tuple descriptor */
    HeapTuple	*vals;		/* tuples */
} SPITupleTable;

#define SPI_ERROR_CONNECT	-1
#define SPI_ERROR_COPY		-2
#define SPI_ERROR_OPUNKNOWN	-3
#define SPI_ERROR_UNCONNECTED	-4
#define SPI_ERROR_CURSOR	-5
#define SPI_ERROR_TRANSACTION	-6

#define SPI_OK_CONNECT		0
#define SPI_OK_FINISH		1
#define SPI_OK_FETCH		2
#define SPI_OK_UTILITY		3
#define SPI_OK_SELECT		4
#define SPI_OK_SELINTO		5
#define SPI_OK_INSERT		6
#define SPI_OK_DELETE		7
#define SPI_OK_UPDATE		8
#define SPI_OK_CURSOR		9

extern uint32 SPI_processed;
extern SPITupleTable *SPI_tuptable;

extern int SPI_connect (void);
extern int SPI_finish (void);
extern int SPI_exec (char *src);

#endif /* SPI_H */
