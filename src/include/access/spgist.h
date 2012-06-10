/*-------------------------------------------------------------------------
 *
 * spgist.h
 *	  Public header file for SP-GiST access method.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/spgist.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPGIST_H
#define SPGIST_H

#include "access/skey.h"
#include "access/xlog.h"
#include "fmgr.h"


/* reloption parameters */
#define SPGIST_MIN_FILLFACTOR			10
#define SPGIST_DEFAULT_FILLFACTOR		80

/* SPGiST opclass support function numbers */
#define SPGIST_CONFIG_PROC				1
#define SPGIST_CHOOSE_PROC				2
#define SPGIST_PICKSPLIT_PROC			3
#define SPGIST_INNER_CONSISTENT_PROC	4
#define SPGIST_LEAF_CONSISTENT_PROC		5
#define SPGISTNProc						5

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
	spgChooseResultType resultType;		/* action code, see above */
	union
	{
		struct					/* results for spgMatchNode */
		{
			int			nodeN;	/* descend to this node (index from 0) */
			int			levelAdd;		/* increment level by this much */
			Datum		restDatum;		/* new leaf datum */
		}			matchNode;
		struct					/* results for spgAddNode */
		{
			Datum		nodeLabel;		/* new node's label */
			int			nodeN;	/* where to insert it (index from 0) */
		}			addNode;
		struct					/* results for spgSplitTuple */
		{
			/* Info to form new inner tuple with one node */
			bool		prefixHasPrefix;		/* tuple should have a prefix? */
			Datum		prefixPrefixDatum;		/* if so, its value */
			Datum		nodeLabel;		/* node's label */

			/* Info to form new lower-level inner tuple with all old nodes */
			bool		postfixHasPrefix;		/* tuple should have a prefix? */
			Datum		postfixPrefixDatum;		/* if so, its value */
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

	int		   *mapTuplesToNodes;		/* node index for each leaf tuple */
	Datum	   *leafTupleDatums;	/* datum to store in each new leaf tuple */
} spgPickSplitOut;

/*
 * Argument structs for spg_inner_consistent method
 */
typedef struct spgInnerConsistentIn
{
	ScanKey		scankeys;		/* array of operators and comparison values */
	int			nkeys;			/* length of array */

	Datum		reconstructedValue;		/* value reconstructed at parent */
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
} spgInnerConsistentOut;

/*
 * Argument structs for spg_leaf_consistent method
 */
typedef struct spgLeafConsistentIn
{
	ScanKey		scankeys;		/* array of operators and comparison values */
	int			nkeys;			/* length of array */

	Datum		reconstructedValue;		/* value reconstructed at parent */
	int			level;			/* current level (counting from zero) */
	bool		returnData;		/* original data must be returned? */

	Datum		leafDatum;		/* datum in leaf tuple */
} spgLeafConsistentIn;

typedef struct spgLeafConsistentOut
{
	Datum		leafValue;		/* reconstructed original data, if any */
	bool		recheck;		/* set true if operator must be rechecked */
} spgLeafConsistentOut;


/* spginsert.c */
extern Datum spgbuild(PG_FUNCTION_ARGS);
extern Datum spgbuildempty(PG_FUNCTION_ARGS);
extern Datum spginsert(PG_FUNCTION_ARGS);

/* spgscan.c */
extern Datum spgbeginscan(PG_FUNCTION_ARGS);
extern Datum spgendscan(PG_FUNCTION_ARGS);
extern Datum spgrescan(PG_FUNCTION_ARGS);
extern Datum spgmarkpos(PG_FUNCTION_ARGS);
extern Datum spgrestrpos(PG_FUNCTION_ARGS);
extern Datum spggetbitmap(PG_FUNCTION_ARGS);
extern Datum spggettuple(PG_FUNCTION_ARGS);
extern Datum spgcanreturn(PG_FUNCTION_ARGS);

/* spgutils.c */
extern Datum spgoptions(PG_FUNCTION_ARGS);

/* spgvacuum.c */
extern Datum spgbulkdelete(PG_FUNCTION_ARGS);
extern Datum spgvacuumcleanup(PG_FUNCTION_ARGS);

/* spgxlog.c */
extern void spg_redo(XLogRecPtr lsn, XLogRecord *record);
extern void spg_desc(StringInfo buf, uint8 xl_info, char *rec);
extern void spg_xlog_startup(void);
extern void spg_xlog_cleanup(void);

#endif   /* SPGIST_H */
