/*-------------------------------------------------------------------------
 *
 * nodes.h
 *	  Definitions for tagged nodes.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodes.h,v 1.64 2000/02/15 20:49:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODES_H
#define NODES_H

/*
 * The first field of every node is NodeTag. Each node created (with makeNode)
 * will have one of the following tags as the value of its first field.
 *
 * Note that the number of the node tags are not contiguous. We left holes
 * here so that we can add more tags without changing the existing enum's.
 */
typedef enum NodeTag
{
	T_Invalid = 0,

	/*---------------------
	 * TAGS FOR PLAN NODES (plannodes.h)
	 *---------------------
	 */
	T_Plan = 10,
	T_Result,
	T_Append,
	T_Scan,
	T_SeqScan,
	T_IndexScan,
	T_Join,
	T_NestLoop,
	T_MergeJoin,
	T_HashJoin,
	T_Noname,
	T_Material,
	T_Sort,
	T_Agg,
	T_Unique,
	T_Hash,
	T_Choose,
	T_Group,
	T_SubPlan,
	T_TidScan,

	/*---------------------
	 * TAGS FOR PRIMITIVE NODES (primnodes.h)
	 *---------------------
	 */
	T_Resdom = 100,
	T_Fjoin,
	T_Expr,
	T_Var,
	T_Oper,
	T_Const,
	T_Param,
	T_Aggref,
	T_SubLink,
	T_Func,
	T_Array,
	T_ArrayRef,
	T_Iter,

	/*---------------------
	 * TAGS FOR PLANNER NODES (relation.h)
	 *---------------------
	 */
	T_RelOptInfo = 200,
	T_Path,
	T_IndexPath,
	T_NestPath,
	T_MergePath,
	T_HashPath,
	T_PathKeyItem,
	T_RestrictInfo,
	T_JoinInfo,
	T_Stream,
	T_TidPath,
	T_IndexOptInfo,

	/*---------------------
	 * TAGS FOR EXECUTOR NODES (execnodes.h)
	 *---------------------
	 */
	T_IndexInfo = 300,
	T_RelationInfo,
	T_TupleCount,
	T_TupleTableSlot,
	T_ExprContext,
	T_ProjectionInfo,
	T_JunkFilter,
	T_EState,
	T_BaseNode,
	T_CommonState,
	T_ResultState,
	T_AppendState,
	T_CommonScanState,
	T_ScanState,
	T_IndexScanState,
	T_JoinState,
	T_NestLoopState,
	T_MergeJoinState,
	T_HashJoinState,
	T_MaterialState,
	T_AggState,
	T_GroupState,
	T_SortState,
	T_UniqueState,
	T_HashState,
	T_TidScanState,

	/*---------------------
	 * TAGS FOR MEMORY NODES (memnodes.h)
	 *---------------------
	 */
	T_MemoryContext = 400,
	T_GlobalMemory,
	T_PortalMemoryContext,
	T_PortalVariableMemory,
	T_PortalHeapMemory,

	/*---------------------
	 * TAGS FOR VALUE NODES (pg_list.h)
	 *---------------------
	 */
	T_Value = 500,
	T_List,
	T_Integer,
	T_Float,
	T_String,
	T_Null,

	/*---------------------
	 * TAGS FOR PARSE TREE NODES (parsenode.h)
	 *---------------------
	 */
	T_Query = 600,
	T_InsertStmt,
	T_DeleteStmt,
	T_UpdateStmt,
	T_SelectStmt,
    T_AlterTableStmt,
	T_AggregateStmt,
	T_ChangeACLStmt,
	T_ClosePortalStmt,
	T_ClusterStmt,
	T_CopyStmt,
	T_CreateStmt,
	T_VersionStmt,
	T_DefineStmt,
	T_DropStmt,
	T_TruncateStmt,
	T_CommentStmt,
	T_ExtendStmt,
	T_FetchStmt,
	T_IndexStmt,
	T_ProcedureStmt,
	T_RemoveAggrStmt,
	T_RemoveFuncStmt,
	T_RemoveOperStmt,
	T_RemoveStmt,
	T_RenameStmt,
	T_RuleStmt,
	T_NotifyStmt,
	T_ListenStmt,
	T_UnlistenStmt,
	T_TransactionStmt,
	T_ViewStmt,
	T_LoadStmt,
	T_CreatedbStmt,
	T_DropdbStmt,
	T_VacuumStmt,
	T_ExplainStmt,
	T_CreateSeqStmt,
	T_VariableSetStmt,
	T_VariableShowStmt,
	T_VariableResetStmt,
	T_CreateTrigStmt,
	T_DropTrigStmt,
	T_CreatePLangStmt,
	T_DropPLangStmt,
	T_CreateUserStmt,
	T_AlterUserStmt,
	T_DropUserStmt,
	T_LockStmt,
	T_ConstraintsSetStmt,
    T_CreateGroupStmt,
    T_AlterGroupStmt,
    T_DropGroupStmt,

	T_A_Expr = 700,
	T_Attr,
	T_A_Const,
	T_ParamNo,
	T_Ident,
	T_FuncCall,
	T_A_Indices,
	T_ResTarget,
	T_TypeCast,
	T_RelExpr,
	T_SortGroupBy,
	T_RangeVar,
	T_TypeName,
	T_IndexElem,
	T_ColumnDef,
	T_Constraint,
	T_DefElem,
	T_TargetEntry,
	T_RangeTblEntry,
	T_SortClause,
	T_GroupClause,
	T_SubSelect,
	T_JoinExpr,
	T_CaseExpr,
	T_CaseWhen,
	T_RowMark,
	T_FkConstraint
} NodeTag;

