/*-------------------------------------------------------------------------
 *
 * spi.h
 *
 * $Id: spi.h,v 1.39 2003/10/01 21:30:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPI_H
#define SPI_H

/*
 * This file may be used by client modules that haven't already
 * included postgres.h
 */
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
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/portal.h"
#include "utils/syscache.h"
#include "catalog/pg_language.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "executor/execdefs.h"

typedef struct
{
	MemoryContext tuptabcxt;	/* memory context of result table */
	uint32		alloced;		/* # of alloced vals */
	uint32		free;			/* # of free vals */
	TupleDesc	tupdesc;		/* tuple descriptor */
	HeapTuple  *vals;			/* tuples */
} SPITupleTable;

#define SPI_ERROR_CONNECT		(-1)
#define SPI_ERROR_COPY			(-2)
#define SPI_ERROR_OPUNKNOWN		(-3)
#define SPI_ERROR_UNCONNECTED	(-4)
#define SPI_ERROR_CURSOR		(-5)
#define SPI_ERROR_ARGUMENT		(-6)
#define SPI_ERROR_PARAM			(-7)
#define SPI_ERROR_TRANSACTION	(-8)
#define SPI_ERROR_NOATTRIBUTE	(-9)
#define SPI_ERROR_NOOUTFUNC		(-10)
#define SPI_ERROR_TYPUNKNOWN	(-11)

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
extern DLLIMPORT Oid SPI_lastoid;
extern DLLIMPORT SPITupleTable *SPI_tuptable;
extern DLLIMPORT int SPI_result;

extern int	SPI_connect(void);
extern int	SPI_finish(void);
extern void SPI_push(void);
extern void SPI_pop(void);
extern int	SPI_exec(const char *src, int tcount);
extern int SPI_execp(void *plan, Datum *values, const char *Nulls,
		  int tcount);
extern int SPI_execp_current(void *plan, Datum *values, const char *Nulls,
							 bool useCurrentSnapshot, int tcount);
extern void *SPI_prepare(const char *src, int nargs, Oid *argtypes);
extern void *SPI_saveplan(void *plan);
extern int	SPI_freeplan(void *plan);

extern HeapTuple SPI_copytuple(HeapTuple tuple);
extern TupleDesc SPI_copytupledesc(TupleDesc tupdesc);
extern TupleTableSlot *SPI_copytupleintoslot(HeapTuple tuple,
					  TupleDesc tupdesc);
extern HeapTuple SPI_modifytuple(Relation rel, HeapTuple tuple, int natts,
				int *attnum, Datum *Values, const char *Nulls);
extern int	SPI_fnumber(TupleDesc tupdesc, const char *fname);
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
extern void SPI_freetuptable(SPITupleTable *tuptable);

extern Portal SPI_cursor_open(const char *name, void *plan,
				Datum *Values, const char *Nulls);
extern Portal SPI_cursor_find(const char *name);
extern void SPI_cursor_fetch(Portal portal, bool forward, int count);
extern void SPI_cursor_move(Portal portal, bool forward, int count);
extern void SPI_cursor_close(Portal portal);

extern void AtEOXact_SPI(void);

#endif   /* SPI_H */
