/*-------------------------------------------------------------------------
 *
 * spi.h
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPI_H
#define SPI_H

#include "postgres.h"

/*
 *	These are not needed by this file, but used by other programs
 *	using SPI
 */
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "tcop/dest.h"
#include "nodes/params.h"
#include "utils/fcache.h"
#include "utils/datum.h"
#include "utils/syscache.h"
#include "utils/portal.h"
#include "utils/builtins.h"
#include "catalog/pg_language.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "executor/execdefs.h"

typedef struct
{
	uint32		alloced;		/* # of alloced vals */
	uint32		free;			/* # of free vals */
	TupleDesc	tupdesc;		/* tuple descriptor */
	HeapTuple  *vals;			/* tuples */
} SPITupleTable;

#define SPI_ERROR_CONNECT		-1
#define SPI_ERROR_COPY			-2
#define SPI_ERROR_OPUNKNOWN		-3
#define SPI_ERROR_UNCONNECTED	-4
#define SPI_ERROR_CURSOR		-5
#define SPI_ERROR_ARGUMENT		-6
#define SPI_ERROR_PARAM			-7
#define SPI_ERROR_TRANSACTION	-8
#define SPI_ERROR_NOATTRIBUTE	-9
#define SPI_ERROR_NOOUTFUNC		-10
#define SPI_ERROR_TYPUNKNOWN	-11

#define SPI_OK_CONNECT			1
#define SPI_OK_FINISH			2
#define SPI_OK_FETCH			3
#define SPI_OK_UTILITY			4
#define SPI_OK_SELECT			5
#define SPI_OK_SELINTO			6
#define SPI_OK_INSERT			7
#define SPI_OK_DELETE			8
#define SPI_OK_UPDATE			9
#define SPI_OK_CURSOR			10

extern DLLIMPORT uint32 SPI_processed;
extern DLLIMPORT SPITupleTable *SPI_tuptable;
extern DLLIMPORT int SPI_result;

extern int	SPI_connect(void);
extern int	SPI_finish(void);
extern void SPI_push(void);
extern void SPI_pop(void);
extern int	SPI_exec(char *src, int tcount);
extern int	SPI_execp(void *plan, Datum *values, char *Nulls, int tcount);
extern void *SPI_prepare(char *src, int nargs, Oid *argtypes);
extern void *SPI_saveplan(void *plan);

extern HeapTuple SPI_copytuple(HeapTuple tuple);
extern HeapTuple SPI_modifytuple(Relation rel, HeapTuple tuple, int natts,
				int *attnum, Datum *Values, char *Nulls);
extern int	SPI_fnumber(TupleDesc tupdesc, char *fname);
extern char *SPI_fname(TupleDesc tupdesc, int fnumber);
extern char *SPI_getvalue(HeapTuple tuple, TupleDesc tupdesc, int fnumber);
extern Datum SPI_getbinval(HeapTuple tuple, TupleDesc tupdesc, int fnumber, bool *isnull);
extern char *SPI_gettype(TupleDesc tupdesc, int fnumber);
extern Oid	SPI_gettypeid(TupleDesc tupdesc, int fnumber);
extern char *SPI_getrelname(Relation rel);
extern void *SPI_palloc(Size size);
extern void *SPI_repalloc(void *pointer, Size size);
extern void SPI_pfree(void *pointer);
extern void SPI_freetuple(HeapTuple pointer);

#endif	 /* SPI_H */