/*
 * The first field of a node of any type is guaranteed to be the NodeTag.
 * Hence the type of any node can be gotten by casting it to Node. Declaring
 * a variable to be of Node * (instead of void *) can also facilitate
 * debugging.
 */
typedef struct Node
{
	NodeTag		type;
} Node;

#define nodeTag(nodeptr)		(((Node*)(nodeptr))->type)

#define makeNode(_type_)		((_type_ *) newNode(sizeof(_type_),T_##_type_))
#define NodeSetTag(nodeptr,t)	(((Node*)(nodeptr))->type = (t))

#define IsA(nodeptr,_type_)		(nodeTag(nodeptr) == T_##_type_)

/* ----------------------------------------------------------------
 *					  IsA functions (no inheritance any more)
 * ----------------------------------------------------------------
 */
#define IsA_JoinPath(jp) \
	(IsA(jp, NestPath) || IsA(jp, MergePath) || IsA(jp, HashPath))

#define IsA_Join(jp) \
	(IsA(jp, Join) || IsA(jp, NestLoop) || \
	 IsA(jp, MergeJoin) || IsA(jp, HashJoin))

#define IsA_Noname(t) \
	(IsA(t, Noname) || IsA(t, Material) || IsA(t, Sort) || \
	 IsA(t, Unique))

#define IsA_Value(t) \
	(IsA(t, Integer) || IsA(t, Float) || IsA(t, String))

/* ----------------------------------------------------------------
 *					  extern declarations follow
 * ----------------------------------------------------------------
 */

/*
 * nodes/nodes.c
 */
extern Node *newNode(Size size, NodeTag tag);

/*
 * nodes/{outfuncs.c,print.c}
 */
extern char *nodeToString(void *obj);

/*
 * nodes/{readfuncs.c,read.c}
 */
extern void *stringToNode(char *str);

/*
 * nodes/copyfuncs.c
 */
extern void *copyObject(void *obj);

/*
 * nodes/equalfuncs.c
 */
extern bool equal(void *a, void *b);


/*
 * Typedefs for identifying qualifier selectivities and plan costs as such.
 * These are just plain "double"s, but declaring a variable as Selectivity
 * or Cost makes the intent more obvious.
 *
 * These could have gone into plannodes.h or some such, but many files
 * depend on them...
 */
typedef double Selectivity;		/* fraction of tuples a qualifier will pass */
typedef double Cost;			/* execution cost (in page-access units) */


/*
 * CmdType -
 *	  enums for type of operation to aid debugging
 *
 * ??? could have put this in parsenodes.h but many files not in the
 *	  optimizer also need this...
 */
typedef enum CmdType
{
	CMD_UNKNOWN,
	CMD_SELECT,					/* select stmt (formerly retrieve) */
	CMD_UPDATE,					/* update stmt (formerly replace) */
	CMD_INSERT,					/* insert stmt (formerly append) */
	CMD_DELETE,
	CMD_UTILITY,				/* cmds like create, destroy, copy,
								 * vacuum, etc. */
	CMD_NOTHING					/* dummy command for instead nothing rules
								 * with qual */
} CmdType;


#endif	 /* NODES_H */
