/*-------------------------------------------------------------------------
 *
 * spgist.h
 *	  Public header file for SP-GiST access method.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/spgist.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPGIST_H
#define SPGIST_H

#include "access/amapi.h"
#include "access/xlogreader.h"
#include "fmgr.h"
#include "lib/stringinfo.h"


/* reloption parameters */
#define SPGIST_MIN_FILLFACTOR			10
#define SPGIST_DEFAULT_FILLFACTOR		80

/* SPGiST opclass support function numbers */
#define SPGIST_CONFIG_PROC				1
#define SPGIST_CHOOSE_PROC				2
#define SPGIST_PICKSPLIT_PROC			3
#define SPGIST_INNER_CONSISTENT_PROC	4
#define SPGIST_LEAF_CONSISTENT_PROC		5
#define SPGIST_COMPRESS_PROC			6
#define SPGISTNRequiredProc				5
#define SPGISTNProc						6

/*
 * Argument structs for spg_config method
 */
typedef struct spgConfigIn
{
	Oid			attType;		/* Data type to be indexed */
} spgConfigIn;

typedef struct spgConfigOut
{
	Oid			prefixType;		/* Data type of inner-tuple prefixes */
	Oid			labelType;		/* Data type of inner-tuple node labels */
	Oid			leafType;		/* Data type of leaf-tuple values */
	bool		canReturnData;	/* Opclass can reconstruct original data */
	bool		longValuesOK;	/* Opclass can cope with values > 1 page */
} spgConfigOut;

/*
 * Argument structs for spg_choose method
 */
typedef struct spgChooseIn
{
	Datum		datum;			/* original datum to be indexed */
	Datum		leafDatum;		/* current datum to be stored at leaf */
	int			level;			/* current level (counting from zero) */

	/* Data from current inner tuple */
	bool		allTheSame;		/* tuple is marked all-the-same? */
	bool		hasPrefix;		/* tuple has a prefix? */
	Datum		prefixDatum;	/* if so, the prefix value */
	int			nNodes;			/* number of nodes in the inner tuple */
	Datum	   *nodeLabels;		/* node label values (NULL if none) */
} spgChooseIn;

typedef enum spgChooseResultType
{
	spgMatchNode = 1,			/* descend into existing node */
	spgAddNode,					/* add a node to the inner tuple */
	spgSplitTuple				/* split inner tuple (change its prefix) */
} spgChooseResultType;

typedef struct spgChooseOut
{
	spgChooseResultType resultType; /* action code, see above */
	union
	{
		struct					/* results for spgMatchNode */
		{
			int			nodeN;	/* descend to this node (index from 0) */
			int			levelAdd;	/* increment level by this much */
			Datum		restDatum;	/* new leaf datum */
		}			matchNode;
		struct					/* results for spgAddNode */
		{
			Datum		nodeLabel;	/* new node's label */
			int			nodeN;	/* where to insert it (index from 0) */
		}			addNode;
		struct					/* results for spgSplitTuple */
		{
			/* Info to form new upper-level inner tuple with one child tuple */
			bool		prefixHasPrefix;	/* tuple should have a prefix? */
			Datum		prefixPrefixDatum;	/* if so, its value */
			int			prefixNNodes;	/* number of nodes */
			Datum	   *prefixNodeLabels;	/* their labels (or NULL for no
											 * labels) */
			int			childNodeN; /* which node gets child tuple */

			/* Info to form new lower-level inner tuple with all old nodes */
			bool		postfixHasPrefix;	/* tuple should have a prefix? */
			Datum		postfixPrefixDatum; /* if so, its value */
		}			splitTuple;
	}			result;
} spgChooseOut;

/*
 * Argument structs for spg_picksplit method
 */
typedef struct spgPickSplitIn
{
	int			nTuples;		/* number of leaf tuples */
	Datum	   *datums;			/* their datums (array of length nTuples) */
	int			level;			/* current level (counting from zero) */
} spgPickSplitIn;

typedef struct spgPickSplitOut
{
	bool		hasPrefix;		/* new inner tuple should have a prefix? */
	Datum		prefixDatum;	/* if so, its value */

	int			nNodes;			/* number of nodes for new inner tuple */
	Datum	   *nodeLabels;		/* their labels (or NULL for no labels) */

	int		   *mapTuplesToNodes;	/* node index for each leaf tuple */
	Datum	   *leafTupleDatums;	/* datum to store in each new leaf tuple */
} spgPickSplitOut;

/*
 * Argument structs for spg_inner_consistent method
 */
typedef struct spgInnerConsistentIn
{
	ScanKey		scankeys;		/* array of operators and comparison values */
	ScanKey		orderbys;		/* array of ordering operators and comparison
								 * values */
	int			nkeys;			/* length of scankeys array */
	int			norderbys;		/* length of orderbys array */

	Datum		reconstructedValue; /* value reconstructed at parent */
	void	   *traversalValue; /* opclass-specific traverse value */
	MemoryContext traversalMemoryContext;	/* put new traverse values here */
	int			level;			/* current level (counting from zero) */
	bool		returnData;		/* original data must be returned? */

	/* Data from current inner tuple */
	bool		allTheSame;		/* tuple is marked all-the-same? */
	bool		hasPrefix;		/* tuple has a prefix? */
	Datum		prefixDatum;	/* if so, the prefix value */
	int			nNodes;			/* number of nodes in the inner tuple */
	Datum	   *nodeLabels;		/* node label values (NULL if none) */
} spgInnerConsistentIn;

typedef struct spgInnerConsistentOut
{
	int			nNodes;			/* number of child nodes to be visited */
	int		   *nodeNumbers;	/* their indexes in the node array */
	int		   *levelAdds;		/* increment level by this much for each */
	Datum	   *reconstructedValues;	/* associated reconstructed values */
	void	  **traversalValues;	/* opclass-specific traverse values */
	double	  **distances;		/* associated distances */
} spgInnerConsistentOut;

/*
 * Argument structs for spg_leaf_consistent method
 */
typedef struct spgLeafConsistentIn
{
	ScanKey		scankeys;		/* array of operators and comparison values */
	ScanKey		orderbys;		/* array of ordering operators and comparison
								 * values */
	int			nkeys;			/* length of scankeys array */
	int			norderbys;		/* length of orderbys array */

	Datum		reconstructedValue; /* value reconstructed at parent */
	void	   *traversalValue; /* opclass-specific traverse value */
	int			level;			/* current level (counting from zero) */
	bool		returnData;		/* original data must be returned? */

	Datum		leafDatum;		/* datum in leaf tuple */
} spgLeafConsistentIn;

typedef struct spgLeafConsistentOut
{
	Datum		leafValue;		/* reconstructed original data, if any */
	bool		recheck;		/* set true if operator must be rechecked */
	bool		recheckDistances;	/* set true if distances must be rechecked */
	double	   *distances;		/* associated distances */
} spgLeafConsistentOut;


/* spgutils.c */
extern bytea *spgoptions(Datum reloptions, bool validate);

/* spginsert.c */
extern IndexBuildResult *spgbuild(Relation heap, Relation index,
								  struct IndexInfo *indexInfo);
extern void spgbuildempty(Relation index);
extern bool spginsert(Relation index, Datum *values, bool *isnull,
					  ItemPointer ht_ctid, Relation heapRel,
					  IndexUniqueCheck checkUnique,
					  struct IndexInfo *indexInfo);

/* spgscan.c */
extern IndexScanDesc spgbeginscan(Relation rel, int keysz, int orderbysz);
extern void spgendscan(IndexScanDesc scan);
extern void spgrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					  ScanKey orderbys, int norderbys);
extern int64 spggetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool spggettuple(IndexScanDesc scan, ScanDirection dir);
extern bool spgcanreturn(Relation index, int attno);

/* spgvacuum.c */
extern IndexBulkDeleteResult *spgbulkdelete(IndexVacuumInfo *info,
											IndexBulkDeleteResult *stats,
											IndexBulkDeleteCallback callback,
											void *callback_state);
extern IndexBulkDeleteResult *spgvacuumcleanup(IndexVacuumInfo *info,
											   IndexBulkDeleteResult *stats);

/* spgvalidate.c */
extern bool spgvalidate(Oid opclassoid);

#endif							/* SPGIST_H */
