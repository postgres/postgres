/*-------------------------------------------------------------------------
 *
 * ruleutils.c
 *	  Functions to convert stored expressions/querytrees back to
 *	  source text
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/ruleutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_language.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "common/keywords.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_agg.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSupport.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/varlena.h"
#include "utils/xml.h"

/* ----------
 * Pretty formatting constants
 * ----------
 */

/* Indent counts */
#define PRETTYINDENT_STD		8
#define PRETTYINDENT_JOIN		4
#define PRETTYINDENT_VAR		4

#define PRETTYINDENT_LIMIT		40	/* wrap limit */

/* Pretty flags */
#define PRETTYFLAG_PAREN		0x0001
#define PRETTYFLAG_INDENT		0x0002
#define PRETTYFLAG_SCHEMA		0x0004

/* Standard conversion of a "bool pretty" option to detailed flags */
#define GET_PRETTY_FLAGS(pretty) \
	((pretty) ? (PRETTYFLAG_PAREN | PRETTYFLAG_INDENT | PRETTYFLAG_SCHEMA) \
	 : PRETTYFLAG_INDENT)

/* Default line length for pretty-print wrapping: 0 means wrap always */
#define WRAP_COLUMN_DEFAULT		0

/* macros to test if pretty action needed */
#define PRETTY_PAREN(context)	((context)->prettyFlags & PRETTYFLAG_PAREN)
#define PRETTY_INDENT(context)	((context)->prettyFlags & PRETTYFLAG_INDENT)
#define PRETTY_SCHEMA(context)	((context)->prettyFlags & PRETTYFLAG_SCHEMA)


/* ----------
 * Local data types
 * ----------
 */

/* Context info needed for invoking a recursive querytree display routine */
typedef struct
{
	StringInfo	buf;			/* output buffer to append to */
	List	   *namespaces;		/* List of deparse_namespace nodes */
	TupleDesc	resultDesc;		/* if top level of a view, the view's tupdesc */
	List	   *targetList;		/* Current query level's SELECT targetlist */
	List	   *windowClause;	/* Current query level's WINDOW clause */
	int			prettyFlags;	/* enabling of pretty-print functions */
	int			wrapColumn;		/* max line length, or -1 for no limit */
	int			indentLevel;	/* current indent level for pretty-print */
	bool		varprefix;		/* true to print prefixes on Vars */
	bool		colNamesVisible;	/* do we care about output column names? */
	bool		inGroupBy;		/* deparsing GROUP BY clause? */
	bool		varInOrderBy;	/* deparsing simple Var in ORDER BY? */
	Bitmapset  *appendparents;	/* if not null, map child Vars of these relids
								 * back to the parent rel */
} deparse_context;

/*
 * Each level of query context around a subtree needs a level of Var namespace.
 * A Var having varlevelsup=N refers to the N'th item (counting from 0) in
 * the current context's namespaces list.
 *
 * rtable is the list of actual RTEs from the Query or PlannedStmt.
 * rtable_names holds the alias name to be used for each RTE (either a C
 * string, or NULL for nameless RTEs such as unnamed joins).
 * rtable_columns holds the column alias names to be used for each RTE.
 *
 * subplans is a list of Plan trees for SubPlans and CTEs (it's only used
 * in the PlannedStmt case).
 * ctes is a list of CommonTableExpr nodes (only used in the Query case).
 * appendrels, if not null (it's only used in the PlannedStmt case), is an
 * array of AppendRelInfo nodes, indexed by child relid.  We use that to map
 * child-table Vars to their inheritance parents.
 *
 * In some cases we need to make names of merged JOIN USING columns unique
 * across the whole query, not only per-RTE.  If so, unique_using is true
 * and using_names is a list of C strings representing names already assigned
 * to USING columns.
 *
 * When deparsing plan trees, there is always just a single item in the
 * deparse_namespace list (since a plan tree never contains Vars with
 * varlevelsup > 0).  We store the Plan node that is the immediate
 * parent of the expression to be deparsed, as well as a list of that
 * Plan's ancestors.  In addition, we store its outer and inner subplan nodes,
 * as well as their targetlists, and the index tlist if the current plan node
 * might contain INDEX_VAR Vars.  (These fields could be derived on-the-fly
 * from the current Plan node, but it seems notationally clearer to set them
 * up as separate fields.)
 */
typedef struct
{
	List	   *rtable;			/* List of RangeTblEntry nodes */
	List	   *rtable_names;	/* Parallel list of names for RTEs */
	List	   *rtable_columns; /* Parallel list of deparse_columns structs */
	List	   *subplans;		/* List of Plan trees for SubPlans */
	List	   *ctes;			/* List of CommonTableExpr nodes */
	AppendRelInfo **appendrels; /* Array of AppendRelInfo nodes, or NULL */
	char	   *ret_old_alias;	/* alias for OLD in RETURNING list */
	char	   *ret_new_alias;	/* alias for NEW in RETURNING list */
	/* Workspace for column alias assignment: */
	bool		unique_using;	/* Are we making USING names globally unique */
	List	   *using_names;	/* List of assigned names for USING columns */
	/* Remaining fields are used only when deparsing a Plan tree: */
	Plan	   *plan;			/* immediate parent of current expression */
	List	   *ancestors;		/* ancestors of plan */
	Plan	   *outer_plan;		/* outer subnode, or NULL if none */
	Plan	   *inner_plan;		/* inner subnode, or NULL if none */
	List	   *outer_tlist;	/* referent for OUTER_VAR Vars */
	List	   *inner_tlist;	/* referent for INNER_VAR Vars */
	List	   *index_tlist;	/* referent for INDEX_VAR Vars */
	/* Special namespace representing a function signature: */
	char	   *funcname;
	int			numargs;
	char	  **argnames;
} deparse_namespace;

/*
 * Per-relation data about column alias names.
 *
 * Selecting aliases is unreasonably complicated because of the need to dump
 * rules/views whose underlying tables may have had columns added, deleted, or
 * renamed since the query was parsed.  We must nonetheless print the rule/view
 * in a form that can be reloaded and will produce the same results as before.
 *
 * For each RTE used in the query, we must assign column aliases that are
 * unique within that RTE.  SQL does not require this of the original query,
 * but due to factors such as *-expansion we need to be able to uniquely
 * reference every column in a decompiled query.  As long as we qualify all
 * column references, per-RTE uniqueness is sufficient for that.
 *
 * However, we can't ensure per-column name uniqueness for unnamed join RTEs,
 * since they just inherit column names from their input RTEs, and we can't
 * rename the columns at the join level.  Most of the time this isn't an issue
 * because we don't need to reference the join's output columns as such; we
 * can reference the input columns instead.  That approach can fail for merged
 * JOIN USING columns, however, so when we have one of those in an unnamed
 * join, we have to make that column's alias globally unique across the whole
 * query to ensure it can be referenced unambiguously.
 *
 * Another problem is that a JOIN USING clause requires the columns to be
 * merged to have the same aliases in both input RTEs, and that no other
 * columns in those RTEs or their children conflict with the USING names.
 * To handle that, we do USING-column alias assignment in a recursive
 * traversal of the query's jointree.  When descending through a JOIN with
 * USING, we preassign the USING column names to the child columns, overriding
 * other rules for column alias assignment.  We also mark each RTE with a list
 * of all USING column names selected for joins containing that RTE, so that
 * when we assign other columns' aliases later, we can avoid conflicts.
 *
 * Another problem is that if a JOIN's input tables have had columns added or
 * deleted since the query was parsed, we must generate a column alias list
 * for the join that matches the current set of input columns --- otherwise, a
 * change in the number of columns in the left input would throw off matching
 * of aliases to columns of the right input.  Thus, positions in the printable
 * column alias list are not necessarily one-for-one with varattnos of the
 * JOIN, so we need a separate new_colnames[] array for printing purposes.
 *
 * Finally, when dealing with wide tables we risk O(N^2) costs in assigning
 * non-duplicate column names.  We ameliorate that by using a hash table that
 * holds all the strings appearing in colnames, new_colnames, and parentUsing.
 */
typedef struct
{
	/*
	 * colnames is an array containing column aliases to use for columns that
	 * existed when the query was parsed.  Dropped columns have NULL entries.
	 * This array can be directly indexed by varattno to get a Var's name.
	 *
	 * Non-NULL entries are guaranteed unique within the RTE, *except* when
	 * this is for an unnamed JOIN RTE.  In that case we merely copy up names
	 * from the two input RTEs.
	 *
	 * During the recursive descent in set_using_names(), forcible assignment
	 * of a child RTE's column name is represented by pre-setting that element
	 * of the child's colnames array.  So at that stage, NULL entries in this
	 * array just mean that no name has been preassigned, not necessarily that
	 * the column is dropped.
	 */
	int			num_cols;		/* length of colnames[] array */
	char	  **colnames;		/* array of C strings and NULLs */

	/*
	 * new_colnames is an array containing column aliases to use for columns
	 * that would exist if the query was re-parsed against the current
	 * definitions of its base tables.  This is what to print as the column
	 * alias list for the RTE.  This array does not include dropped columns,
	 * but it will include columns added since original parsing.  Indexes in
	 * it therefore have little to do with current varattno values.  As above,
	 * entries are unique unless this is for an unnamed JOIN RTE.  (In such an
	 * RTE, we never actually print this array, but we must compute it anyway
	 * for possible use in computing column names of upper joins.) The
	 * parallel array is_new_col marks which of these columns are new since
	 * original parsing.  Entries with is_new_col false must match the
	 * non-NULL colnames entries one-for-one.
	 */
	int			num_new_cols;	/* length of new_colnames[] array */
	char	  **new_colnames;	/* array of C strings */
	bool	   *is_new_col;		/* array of bool flags */

	/* This flag tells whether we should actually print a column alias list */
	bool		printaliases;

	/* This list has all names used as USING names in joins above this RTE */
	List	   *parentUsing;	/* names assigned to parent merged columns */

	/*
	 * If this struct is for a JOIN RTE, we fill these fields during the
	 * set_using_names() pass to describe its relationship to its child RTEs.
	 *
	 * leftattnos and rightattnos are arrays with one entry per existing
	 * output column of the join (hence, indexable by join varattno).  For a
	 * simple reference to a column of the left child, leftattnos[i] is the
	 * child RTE's attno and rightattnos[i] is zero; and conversely for a
	 * column of the right child.  But for merged columns produced by JOIN
	 * USING/NATURAL JOIN, both leftattnos[i] and rightattnos[i] are nonzero.
	 * Note that a simple reference might be to a child RTE column that's been
	 * dropped; but that's OK since the column could not be used in the query.
	 *
	 * If it's a JOIN USING, usingNames holds the alias names selected for the
	 * merged columns (these might be different from the original USING list,
	 * if we had to modify names to achieve uniqueness).
	 */
	int			leftrti;		/* rangetable index of left child */
	int			rightrti;		/* rangetable index of right child */
	int		   *leftattnos;		/* left-child varattnos of join cols, or 0 */
	int		   *rightattnos;	/* right-child varattnos of join cols, or 0 */
	List	   *usingNames;		/* names assigned to merged columns */

	/*
	 * Hash table holding copies of all the strings appearing in this struct's
	 * colnames, new_colnames, and parentUsing.  We use a hash table only for
	 * sufficiently wide relations, and only during the colname-assignment
	 * functions set_relation_column_names and set_join_column_names;
	 * otherwise, names_hash is NULL.
	 */
	HTAB	   *names_hash;		/* entries are just strings */
} deparse_columns;

/* This macro is analogous to rt_fetch(), but for deparse_columns structs */
#define deparse_columns_fetch(rangetable_index, dpns) \
	((deparse_columns *) list_nth((dpns)->rtable_columns, (rangetable_index)-1))

/*
 * Entry in set_rtable_names' hash table
 */
typedef struct
{
	char		name[NAMEDATALEN];	/* Hash key --- must be first */
	int			counter;		/* Largest addition used so far for name */
} NameHashEntry;

/* Callback signature for resolve_special_varno() */
typedef void (*rsv_callback) (Node *node, deparse_context *context,
							  void *callback_arg);


/* ----------
 * Global data
 * ----------
 */
static SPIPlanPtr plan_getrulebyoid = NULL;
static const char *const query_getrulebyoid = "SELECT * FROM pg_catalog.pg_rewrite WHERE oid = $1";
static SPIPlanPtr plan_getviewrule = NULL;
static const char *const query_getviewrule = "SELECT * FROM pg_catalog.pg_rewrite WHERE ev_class = $1 AND rulename = $2";

/* GUC parameters */
bool		quote_all_identifiers = false;


/* ----------
 * Local functions
 *
 * Most of these functions used to use fixed-size buffers to build their
 * results.  Now, they take an (already initialized) StringInfo object
 * as a parameter, and append their text output to its contents.
 * ----------
 */
static char *deparse_expression_pretty(Node *expr, List *dpcontext,
									   bool forceprefix, bool showimplicit,
									   int prettyFlags, int startIndent);
static char *pg_get_viewdef_worker(Oid viewoid,
								   int prettyFlags, int wrapColumn);
static char *pg_get_triggerdef_worker(Oid trigid, bool pretty);
static int	decompile_column_index_array(Datum column_index_array, Oid relId,
										 bool withPeriod, StringInfo buf);
static char *pg_get_ruledef_worker(Oid ruleoid, int prettyFlags);
static char *pg_get_indexdef_worker(Oid indexrelid, int colno,
									const Oid *excludeOps,
									bool attrsOnly, bool keysOnly,
									bool showTblSpc, bool inherits,
									int prettyFlags, bool missing_ok);
static char *pg_get_statisticsobj_worker(Oid statextid, bool columns_only,
										 bool missing_ok);
static char *pg_get_partkeydef_worker(Oid relid, int prettyFlags,
									  bool attrsOnly, bool missing_ok);
static char *pg_get_constraintdef_worker(Oid constraintId, bool fullCommand,
										 int prettyFlags, bool missing_ok);
static text *pg_get_expr_worker(text *expr, Oid relid, int prettyFlags);
static int	print_function_arguments(StringInfo buf, HeapTuple proctup,
									 bool print_table_args, bool print_defaults);
static void print_function_rettype(StringInfo buf, HeapTuple proctup);
static void print_function_trftypes(StringInfo buf, HeapTuple proctup);
static void print_function_sqlbody(StringInfo buf, HeapTuple proctup);
static void set_rtable_names(deparse_namespace *dpns, List *parent_namespaces,
							 Bitmapset *rels_used);
static void set_deparse_for_query(deparse_namespace *dpns, Query *query,
								  List *parent_namespaces);
static void set_simple_column_names(deparse_namespace *dpns);
static bool has_dangerous_join_using(deparse_namespace *dpns, Node *jtnode);
static void set_using_names(deparse_namespace *dpns, Node *jtnode,
							List *parentUsing);
static void set_relation_column_names(deparse_namespace *dpns,
									  RangeTblEntry *rte,
									  deparse_columns *colinfo);
static void set_join_column_names(deparse_namespace *dpns, RangeTblEntry *rte,
								  deparse_columns *colinfo);
static bool colname_is_unique(const char *colname, deparse_namespace *dpns,
							  deparse_columns *colinfo);
static char *make_colname_unique(char *colname, deparse_namespace *dpns,
								 deparse_columns *colinfo);
static void expand_colnames_array_to(deparse_columns *colinfo, int n);
static void build_colinfo_names_hash(deparse_columns *colinfo);
static void add_to_names_hash(deparse_columns *colinfo, const char *name);
static void destroy_colinfo_names_hash(deparse_columns *colinfo);
static void identify_join_columns(JoinExpr *j, RangeTblEntry *jrte,
								  deparse_columns *colinfo);
static char *get_rtable_name(int rtindex, deparse_context *context);
static void set_deparse_plan(deparse_namespace *dpns, Plan *plan);
static Plan *find_recursive_union(deparse_namespace *dpns,
								  WorkTableScan *wtscan);
static void push_child_plan(deparse_namespace *dpns, Plan *plan,
							deparse_namespace *save_dpns);
static void pop_child_plan(deparse_namespace *dpns,
						   deparse_namespace *save_dpns);
static void push_ancestor_plan(deparse_namespace *dpns, ListCell *ancestor_cell,
							   deparse_namespace *save_dpns);
static void pop_ancestor_plan(deparse_namespace *dpns,
							  deparse_namespace *save_dpns);
static void make_ruledef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc,
						 int prettyFlags);
static void make_viewdef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc,
						 int prettyFlags, int wrapColumn);
static void get_query_def(Query *query, StringInfo buf, List *parentnamespace,
						  TupleDesc resultDesc, bool colNamesVisible,
						  int prettyFlags, int wrapColumn, int startIndent);
static void get_values_def(List *values_lists, deparse_context *context);
static void get_with_clause(Query *query, deparse_context *context);
static void get_select_query_def(Query *query, deparse_context *context);
static void get_insert_query_def(Query *query, deparse_context *context);
static void get_update_query_def(Query *query, deparse_context *context);
static void get_update_query_targetlist_def(Query *query, List *targetList,
											deparse_context *context,
											RangeTblEntry *rte);
static void get_delete_query_def(Query *query, deparse_context *context);
static void get_merge_query_def(Query *query, deparse_context *context);
static void get_utility_query_def(Query *query, deparse_context *context);
static void get_basic_select_query(Query *query, deparse_context *context);
static void get_target_list(List *targetList, deparse_context *context);
static void get_returning_clause(Query *query, deparse_context *context);
static void get_setop_query(Node *setOp, Query *query,
							deparse_context *context);
static Node *get_rule_sortgroupclause(Index ref, List *tlist,
									  bool force_colno,
									  deparse_context *context);
static void get_rule_groupingset(GroupingSet *gset, List *targetlist,
								 bool omit_parens, deparse_context *context);
static void get_rule_orderby(List *orderList, List *targetList,
							 bool force_colno, deparse_context *context);
static void get_rule_windowclause(Query *query, deparse_context *context);
static void get_rule_windowspec(WindowClause *wc, List *targetList,
								deparse_context *context);
static char *get_variable(Var *var, int levelsup, bool istoplevel,
						  deparse_context *context);
static void get_special_variable(Node *node, deparse_context *context,
								 void *callback_arg);
static void resolve_special_varno(Node *node, deparse_context *context,
								  rsv_callback callback, void *callback_arg);
static Node *find_param_referent(Param *param, deparse_context *context,
								 deparse_namespace **dpns_p, ListCell **ancestor_cell_p);
static SubPlan *find_param_generator(Param *param, deparse_context *context,
									 int *column_p);
static SubPlan *find_param_generator_initplan(Param *param, Plan *plan,
											  int *column_p);
static void get_parameter(Param *param, deparse_context *context);
static const char *get_simple_binary_op_name(OpExpr *expr);
static bool isSimpleNode(Node *node, Node *parentNode, int prettyFlags);
static void appendContextKeyword(deparse_context *context, const char *str,
								 int indentBefore, int indentAfter, int indentPlus);
static void removeStringInfoSpaces(StringInfo str);
static void get_rule_expr(Node *node, deparse_context *context,
						  bool showimplicit);
static void get_rule_expr_toplevel(Node *node, deparse_context *context,
								   bool showimplicit);
static void get_rule_list_toplevel(List *lst, deparse_context *context,
								   bool showimplicit);
static void get_rule_expr_funccall(Node *node, deparse_context *context,
								   bool showimplicit);
static bool looks_like_function(Node *node);
static void get_oper_expr(OpExpr *expr, deparse_context *context);
static void get_func_expr(FuncExpr *expr, deparse_context *context,
						  bool showimplicit);
static void get_agg_expr(Aggref *aggref, deparse_context *context,
						 Aggref *original_aggref);
static void get_agg_expr_helper(Aggref *aggref, deparse_context *context,
								Aggref *original_aggref, const char *funcname,
								const char *options, bool is_json_objectagg);
static void get_agg_combine_expr(Node *node, deparse_context *context,
								 void *callback_arg);
static void get_windowfunc_expr(WindowFunc *wfunc, deparse_context *context);
static void get_windowfunc_expr_helper(WindowFunc *wfunc, deparse_context *context,
									   const char *funcname, const char *options,
									   bool is_json_objectagg);
static bool get_func_sql_syntax(FuncExpr *expr, deparse_context *context);
static void get_coercion_expr(Node *arg, deparse_context *context,
							  Oid resulttype, int32 resulttypmod,
							  Node *parentNode);
static void get_const_expr(Const *constval, deparse_context *context,
						   int showtype);
static void get_const_collation(Const *constval, deparse_context *context);
static void get_json_format(JsonFormat *format, StringInfo buf);
static void get_json_returning(JsonReturning *returning, StringInfo buf,
							   bool json_format_by_default);
static void get_json_constructor(JsonConstructorExpr *ctor,
								 deparse_context *context, bool showimplicit);
static void get_json_constructor_options(JsonConstructorExpr *ctor,
										 StringInfo buf);
static void get_json_agg_constructor(JsonConstructorExpr *ctor,
									 deparse_context *context,
									 const char *funcname,
									 bool is_json_objectagg);
static void simple_quote_literal(StringInfo buf, const char *val);
static void get_sublink_expr(SubLink *sublink, deparse_context *context);
static void get_tablefunc(TableFunc *tf, deparse_context *context,
						  bool showimplicit);
static void get_from_clause(Query *query, const char *prefix,
							deparse_context *context);
static void get_from_clause_item(Node *jtnode, Query *query,
								 deparse_context *context);
static void get_rte_alias(RangeTblEntry *rte, int varno, bool use_as,
						  deparse_context *context);
static void get_column_alias_list(deparse_columns *colinfo,
								  deparse_context *context);
static void get_from_clause_coldeflist(RangeTblFunction *rtfunc,
									   deparse_columns *colinfo,
									   deparse_context *context);
static void get_tablesample_def(TableSampleClause *tablesample,
								deparse_context *context);
static void get_opclass_name(Oid opclass, Oid actual_datatype,
							 StringInfo buf);
static Node *processIndirection(Node *node, deparse_context *context);
static void printSubscripts(SubscriptingRef *sbsref, deparse_context *context);
static char *get_relation_name(Oid relid);
static char *generate_relation_name(Oid relid, List *namespaces);
static char *generate_qualified_relation_name(Oid relid);
static char *generate_function_name(Oid funcid, int nargs,
									List *argnames, Oid *argtypes,
									bool has_variadic, bool *use_variadic_p,
									bool inGroupBy);
static char *generate_operator_name(Oid operid, Oid arg1, Oid arg2);
static void add_cast_to(StringInfo buf, Oid typid);
static char *generate_qualified_type_name(Oid typid);
static text *string_to_text(char *str);
static char *flatten_reloptions(Oid relid);
static void get_reloptions(StringInfo buf, Datum reloptions);
static void get_json_path_spec(Node *path_spec, deparse_context *context,
							   bool showimplicit);
static void get_json_table_columns(TableFunc *tf, JsonTablePathScan *scan,
								   deparse_context *context,
								   bool showimplicit);
static void get_json_table_nested_columns(TableFunc *tf, JsonTablePlan *plan,
										  deparse_context *context,
										  bool showimplicit,
										  bool needcomma);

#define only_marker(rte)  ((rte)->inh ? "" : "ONLY ")


/* ----------
 * pg_get_ruledef		- Do it all and return a text
 *				  that could be used as a statement
 *				  to recreate the rule
 * ----------
 */
Datum
pg_get_ruledef(PG_FUNCTION_ARGS)
{
	Oid			ruleoid = PG_GETARG_OID(0);
	int			prettyFlags;
	char	   *res;

	prettyFlags = PRETTYFLAG_INDENT;

	res = pg_get_ruledef_worker(ruleoid, prettyFlags);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}


Datum
pg_get_ruledef_ext(PG_FUNCTION_ARGS)
{
	Oid			ruleoid = PG_GETARG_OID(0);
	bool		pretty = PG_GETARG_BOOL(1);
	int			prettyFlags;
	char	   *res;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	res = pg_get_ruledef_worker(ruleoid, prettyFlags);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}


static char *
pg_get_ruledef_worker(Oid ruleoid, int prettyFlags)
{
	Datum		args[1];
	char		nulls[1];
	int			spirc;
	HeapTuple	ruletup;
	TupleDesc	rulettc;
	StringInfoData buf;

	/*
	 * Do this first so that string is alloc'd in outer context not SPI's.
	 */
	initStringInfo(&buf);

	/*
	 * Connect to SPI manager
	 */
	SPI_connect();

	/*
	 * On the first call prepare the plan to lookup pg_rewrite. We read
	 * pg_rewrite over the SPI manager instead of using the syscache to be
	 * checked for read access on pg_rewrite.
	 */
	if (plan_getrulebyoid == NULL)
	{
		Oid			argtypes[1];
		SPIPlanPtr	plan;

		argtypes[0] = OIDOID;
		plan = SPI_prepare(query_getrulebyoid, 1, argtypes);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare failed for \"%s\"", query_getrulebyoid);
		SPI_keepplan(plan);
		plan_getrulebyoid = plan;
	}

	/*
	 * Get the pg_rewrite tuple for this rule
	 */
	args[0] = ObjectIdGetDatum(ruleoid);
	nulls[0] = ' ';
	spirc = SPI_execute_plan(plan_getrulebyoid, args, nulls, true, 0);
	if (spirc != SPI_OK_SELECT)
		elog(ERROR, "failed to get pg_rewrite tuple for rule %u", ruleoid);
	if (SPI_processed != 1)
	{
		/*
		 * There is no tuple data available here, just keep the output buffer
		 * empty.
		 */
	}
	else
	{
		/*
		 * Get the rule's definition and put it into executor's memory
		 */
		ruletup = SPI_tuptable->vals[0];
		rulettc = SPI_tuptable->tupdesc;
		make_ruledef(&buf, ruletup, rulettc, prettyFlags);
	}

	/*
	 * Disconnect from SPI manager
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	if (buf.len == 0)
		return NULL;

	return buf.data;
}


/* ----------
 * pg_get_viewdef		- Mainly the same thing, but we
 *				  only return the SELECT part of a view
 * ----------
 */
Datum
pg_get_viewdef(PG_FUNCTION_ARGS)
{
	/* By OID */
	Oid			viewoid = PG_GETARG_OID(0);
	int			prettyFlags;
	char	   *res;

	prettyFlags = PRETTYFLAG_INDENT;

	res = pg_get_viewdef_worker(viewoid, prettyFlags, WRAP_COLUMN_DEFAULT);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}


Datum
pg_get_viewdef_ext(PG_FUNCTION_ARGS)
{
	/* By OID */
	Oid			viewoid = PG_GETARG_OID(0);
	bool		pretty = PG_GETARG_BOOL(1);
	int			prettyFlags;
	char	   *res;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	res = pg_get_viewdef_worker(viewoid, prettyFlags, WRAP_COLUMN_DEFAULT);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

Datum
pg_get_viewdef_wrap(PG_FUNCTION_ARGS)
{
	/* By OID */
	Oid			viewoid = PG_GETARG_OID(0);
	int			wrap = PG_GETARG_INT32(1);
	int			prettyFlags;
	char	   *res;

	/* calling this implies we want pretty printing */
	prettyFlags = GET_PRETTY_FLAGS(true);

	res = pg_get_viewdef_worker(viewoid, prettyFlags, wrap);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

Datum
pg_get_viewdef_name(PG_FUNCTION_ARGS)
{
	/* By qualified name */
	text	   *viewname = PG_GETARG_TEXT_PP(0);
	int			prettyFlags;
	RangeVar   *viewrel;
	Oid			viewoid;
	char	   *res;

	prettyFlags = PRETTYFLAG_INDENT;

	/* Look up view name.  Can't lock it - we might not have privileges. */
	viewrel = makeRangeVarFromNameList(textToQualifiedNameList(viewname));
	viewoid = RangeVarGetRelid(viewrel, NoLock, false);

	res = pg_get_viewdef_worker(viewoid, prettyFlags, WRAP_COLUMN_DEFAULT);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}


Datum
pg_get_viewdef_name_ext(PG_FUNCTION_ARGS)
{
	/* By qualified name */
	text	   *viewname = PG_GETARG_TEXT_PP(0);
	bool		pretty = PG_GETARG_BOOL(1);
	int			prettyFlags;
	RangeVar   *viewrel;
	Oid			viewoid;
	char	   *res;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	/* Look up view name.  Can't lock it - we might not have privileges. */
	viewrel = makeRangeVarFromNameList(textToQualifiedNameList(viewname));
	viewoid = RangeVarGetRelid(viewrel, NoLock, false);

	res = pg_get_viewdef_worker(viewoid, prettyFlags, WRAP_COLUMN_DEFAULT);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

/*
 * Common code for by-OID and by-name variants of pg_get_viewdef
 */
static char *
pg_get_viewdef_worker(Oid viewoid, int prettyFlags, int wrapColumn)
{
	Datum		args[2];
	char		nulls[2];
	int			spirc;
	HeapTuple	ruletup;
	TupleDesc	rulettc;
	StringInfoData buf;

	/*
	 * Do this first so that string is alloc'd in outer context not SPI's.
	 */
	initStringInfo(&buf);

	/*
	 * Connect to SPI manager
	 */
	SPI_connect();

	/*
	 * On the first call prepare the plan to lookup pg_rewrite. We read
	 * pg_rewrite over the SPI manager instead of using the syscache to be
	 * checked for read access on pg_rewrite.
	 */
	if (plan_getviewrule == NULL)
	{
		Oid			argtypes[2];
		SPIPlanPtr	plan;

		argtypes[0] = OIDOID;
		argtypes[1] = NAMEOID;
		plan = SPI_prepare(query_getviewrule, 2, argtypes);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare failed for \"%s\"", query_getviewrule);
		SPI_keepplan(plan);
		plan_getviewrule = plan;
	}

	/*
	 * Get the pg_rewrite tuple for the view's SELECT rule
	 */
	args[0] = ObjectIdGetDatum(viewoid);
	args[1] = DirectFunctionCall1(namein, CStringGetDatum(ViewSelectRuleName));
	nulls[0] = ' ';
	nulls[1] = ' ';
	spirc = SPI_execute_plan(plan_getviewrule, args, nulls, true, 0);
	if (spirc != SPI_OK_SELECT)
		elog(ERROR, "failed to get pg_rewrite tuple for view %u", viewoid);
	if (SPI_processed != 1)
	{
		/*
		 * There is no tuple data available here, just keep the output buffer
		 * empty.
		 */
	}
	else
	{
		/*
		 * Get the rule's definition and put it into executor's memory
		 */
		ruletup = SPI_tuptable->vals[0];
		rulettc = SPI_tuptable->tupdesc;
		make_viewdef(&buf, ruletup, rulettc, prettyFlags, wrapColumn);
	}

	/*
	 * Disconnect from SPI manager
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	if (buf.len == 0)
		return NULL;

	return buf.data;
}

/* ----------
 * pg_get_triggerdef		- Get the definition of a trigger
 * ----------
 */
Datum
pg_get_triggerdef(PG_FUNCTION_ARGS)
{
	Oid			trigid = PG_GETARG_OID(0);
	char	   *res;

	res = pg_get_triggerdef_worker(trigid, false);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

Datum
pg_get_triggerdef_ext(PG_FUNCTION_ARGS)
{
	Oid			trigid = PG_GETARG_OID(0);
	bool		pretty = PG_GETARG_BOOL(1);
	char	   *res;

	res = pg_get_triggerdef_worker(trigid, pretty);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

static char *
pg_get_triggerdef_worker(Oid trigid, bool pretty)
{
	HeapTuple	ht_trig;
	Form_pg_trigger trigrec;
	StringInfoData buf;
	Relation	tgrel;
	ScanKeyData skey[1];
	SysScanDesc tgscan;
	int			findx = 0;
	char	   *tgname;
	char	   *tgoldtable;
	char	   *tgnewtable;
	Datum		value;
	bool		isnull;

	/*
	 * Fetch the pg_trigger tuple by the Oid of the trigger
	 */
	tgrel = table_open(TriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_trigger_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(trigid));

	tgscan = systable_beginscan(tgrel, TriggerOidIndexId, true,
								NULL, 1, skey);

	ht_trig = systable_getnext(tgscan);

	if (!HeapTupleIsValid(ht_trig))
	{
		systable_endscan(tgscan);
		table_close(tgrel, AccessShareLock);
		return NULL;
	}

	trigrec = (Form_pg_trigger) GETSTRUCT(ht_trig);

	/*
	 * Start the trigger definition. Note that the trigger's name should never
	 * be schema-qualified, but the trigger rel's name may be.
	 */
	initStringInfo(&buf);

	tgname = NameStr(trigrec->tgname);
	appendStringInfo(&buf, "CREATE %sTRIGGER %s ",
					 OidIsValid(trigrec->tgconstraint) ? "CONSTRAINT " : "",
					 quote_identifier(tgname));

	if (TRIGGER_FOR_BEFORE(trigrec->tgtype))
		appendStringInfoString(&buf, "BEFORE");
	else if (TRIGGER_FOR_AFTER(trigrec->tgtype))
		appendStringInfoString(&buf, "AFTER");
	else if (TRIGGER_FOR_INSTEAD(trigrec->tgtype))
		appendStringInfoString(&buf, "INSTEAD OF");
	else
		elog(ERROR, "unexpected tgtype value: %d", trigrec->tgtype);

	if (TRIGGER_FOR_INSERT(trigrec->tgtype))
	{
		appendStringInfoString(&buf, " INSERT");
		findx++;
	}
	if (TRIGGER_FOR_DELETE(trigrec->tgtype))
	{
		if (findx > 0)
			appendStringInfoString(&buf, " OR DELETE");
		else
			appendStringInfoString(&buf, " DELETE");
		findx++;
	}
	if (TRIGGER_FOR_UPDATE(trigrec->tgtype))
	{
		if (findx > 0)
			appendStringInfoString(&buf, " OR UPDATE");
		else
			appendStringInfoString(&buf, " UPDATE");
		findx++;
		/* tgattr is first var-width field, so OK to access directly */
		if (trigrec->tgattr.dim1 > 0)
		{
			int			i;

			appendStringInfoString(&buf, " OF ");
			for (i = 0; i < trigrec->tgattr.dim1; i++)
			{
				char	   *attname;

				if (i > 0)
					appendStringInfoString(&buf, ", ");
				attname = get_attname(trigrec->tgrelid,
									  trigrec->tgattr.values[i], false);
				appendStringInfoString(&buf, quote_identifier(attname));
			}
		}
	}
	if (TRIGGER_FOR_TRUNCATE(trigrec->tgtype))
	{
		if (findx > 0)
			appendStringInfoString(&buf, " OR TRUNCATE");
		else
			appendStringInfoString(&buf, " TRUNCATE");
		findx++;
	}

	/*
	 * In non-pretty mode, always schema-qualify the target table name for
	 * safety.  In pretty mode, schema-qualify only if not visible.
	 */
	appendStringInfo(&buf, " ON %s ",
					 pretty ?
					 generate_relation_name(trigrec->tgrelid, NIL) :
					 generate_qualified_relation_name(trigrec->tgrelid));

	if (OidIsValid(trigrec->tgconstraint))
	{
		if (OidIsValid(trigrec->tgconstrrelid))
			appendStringInfo(&buf, "FROM %s ",
							 generate_relation_name(trigrec->tgconstrrelid, NIL));
		if (!trigrec->tgdeferrable)
			appendStringInfoString(&buf, "NOT ");
		appendStringInfoString(&buf, "DEFERRABLE INITIALLY ");
		if (trigrec->tginitdeferred)
			appendStringInfoString(&buf, "DEFERRED ");
		else
			appendStringInfoString(&buf, "IMMEDIATE ");
	}

	value = fastgetattr(ht_trig, Anum_pg_trigger_tgoldtable,
						tgrel->rd_att, &isnull);
	if (!isnull)
		tgoldtable = NameStr(*DatumGetName(value));
	else
		tgoldtable = NULL;
	value = fastgetattr(ht_trig, Anum_pg_trigger_tgnewtable,
						tgrel->rd_att, &isnull);
	if (!isnull)
		tgnewtable = NameStr(*DatumGetName(value));
	else
		tgnewtable = NULL;
	if (tgoldtable != NULL || tgnewtable != NULL)
	{
		appendStringInfoString(&buf, "REFERENCING ");
		if (tgoldtable != NULL)
			appendStringInfo(&buf, "OLD TABLE AS %s ",
							 quote_identifier(tgoldtable));
		if (tgnewtable != NULL)
			appendStringInfo(&buf, "NEW TABLE AS %s ",
							 quote_identifier(tgnewtable));
	}

	if (TRIGGER_FOR_ROW(trigrec->tgtype))
		appendStringInfoString(&buf, "FOR EACH ROW ");
	else
		appendStringInfoString(&buf, "FOR EACH STATEMENT ");

	/* If the trigger has a WHEN qualification, add that */
	value = fastgetattr(ht_trig, Anum_pg_trigger_tgqual,
						tgrel->rd_att, &isnull);
	if (!isnull)
	{
		Node	   *qual;
		char		relkind;
		deparse_context context;
		deparse_namespace dpns;
		RangeTblEntry *oldrte;
		RangeTblEntry *newrte;

		appendStringInfoString(&buf, "WHEN (");

		qual = stringToNode(TextDatumGetCString(value));

		relkind = get_rel_relkind(trigrec->tgrelid);

		/* Build minimal OLD and NEW RTEs for the rel */
		oldrte = makeNode(RangeTblEntry);
		oldrte->rtekind = RTE_RELATION;
		oldrte->relid = trigrec->tgrelid;
		oldrte->relkind = relkind;
		oldrte->rellockmode = AccessShareLock;
		oldrte->alias = makeAlias("old", NIL);
		oldrte->eref = oldrte->alias;
		oldrte->lateral = false;
		oldrte->inh = false;
		oldrte->inFromCl = true;

		newrte = makeNode(RangeTblEntry);
		newrte->rtekind = RTE_RELATION;
		newrte->relid = trigrec->tgrelid;
		newrte->relkind = relkind;
		newrte->rellockmode = AccessShareLock;
		newrte->alias = makeAlias("new", NIL);
		newrte->eref = newrte->alias;
		newrte->lateral = false;
		newrte->inh = false;
		newrte->inFromCl = true;

		/* Build two-element rtable */
		memset(&dpns, 0, sizeof(dpns));
		dpns.rtable = list_make2(oldrte, newrte);
		dpns.subplans = NIL;
		dpns.ctes = NIL;
		dpns.appendrels = NULL;
		set_rtable_names(&dpns, NIL, NULL);
		set_simple_column_names(&dpns);

		/* Set up context with one-deep namespace stack */
		context.buf = &buf;
		context.namespaces = list_make1(&dpns);
		context.resultDesc = NULL;
		context.targetList = NIL;
		context.windowClause = NIL;
		context.varprefix = true;
		context.prettyFlags = GET_PRETTY_FLAGS(pretty);
		context.wrapColumn = WRAP_COLUMN_DEFAULT;
		context.indentLevel = PRETTYINDENT_STD;
		context.colNamesVisible = true;
		context.inGroupBy = false;
		context.varInOrderBy = false;
		context.appendparents = NULL;

		get_rule_expr(qual, &context, false);

		appendStringInfoString(&buf, ") ");
	}

	appendStringInfo(&buf, "EXECUTE FUNCTION %s(",
					 generate_function_name(trigrec->tgfoid, 0,
											NIL, NULL,
											false, NULL, false));

	if (trigrec->tgnargs > 0)
	{
		char	   *p;
		int			i;

		value = fastgetattr(ht_trig, Anum_pg_trigger_tgargs,
							tgrel->rd_att, &isnull);
		if (isnull)
			elog(ERROR, "tgargs is null for trigger %u", trigid);
		p = (char *) VARDATA_ANY(DatumGetByteaPP(value));
		for (i = 0; i < trigrec->tgnargs; i++)
		{
			if (i > 0)
				appendStringInfoString(&buf, ", ");
			simple_quote_literal(&buf, p);
			/* advance p to next string embedded in tgargs */
			while (*p)
				p++;
			p++;
		}
	}

	/* We deliberately do not put semi-colon at end */
	appendStringInfoChar(&buf, ')');

	/* Clean up */
	systable_endscan(tgscan);

	table_close(tgrel, AccessShareLock);

	return buf.data;
}

/* ----------
 * pg_get_indexdef			- Get the definition of an index
 *
 * In the extended version, there is a colno argument as well as pretty bool.
 *	if colno == 0, we want a complete index definition.
 *	if colno > 0, we only want the Nth index key's variable or expression.
 *
 * Note that the SQL-function versions of this omit any info about the
 * index tablespace; this is intentional because pg_dump wants it that way.
 * However pg_get_indexdef_string() includes the index tablespace.
 * ----------
 */
Datum
pg_get_indexdef(PG_FUNCTION_ARGS)
{
	Oid			indexrelid = PG_GETARG_OID(0);
	int			prettyFlags;
	char	   *res;

	prettyFlags = PRETTYFLAG_INDENT;

	res = pg_get_indexdef_worker(indexrelid, 0, NULL,
								 false, false,
								 false, false,
								 prettyFlags, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

Datum
pg_get_indexdef_ext(PG_FUNCTION_ARGS)
{
	Oid			indexrelid = PG_GETARG_OID(0);
	int32		colno = PG_GETARG_INT32(1);
	bool		pretty = PG_GETARG_BOOL(2);
	int			prettyFlags;
	char	   *res;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	res = pg_get_indexdef_worker(indexrelid, colno, NULL,
								 colno != 0, false,
								 false, false,
								 prettyFlags, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

/*
 * Internal version for use by ALTER TABLE.
 * Includes a tablespace clause in the result.
 * Returns a palloc'd C string; no pretty-printing.
 */
char *
pg_get_indexdef_string(Oid indexrelid)
{
	return pg_get_indexdef_worker(indexrelid, 0, NULL,
								  false, false,
								  true, true,
								  0, false);
}

/* Internal version that just reports the key-column definitions */
char *
pg_get_indexdef_columns(Oid indexrelid, bool pretty)
{
	int			prettyFlags;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	return pg_get_indexdef_worker(indexrelid, 0, NULL,
								  true, true,
								  false, false,
								  prettyFlags, false);
}

/* Internal version, extensible with flags to control its behavior */
char *
pg_get_indexdef_columns_extended(Oid indexrelid, bits16 flags)
{
	bool		pretty = ((flags & RULE_INDEXDEF_PRETTY) != 0);
	bool		keys_only = ((flags & RULE_INDEXDEF_KEYS_ONLY) != 0);
	int			prettyFlags;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	return pg_get_indexdef_worker(indexrelid, 0, NULL,
								  true, keys_only,
								  false, false,
								  prettyFlags, false);
}

/*
 * Internal workhorse to decompile an index definition.
 *
 * This is now used for exclusion constraints as well: if excludeOps is not
 * NULL then it points to an array of exclusion operator OIDs.
 */
static char *
pg_get_indexdef_worker(Oid indexrelid, int colno,
					   const Oid *excludeOps,
					   bool attrsOnly, bool keysOnly,
					   bool showTblSpc, bool inherits,
					   int prettyFlags, bool missing_ok)
{
	/* might want a separate isConstraint parameter later */
	bool		isConstraint = (excludeOps != NULL);
	HeapTuple	ht_idx;
	HeapTuple	ht_idxrel;
	HeapTuple	ht_am;
	Form_pg_index idxrec;
	Form_pg_class idxrelrec;
	Form_pg_am	amrec;
	IndexAmRoutine *amroutine;
	List	   *indexprs;
	ListCell   *indexpr_item;
	List	   *context;
	Oid			indrelid;
	int			keyno;
	Datum		indcollDatum;
	Datum		indclassDatum;
	Datum		indoptionDatum;
	oidvector  *indcollation;
	oidvector  *indclass;
	int2vector *indoption;
	StringInfoData buf;
	char	   *str;
	char	   *sep;

	/*
	 * Fetch the pg_index tuple by the Oid of the index
	 */
	ht_idx = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexrelid));
	if (!HeapTupleIsValid(ht_idx))
	{
		if (missing_ok)
			return NULL;
		elog(ERROR, "cache lookup failed for index %u", indexrelid);
	}
	idxrec = (Form_pg_index) GETSTRUCT(ht_idx);

	indrelid = idxrec->indrelid;
	Assert(indexrelid == idxrec->indexrelid);

	/* Must get indcollation, indclass, and indoption the hard way */
	indcollDatum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx,
										  Anum_pg_index_indcollation);
	indcollation = (oidvector *) DatumGetPointer(indcollDatum);

	indclassDatum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx,
										   Anum_pg_index_indclass);
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	indoptionDatum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx,
											Anum_pg_index_indoption);
	indoption = (int2vector *) DatumGetPointer(indoptionDatum);

	/*
	 * Fetch the pg_class tuple of the index relation
	 */
	ht_idxrel = SearchSysCache1(RELOID, ObjectIdGetDatum(indexrelid));
	if (!HeapTupleIsValid(ht_idxrel))
		elog(ERROR, "cache lookup failed for relation %u", indexrelid);
	idxrelrec = (Form_pg_class) GETSTRUCT(ht_idxrel);

	/*
	 * Fetch the pg_am tuple of the index' access method
	 */
	ht_am = SearchSysCache1(AMOID, ObjectIdGetDatum(idxrelrec->relam));
	if (!HeapTupleIsValid(ht_am))
		elog(ERROR, "cache lookup failed for access method %u",
			 idxrelrec->relam);
	amrec = (Form_pg_am) GETSTRUCT(ht_am);

	/* Fetch the index AM's API struct */
	amroutine = GetIndexAmRoutine(amrec->amhandler);

	/*
	 * Get the index expressions, if any.  (NOTE: we do not use the relcache
	 * versions of the expressions and predicate, because we want to display
	 * non-const-folded expressions.)
	 */
	if (!heap_attisnull(ht_idx, Anum_pg_index_indexprs, NULL))
	{
		Datum		exprsDatum;
		char	   *exprsString;

		exprsDatum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx,
											Anum_pg_index_indexprs);
		exprsString = TextDatumGetCString(exprsDatum);
		indexprs = (List *) stringToNode(exprsString);
		pfree(exprsString);
	}
	else
		indexprs = NIL;

	indexpr_item = list_head(indexprs);

	context = deparse_context_for(get_relation_name(indrelid), indrelid);

	/*
	 * Start the index definition.  Note that the index's name should never be
	 * schema-qualified, but the indexed rel's name may be.
	 */
	initStringInfo(&buf);

	if (!attrsOnly)
	{
		if (!isConstraint)
			appendStringInfo(&buf, "CREATE %sINDEX %s ON %s%s USING %s (",
							 idxrec->indisunique ? "UNIQUE " : "",
							 quote_identifier(NameStr(idxrelrec->relname)),
							 idxrelrec->relkind == RELKIND_PARTITIONED_INDEX
							 && !inherits ? "ONLY " : "",
							 (prettyFlags & PRETTYFLAG_SCHEMA) ?
							 generate_relation_name(indrelid, NIL) :
							 generate_qualified_relation_name(indrelid),
							 quote_identifier(NameStr(amrec->amname)));
		else					/* currently, must be EXCLUDE constraint */
			appendStringInfo(&buf, "EXCLUDE USING %s (",
							 quote_identifier(NameStr(amrec->amname)));
	}

	/*
	 * Report the indexed attributes
	 */
	sep = "";
	for (keyno = 0; keyno < idxrec->indnatts; keyno++)
	{
		AttrNumber	attnum = idxrec->indkey.values[keyno];
		Oid			keycoltype;
		Oid			keycolcollation;

		/*
		 * Ignore non-key attributes if told to.
		 */
		if (keysOnly && keyno >= idxrec->indnkeyatts)
			break;

		/* Otherwise, print INCLUDE to divide key and non-key attrs. */
		if (!colno && keyno == idxrec->indnkeyatts)
		{
			appendStringInfoString(&buf, ") INCLUDE (");
			sep = "";
		}

		if (!colno)
			appendStringInfoString(&buf, sep);
		sep = ", ";

		if (attnum != 0)
		{
			/* Simple index column */
			char	   *attname;
			int32		keycoltypmod;

			attname = get_attname(indrelid, attnum, false);
			if (!colno || colno == keyno + 1)
				appendStringInfoString(&buf, quote_identifier(attname));
			get_atttypetypmodcoll(indrelid, attnum,
								  &keycoltype, &keycoltypmod,
								  &keycolcollation);
		}
		else
		{
			/* expressional index */
			Node	   *indexkey;

			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexpr_item);
			indexpr_item = lnext(indexprs, indexpr_item);
			/* Deparse */
			str = deparse_expression_pretty(indexkey, context, false, false,
											prettyFlags, 0);
			if (!colno || colno == keyno + 1)
			{
				/* Need parens if it's not a bare function call */
				if (looks_like_function(indexkey))
					appendStringInfoString(&buf, str);
				else
					appendStringInfo(&buf, "(%s)", str);
			}
			keycoltype = exprType(indexkey);
			keycolcollation = exprCollation(indexkey);
		}

		/* Print additional decoration for (selected) key columns */
		if (!attrsOnly && keyno < idxrec->indnkeyatts &&
			(!colno || colno == keyno + 1))
		{
			int16		opt = indoption->values[keyno];
			Oid			indcoll = indcollation->values[keyno];
			Datum		attoptions = get_attoptions(indexrelid, keyno + 1);
			bool		has_options = attoptions != (Datum) 0;

			/* Add collation, if not default for column */
			if (OidIsValid(indcoll) && indcoll != keycolcollation)
				appendStringInfo(&buf, " COLLATE %s",
								 generate_collation_name((indcoll)));

			/* Add the operator class name, if not default */
			get_opclass_name(indclass->values[keyno],
							 has_options ? InvalidOid : keycoltype, &buf);

			if (has_options)
			{
				appendStringInfoString(&buf, " (");
				get_reloptions(&buf, attoptions);
				appendStringInfoChar(&buf, ')');
			}

			/* Add options if relevant */
			if (amroutine->amcanorder)
			{
				/* if it supports sort ordering, report DESC and NULLS opts */
				if (opt & INDOPTION_DESC)
				{
					appendStringInfoString(&buf, " DESC");
					/* NULLS FIRST is the default in this case */
					if (!(opt & INDOPTION_NULLS_FIRST))
						appendStringInfoString(&buf, " NULLS LAST");
				}
				else
				{
					if (opt & INDOPTION_NULLS_FIRST)
						appendStringInfoString(&buf, " NULLS FIRST");
				}
			}

			/* Add the exclusion operator if relevant */
			if (excludeOps != NULL)
				appendStringInfo(&buf, " WITH %s",
								 generate_operator_name(excludeOps[keyno],
														keycoltype,
														keycoltype));
		}
	}

	if (!attrsOnly)
	{
		appendStringInfoChar(&buf, ')');

		if (idxrec->indnullsnotdistinct)
			appendStringInfoString(&buf, " NULLS NOT DISTINCT");

		/*
		 * If it has options, append "WITH (options)"
		 */
		str = flatten_reloptions(indexrelid);
		if (str)
		{
			appendStringInfo(&buf, " WITH (%s)", str);
			pfree(str);
		}

		/*
		 * Print tablespace, but only if requested
		 */
		if (showTblSpc)
		{
			Oid			tblspc;

			tblspc = get_rel_tablespace(indexrelid);
			if (OidIsValid(tblspc))
			{
				if (isConstraint)
					appendStringInfoString(&buf, " USING INDEX");
				appendStringInfo(&buf, " TABLESPACE %s",
								 quote_identifier(get_tablespace_name(tblspc)));
			}
		}

		/*
		 * If it's a partial index, decompile and append the predicate
		 */
		if (!heap_attisnull(ht_idx, Anum_pg_index_indpred, NULL))
		{
			Node	   *node;
			Datum		predDatum;
			char	   *predString;

			/* Convert text string to node tree */
			predDatum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx,
											   Anum_pg_index_indpred);
			predString = TextDatumGetCString(predDatum);
			node = (Node *) stringToNode(predString);
			pfree(predString);

			/* Deparse */
			str = deparse_expression_pretty(node, context, false, false,
											prettyFlags, 0);
			if (isConstraint)
				appendStringInfo(&buf, " WHERE (%s)", str);
			else
				appendStringInfo(&buf, " WHERE %s", str);
		}
	}

	/* Clean up */
	ReleaseSysCache(ht_idx);
	ReleaseSysCache(ht_idxrel);
	ReleaseSysCache(ht_am);

	return buf.data;
}

/* ----------
 * pg_get_querydef
 *
 * Public entry point to deparse one query parsetree.
 * The pretty flags are determined by GET_PRETTY_FLAGS(pretty).
 *
 * The result is a palloc'd C string.
 * ----------
 */
char *
pg_get_querydef(Query *query, bool pretty)
{
	StringInfoData buf;
	int			prettyFlags;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	initStringInfo(&buf);

	get_query_def(query, &buf, NIL, NULL, true,
				  prettyFlags, WRAP_COLUMN_DEFAULT, 0);

	return buf.data;
}

/*
 * pg_get_statisticsobjdef
 *		Get the definition of an extended statistics object
 */
Datum
pg_get_statisticsobjdef(PG_FUNCTION_ARGS)
{
	Oid			statextid = PG_GETARG_OID(0);
	char	   *res;

	res = pg_get_statisticsobj_worker(statextid, false, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

/*
 * Internal version for use by ALTER TABLE.
 * Includes a tablespace clause in the result.
 * Returns a palloc'd C string; no pretty-printing.
 */
char *
pg_get_statisticsobjdef_string(Oid statextid)
{
	return pg_get_statisticsobj_worker(statextid, false, false);
}

/*
 * pg_get_statisticsobjdef_columns
 *		Get columns and expressions for an extended statistics object
 */
Datum
pg_get_statisticsobjdef_columns(PG_FUNCTION_ARGS)
{
	Oid			statextid = PG_GETARG_OID(0);
	char	   *res;

	res = pg_get_statisticsobj_worker(statextid, true, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

/*
 * Internal workhorse to decompile an extended statistics object.
 */
static char *
pg_get_statisticsobj_worker(Oid statextid, bool columns_only, bool missing_ok)
{
	Form_pg_statistic_ext statextrec;
	HeapTuple	statexttup;
	StringInfoData buf;
	int			colno;
	char	   *nsp;
	ArrayType  *arr;
	char	   *enabled;
	Datum		datum;
	bool		ndistinct_enabled;
	bool		dependencies_enabled;
	bool		mcv_enabled;
	int			i;
	List	   *context;
	ListCell   *lc;
	List	   *exprs = NIL;
	bool		has_exprs;
	int			ncolumns;

	statexttup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statextid));

	if (!HeapTupleIsValid(statexttup))
	{
		if (missing_ok)
			return NULL;
		elog(ERROR, "cache lookup failed for statistics object %u", statextid);
	}

	/* has the statistics expressions? */
	has_exprs = !heap_attisnull(statexttup, Anum_pg_statistic_ext_stxexprs, NULL);

	statextrec = (Form_pg_statistic_ext) GETSTRUCT(statexttup);

	/*
	 * Get the statistics expressions, if any.  (NOTE: we do not use the
	 * relcache versions of the expressions, because we want to display
	 * non-const-folded expressions.)
	 */
	if (has_exprs)
	{
		Datum		exprsDatum;
		char	   *exprsString;

		exprsDatum = SysCacheGetAttrNotNull(STATEXTOID, statexttup,
											Anum_pg_statistic_ext_stxexprs);
		exprsString = TextDatumGetCString(exprsDatum);
		exprs = (List *) stringToNode(exprsString);
		pfree(exprsString);
	}
	else
		exprs = NIL;

	/* count the number of columns (attributes and expressions) */
	ncolumns = statextrec->stxkeys.dim1 + list_length(exprs);

	initStringInfo(&buf);

	if (!columns_only)
	{
		nsp = get_namespace_name_or_temp(statextrec->stxnamespace);
		appendStringInfo(&buf, "CREATE STATISTICS %s",
						 quote_qualified_identifier(nsp,
													NameStr(statextrec->stxname)));

		/*
		 * Decode the stxkind column so that we know which stats types to
		 * print.
		 */
		datum = SysCacheGetAttrNotNull(STATEXTOID, statexttup,
									   Anum_pg_statistic_ext_stxkind);
		arr = DatumGetArrayTypeP(datum);
		if (ARR_NDIM(arr) != 1 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "stxkind is not a 1-D char array");
		enabled = (char *) ARR_DATA_PTR(arr);

		ndistinct_enabled = false;
		dependencies_enabled = false;
		mcv_enabled = false;

		for (i = 0; i < ARR_DIMS(arr)[0]; i++)
		{
			if (enabled[i] == STATS_EXT_NDISTINCT)
				ndistinct_enabled = true;
			else if (enabled[i] == STATS_EXT_DEPENDENCIES)
				dependencies_enabled = true;
			else if (enabled[i] == STATS_EXT_MCV)
				mcv_enabled = true;

			/* ignore STATS_EXT_EXPRESSIONS (it's built automatically) */
		}

		/*
		 * If any option is disabled, then we'll need to append the types
		 * clause to show which options are enabled.  We omit the types clause
		 * on purpose when all options are enabled, so a pg_dump/pg_restore
		 * will create all statistics types on a newer postgres version, if
		 * the statistics had all options enabled on the original version.
		 *
		 * But if the statistics is defined on just a single column, it has to
		 * be an expression statistics. In that case we don't need to specify
		 * kinds.
		 */
		if ((!ndistinct_enabled || !dependencies_enabled || !mcv_enabled) &&
			(ncolumns > 1))
		{
			bool		gotone = false;

			appendStringInfoString(&buf, " (");

			if (ndistinct_enabled)
			{
				appendStringInfoString(&buf, "ndistinct");
				gotone = true;
			}

			if (dependencies_enabled)
			{
				appendStringInfo(&buf, "%sdependencies", gotone ? ", " : "");
				gotone = true;
			}

			if (mcv_enabled)
				appendStringInfo(&buf, "%smcv", gotone ? ", " : "");

			appendStringInfoChar(&buf, ')');
		}

		appendStringInfoString(&buf, " ON ");
	}

	/* decode simple column references */
	for (colno = 0; colno < statextrec->stxkeys.dim1; colno++)
	{
		AttrNumber	attnum = statextrec->stxkeys.values[colno];
		char	   *attname;

		if (colno > 0)
			appendStringInfoString(&buf, ", ");

		attname = get_attname(statextrec->stxrelid, attnum, false);

		appendStringInfoString(&buf, quote_identifier(attname));
	}

	context = deparse_context_for(get_relation_name(statextrec->stxrelid),
								  statextrec->stxrelid);

	foreach(lc, exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		char	   *str;
		int			prettyFlags = PRETTYFLAG_PAREN;

		str = deparse_expression_pretty(expr, context, false, false,
										prettyFlags, 0);

		if (colno > 0)
			appendStringInfoString(&buf, ", ");

		/* Need parens if it's not a bare function call */
		if (looks_like_function(expr))
			appendStringInfoString(&buf, str);
		else
			appendStringInfo(&buf, "(%s)", str);

		colno++;
	}

	if (!columns_only)
		appendStringInfo(&buf, " FROM %s",
						 generate_relation_name(statextrec->stxrelid, NIL));

	ReleaseSysCache(statexttup);

	return buf.data;
}

/*
 * Generate text array of expressions for statistics object.
 */
Datum
pg_get_statisticsobjdef_expressions(PG_FUNCTION_ARGS)
{
	Oid			statextid = PG_GETARG_OID(0);
	Form_pg_statistic_ext statextrec;
	HeapTuple	statexttup;
	Datum		datum;
	List	   *context;
	ListCell   *lc;
	List	   *exprs = NIL;
	bool		has_exprs;
	char	   *tmp;
	ArrayBuildState *astate = NULL;

	statexttup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statextid));

	if (!HeapTupleIsValid(statexttup))
		PG_RETURN_NULL();

	/* Does the stats object have expressions? */
	has_exprs = !heap_attisnull(statexttup, Anum_pg_statistic_ext_stxexprs, NULL);

	/* no expressions? we're done */
	if (!has_exprs)
	{
		ReleaseSysCache(statexttup);
		PG_RETURN_NULL();
	}

	statextrec = (Form_pg_statistic_ext) GETSTRUCT(statexttup);

	/*
	 * Get the statistics expressions, and deparse them into text values.
	 */
	datum = SysCacheGetAttrNotNull(STATEXTOID, statexttup,
								   Anum_pg_statistic_ext_stxexprs);
	tmp = TextDatumGetCString(datum);
	exprs = (List *) stringToNode(tmp);
	pfree(tmp);

	context = deparse_context_for(get_relation_name(statextrec->stxrelid),
								  statextrec->stxrelid);

	foreach(lc, exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		char	   *str;
		int			prettyFlags = PRETTYFLAG_INDENT;

		str = deparse_expression_pretty(expr, context, false, false,
										prettyFlags, 0);

		astate = accumArrayResult(astate,
								  PointerGetDatum(cstring_to_text(str)),
								  false,
								  TEXTOID,
								  CurrentMemoryContext);
	}

	ReleaseSysCache(statexttup);

	PG_RETURN_DATUM(makeArrayResult(astate, CurrentMemoryContext));
}

/*
 * pg_get_partkeydef
 *
 * Returns the partition key specification, ie, the following:
 *
 * { RANGE | LIST | HASH } (column opt_collation opt_opclass [, ...])
 */
Datum
pg_get_partkeydef(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *res;

	res = pg_get_partkeydef_worker(relid, PRETTYFLAG_INDENT, false, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

/* Internal version that just reports the column definitions */
char *
pg_get_partkeydef_columns(Oid relid, bool pretty)
{
	int			prettyFlags;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	return pg_get_partkeydef_worker(relid, prettyFlags, true, false);
}

/*
 * Internal workhorse to decompile a partition key definition.
 */
static char *
pg_get_partkeydef_worker(Oid relid, int prettyFlags,
						 bool attrsOnly, bool missing_ok)
{
	Form_pg_partitioned_table form;
	HeapTuple	tuple;
	oidvector  *partclass;
	oidvector  *partcollation;
	List	   *partexprs;
	ListCell   *partexpr_item;
	List	   *context;
	Datum		datum;
	StringInfoData buf;
	int			keyno;
	char	   *str;
	char	   *sep;

	tuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
	{
		if (missing_ok)
			return NULL;
		elog(ERROR, "cache lookup failed for partition key of %u", relid);
	}

	form = (Form_pg_partitioned_table) GETSTRUCT(tuple);

	Assert(form->partrelid == relid);

	/* Must get partclass and partcollation the hard way */
	datum = SysCacheGetAttrNotNull(PARTRELID, tuple,
								   Anum_pg_partitioned_table_partclass);
	partclass = (oidvector *) DatumGetPointer(datum);

	datum = SysCacheGetAttrNotNull(PARTRELID, tuple,
								   Anum_pg_partitioned_table_partcollation);
	partcollation = (oidvector *) DatumGetPointer(datum);


	/*
	 * Get the expressions, if any.  (NOTE: we do not use the relcache
	 * versions of the expressions, because we want to display
	 * non-const-folded expressions.)
	 */
	if (!heap_attisnull(tuple, Anum_pg_partitioned_table_partexprs, NULL))
	{
		Datum		exprsDatum;
		char	   *exprsString;

		exprsDatum = SysCacheGetAttrNotNull(PARTRELID, tuple,
											Anum_pg_partitioned_table_partexprs);
		exprsString = TextDatumGetCString(exprsDatum);
		partexprs = (List *) stringToNode(exprsString);

		if (!IsA(partexprs, List))
			elog(ERROR, "unexpected node type found in partexprs: %d",
				 (int) nodeTag(partexprs));

		pfree(exprsString);
	}
	else
		partexprs = NIL;

	partexpr_item = list_head(partexprs);
	context = deparse_context_for(get_relation_name(relid), relid);

	initStringInfo(&buf);

	switch (form->partstrat)
	{
		case PARTITION_STRATEGY_HASH:
			if (!attrsOnly)
				appendStringInfoString(&buf, "HASH");
			break;
		case PARTITION_STRATEGY_LIST:
			if (!attrsOnly)
				appendStringInfoString(&buf, "LIST");
			break;
		case PARTITION_STRATEGY_RANGE:
			if (!attrsOnly)
				appendStringInfoString(&buf, "RANGE");
			break;
		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) form->partstrat);
	}

	if (!attrsOnly)
		appendStringInfoString(&buf, " (");
	sep = "";
	for (keyno = 0; keyno < form->partnatts; keyno++)
	{
		AttrNumber	attnum = form->partattrs.values[keyno];
		Oid			keycoltype;
		Oid			keycolcollation;
		Oid			partcoll;

		appendStringInfoString(&buf, sep);
		sep = ", ";
		if (attnum != 0)
		{
			/* Simple attribute reference */
			char	   *attname;
			int32		keycoltypmod;

			attname = get_attname(relid, attnum, false);
			appendStringInfoString(&buf, quote_identifier(attname));
			get_atttypetypmodcoll(relid, attnum,
								  &keycoltype, &keycoltypmod,
								  &keycolcollation);
		}
		else
		{
			/* Expression */
			Node	   *partkey;

			if (partexpr_item == NULL)
				elog(ERROR, "too few entries in partexprs list");
			partkey = (Node *) lfirst(partexpr_item);
			partexpr_item = lnext(partexprs, partexpr_item);

			/* Deparse */
			str = deparse_expression_pretty(partkey, context, false, false,
											prettyFlags, 0);
			/* Need parens if it's not a bare function call */
			if (looks_like_function(partkey))
				appendStringInfoString(&buf, str);
			else
				appendStringInfo(&buf, "(%s)", str);

			keycoltype = exprType(partkey);
			keycolcollation = exprCollation(partkey);
		}

		/* Add collation, if not default for column */
		partcoll = partcollation->values[keyno];
		if (!attrsOnly && OidIsValid(partcoll) && partcoll != keycolcollation)
			appendStringInfo(&buf, " COLLATE %s",
							 generate_collation_name((partcoll)));

		/* Add the operator class name, if not default */
		if (!attrsOnly)
			get_opclass_name(partclass->values[keyno], keycoltype, &buf);
	}

	if (!attrsOnly)
		appendStringInfoChar(&buf, ')');

	/* Clean up */
	ReleaseSysCache(tuple);

	return buf.data;
}

/*
 * pg_get_partition_constraintdef
 *
 * Returns partition constraint expression as a string for the input relation
 */
Datum
pg_get_partition_constraintdef(PG_FUNCTION_ARGS)
{
	Oid			relationId = PG_GETARG_OID(0);
	Expr	   *constr_expr;
	int			prettyFlags;
	List	   *context;
	char	   *consrc;

	constr_expr = get_partition_qual_relid(relationId);

	/* Quick exit if no partition constraint */
	if (constr_expr == NULL)
		PG_RETURN_NULL();

	/*
	 * Deparse and return the constraint expression.
	 */
	prettyFlags = PRETTYFLAG_INDENT;
	context = deparse_context_for(get_relation_name(relationId), relationId);
	consrc = deparse_expression_pretty((Node *) constr_expr, context, false,
									   false, prettyFlags, 0);

	PG_RETURN_TEXT_P(string_to_text(consrc));
}

/*
 * pg_get_partconstrdef_string
 *
 * Returns the partition constraint as a C-string for the input relation, with
 * the given alias.  No pretty-printing.
 */
char *
pg_get_partconstrdef_string(Oid partitionId, char *aliasname)
{
	Expr	   *constr_expr;
	List	   *context;

	constr_expr = get_partition_qual_relid(partitionId);
	context = deparse_context_for(aliasname, partitionId);

	return deparse_expression((Node *) constr_expr, context, true, false);
}

/*
 * pg_get_constraintdef
 *
 * Returns the definition for the constraint, ie, everything that needs to
 * appear after "ALTER TABLE ... ADD CONSTRAINT <constraintname>".
 */
Datum
pg_get_constraintdef(PG_FUNCTION_ARGS)
{
	Oid			constraintId = PG_GETARG_OID(0);
	int			prettyFlags;
	char	   *res;

	prettyFlags = PRETTYFLAG_INDENT;

	res = pg_get_constraintdef_worker(constraintId, false, prettyFlags, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

Datum
pg_get_constraintdef_ext(PG_FUNCTION_ARGS)
{
	Oid			constraintId = PG_GETARG_OID(0);
	bool		pretty = PG_GETARG_BOOL(1);
	int			prettyFlags;
	char	   *res;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	res = pg_get_constraintdef_worker(constraintId, false, prettyFlags, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

/*
 * Internal version that returns a full ALTER TABLE ... ADD CONSTRAINT command
 */
char *
pg_get_constraintdef_command(Oid constraintId)
{
	return pg_get_constraintdef_worker(constraintId, true, 0, false);
}

/*
 * As of 9.4, we now use an MVCC snapshot for this.
 */
static char *
pg_get_constraintdef_worker(Oid constraintId, bool fullCommand,
							int prettyFlags, bool missing_ok)
{
	HeapTuple	tup;
	Form_pg_constraint conForm;
	StringInfoData buf;
	SysScanDesc scandesc;
	ScanKeyData scankey[1];
	Snapshot	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	Relation	relation = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&scankey[0],
				Anum_pg_constraint_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(constraintId));

	scandesc = systable_beginscan(relation,
								  ConstraintOidIndexId,
								  true,
								  snapshot,
								  1,
								  scankey);

	/*
	 * We later use the tuple with SysCacheGetAttr() as if we had obtained it
	 * via SearchSysCache, which works fine.
	 */
	tup = systable_getnext(scandesc);

	UnregisterSnapshot(snapshot);

	if (!HeapTupleIsValid(tup))
	{
		if (missing_ok)
		{
			systable_endscan(scandesc);
			table_close(relation, AccessShareLock);
			return NULL;
		}
		elog(ERROR, "could not find tuple for constraint %u", constraintId);
	}

	conForm = (Form_pg_constraint) GETSTRUCT(tup);

	initStringInfo(&buf);

	if (fullCommand)
	{
		if (OidIsValid(conForm->conrelid))
		{
			/*
			 * Currently, callers want ALTER TABLE (without ONLY) for CHECK
			 * constraints, and other types of constraints don't inherit
			 * anyway so it doesn't matter whether we say ONLY or not. Someday
			 * we might need to let callers specify whether to put ONLY in the
			 * command.
			 */
			appendStringInfo(&buf, "ALTER TABLE %s ADD CONSTRAINT %s ",
							 generate_qualified_relation_name(conForm->conrelid),
							 quote_identifier(NameStr(conForm->conname)));
		}
		else
		{
			/* Must be a domain constraint */
			Assert(OidIsValid(conForm->contypid));
			appendStringInfo(&buf, "ALTER DOMAIN %s ADD CONSTRAINT %s ",
							 generate_qualified_type_name(conForm->contypid),
							 quote_identifier(NameStr(conForm->conname)));
		}
	}

	switch (conForm->contype)
	{
		case CONSTRAINT_FOREIGN:
			{
				Datum		val;
				bool		isnull;
				const char *string;

				/* Start off the constraint definition */
				appendStringInfoString(&buf, "FOREIGN KEY (");

				/* Fetch and build referencing-column list */
				val = SysCacheGetAttrNotNull(CONSTROID, tup,
											 Anum_pg_constraint_conkey);

				/* If it is a temporal foreign key then it uses PERIOD. */
				decompile_column_index_array(val, conForm->conrelid, conForm->conperiod, &buf);

				/* add foreign relation name */
				appendStringInfo(&buf, ") REFERENCES %s(",
								 generate_relation_name(conForm->confrelid,
														NIL));

				/* Fetch and build referenced-column list */
				val = SysCacheGetAttrNotNull(CONSTROID, tup,
											 Anum_pg_constraint_confkey);

				decompile_column_index_array(val, conForm->confrelid, conForm->conperiod, &buf);

				appendStringInfoChar(&buf, ')');

				/* Add match type */
				switch (conForm->confmatchtype)
				{
					case FKCONSTR_MATCH_FULL:
						string = " MATCH FULL";
						break;
					case FKCONSTR_MATCH_PARTIAL:
						string = " MATCH PARTIAL";
						break;
					case FKCONSTR_MATCH_SIMPLE:
						string = "";
						break;
					default:
						elog(ERROR, "unrecognized confmatchtype: %d",
							 conForm->confmatchtype);
						string = "";	/* keep compiler quiet */
						break;
				}
				appendStringInfoString(&buf, string);

				/* Add ON UPDATE and ON DELETE clauses, if needed */
				switch (conForm->confupdtype)
				{
					case FKCONSTR_ACTION_NOACTION:
						string = NULL;	/* suppress default */
						break;
					case FKCONSTR_ACTION_RESTRICT:
						string = "RESTRICT";
						break;
					case FKCONSTR_ACTION_CASCADE:
						string = "CASCADE";
						break;
					case FKCONSTR_ACTION_SETNULL:
						string = "SET NULL";
						break;
					case FKCONSTR_ACTION_SETDEFAULT:
						string = "SET DEFAULT";
						break;
					default:
						elog(ERROR, "unrecognized confupdtype: %d",
							 conForm->confupdtype);
						string = NULL;	/* keep compiler quiet */
						break;
				}
				if (string)
					appendStringInfo(&buf, " ON UPDATE %s", string);

				switch (conForm->confdeltype)
				{
					case FKCONSTR_ACTION_NOACTION:
						string = NULL;	/* suppress default */
						break;
					case FKCONSTR_ACTION_RESTRICT:
						string = "RESTRICT";
						break;
					case FKCONSTR_ACTION_CASCADE:
						string = "CASCADE";
						break;
					case FKCONSTR_ACTION_SETNULL:
						string = "SET NULL";
						break;
					case FKCONSTR_ACTION_SETDEFAULT:
						string = "SET DEFAULT";
						break;
					default:
						elog(ERROR, "unrecognized confdeltype: %d",
							 conForm->confdeltype);
						string = NULL;	/* keep compiler quiet */
						break;
				}
				if (string)
					appendStringInfo(&buf, " ON DELETE %s", string);

				/*
				 * Add columns specified to SET NULL or SET DEFAULT if
				 * provided.
				 */
				val = SysCacheGetAttr(CONSTROID, tup,
									  Anum_pg_constraint_confdelsetcols, &isnull);
				if (!isnull)
				{
					appendStringInfoString(&buf, " (");
					decompile_column_index_array(val, conForm->conrelid, false, &buf);
					appendStringInfoChar(&buf, ')');
				}

				break;
			}
		case CONSTRAINT_PRIMARY:
		case CONSTRAINT_UNIQUE:
			{
				Datum		val;
				Oid			indexId;
				int			keyatts;
				HeapTuple	indtup;

				/* Start off the constraint definition */
				if (conForm->contype == CONSTRAINT_PRIMARY)
					appendStringInfoString(&buf, "PRIMARY KEY ");
				else
					appendStringInfoString(&buf, "UNIQUE ");

				indexId = conForm->conindid;

				indtup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexId));
				if (!HeapTupleIsValid(indtup))
					elog(ERROR, "cache lookup failed for index %u", indexId);
				if (conForm->contype == CONSTRAINT_UNIQUE &&
					((Form_pg_index) GETSTRUCT(indtup))->indnullsnotdistinct)
					appendStringInfoString(&buf, "NULLS NOT DISTINCT ");

				appendStringInfoChar(&buf, '(');

				/* Fetch and build target column list */
				val = SysCacheGetAttrNotNull(CONSTROID, tup,
											 Anum_pg_constraint_conkey);

				keyatts = decompile_column_index_array(val, conForm->conrelid, false, &buf);
				if (conForm->conperiod)
					appendStringInfoString(&buf, " WITHOUT OVERLAPS");

				appendStringInfoChar(&buf, ')');

				/* Build including column list (from pg_index.indkeys) */
				val = SysCacheGetAttrNotNull(INDEXRELID, indtup,
											 Anum_pg_index_indnatts);
				if (DatumGetInt32(val) > keyatts)
				{
					Datum		cols;
					Datum	   *keys;
					int			nKeys;
					int			j;

					appendStringInfoString(&buf, " INCLUDE (");

					cols = SysCacheGetAttrNotNull(INDEXRELID, indtup,
												  Anum_pg_index_indkey);

					deconstruct_array_builtin(DatumGetArrayTypeP(cols), INT2OID,
											  &keys, NULL, &nKeys);

					for (j = keyatts; j < nKeys; j++)
					{
						char	   *colName;

						colName = get_attname(conForm->conrelid,
											  DatumGetInt16(keys[j]), false);
						if (j > keyatts)
							appendStringInfoString(&buf, ", ");
						appendStringInfoString(&buf, quote_identifier(colName));
					}

					appendStringInfoChar(&buf, ')');
				}
				ReleaseSysCache(indtup);

				/* XXX why do we only print these bits if fullCommand? */
				if (fullCommand && OidIsValid(indexId))
				{
					char	   *options = flatten_reloptions(indexId);
					Oid			tblspc;

					if (options)
					{
						appendStringInfo(&buf, " WITH (%s)", options);
						pfree(options);
					}

					/*
					 * Print the tablespace, unless it's the database default.
					 * This is to help ALTER TABLE usage of this facility,
					 * which needs this behavior to recreate exact catalog
					 * state.
					 */
					tblspc = get_rel_tablespace(indexId);
					if (OidIsValid(tblspc))
						appendStringInfo(&buf, " USING INDEX TABLESPACE %s",
										 quote_identifier(get_tablespace_name(tblspc)));
				}

				break;
			}
		case CONSTRAINT_CHECK:
			{
				Datum		val;
				char	   *conbin;
				char	   *consrc;
				Node	   *expr;
				List	   *context;

				/* Fetch constraint expression in parsetree form */
				val = SysCacheGetAttrNotNull(CONSTROID, tup,
											 Anum_pg_constraint_conbin);

				conbin = TextDatumGetCString(val);
				expr = stringToNode(conbin);

				/* Set up deparsing context for Var nodes in constraint */
				if (conForm->conrelid != InvalidOid)
				{
					/* relation constraint */
					context = deparse_context_for(get_relation_name(conForm->conrelid),
												  conForm->conrelid);
				}
				else
				{
					/* domain constraint --- can't have Vars */
					context = NIL;
				}

				consrc = deparse_expression_pretty(expr, context, false, false,
												   prettyFlags, 0);

				/*
				 * Now emit the constraint definition, adding NO INHERIT if
				 * necessary.
				 *
				 * There are cases where the constraint expression will be
				 * fully parenthesized and we don't need the outer parens ...
				 * but there are other cases where we do need 'em.  Be
				 * conservative for now.
				 *
				 * Note that simply checking for leading '(' and trailing ')'
				 * would NOT be good enough, consider "(x > 0) AND (y > 0)".
				 */
				appendStringInfo(&buf, "CHECK (%s)%s",
								 consrc,
								 conForm->connoinherit ? " NO INHERIT" : "");
				break;
			}
		case CONSTRAINT_NOTNULL:
			{
				if (conForm->conrelid)
				{
					AttrNumber	attnum;

					attnum = extractNotNullColumn(tup);

					appendStringInfo(&buf, "NOT NULL %s",
									 quote_identifier(get_attname(conForm->conrelid,
																  attnum, false)));
					if (((Form_pg_constraint) GETSTRUCT(tup))->connoinherit)
						appendStringInfoString(&buf, " NO INHERIT");
				}
				else if (conForm->contypid)
				{
					/* conkey is null for domain not-null constraints */
					appendStringInfoString(&buf, "NOT NULL");
				}
				break;
			}

		case CONSTRAINT_TRIGGER:

			/*
			 * There isn't an ALTER TABLE syntax for creating a user-defined
			 * constraint trigger, but it seems better to print something than
			 * throw an error; if we throw error then this function couldn't
			 * safely be applied to all rows of pg_constraint.
			 */
			appendStringInfoString(&buf, "TRIGGER");
			break;
		case CONSTRAINT_EXCLUSION:
			{
				Oid			indexOid = conForm->conindid;
				Datum		val;
				Datum	   *elems;
				int			nElems;
				int			i;
				Oid		   *operators;

				/* Extract operator OIDs from the pg_constraint tuple */
				val = SysCacheGetAttrNotNull(CONSTROID, tup,
											 Anum_pg_constraint_conexclop);

				deconstruct_array_builtin(DatumGetArrayTypeP(val), OIDOID,
										  &elems, NULL, &nElems);

				operators = (Oid *) palloc(nElems * sizeof(Oid));
				for (i = 0; i < nElems; i++)
					operators[i] = DatumGetObjectId(elems[i]);

				/* pg_get_indexdef_worker does the rest */
				/* suppress tablespace because pg_dump wants it that way */
				appendStringInfoString(&buf,
									   pg_get_indexdef_worker(indexOid,
															  0,
															  operators,
															  false,
															  false,
															  false,
															  false,
															  prettyFlags,
															  false));
				break;
			}
		default:
			elog(ERROR, "invalid constraint type \"%c\"", conForm->contype);
			break;
	}

	if (conForm->condeferrable)
		appendStringInfoString(&buf, " DEFERRABLE");
	if (conForm->condeferred)
		appendStringInfoString(&buf, " INITIALLY DEFERRED");

	/* Validated status is irrelevant when the constraint is NOT ENFORCED. */
	if (!conForm->conenforced)
		appendStringInfoString(&buf, " NOT ENFORCED");
	else if (!conForm->convalidated)
		appendStringInfoString(&buf, " NOT VALID");

	/* Cleanup */
	systable_endscan(scandesc);
	table_close(relation, AccessShareLock);

	return buf.data;
}


/*
 * Convert an int16[] Datum into a comma-separated list of column names
 * for the indicated relation; append the list to buf.  Returns the number
 * of keys.
 */
static int
decompile_column_index_array(Datum column_index_array, Oid relId,
							 bool withPeriod, StringInfo buf)
{
	Datum	   *keys;
	int			nKeys;
	int			j;

	/* Extract data from array of int16 */
	deconstruct_array_builtin(DatumGetArrayTypeP(column_index_array), INT2OID,
							  &keys, NULL, &nKeys);

	for (j = 0; j < nKeys; j++)
	{
		char	   *colName;

		colName = get_attname(relId, DatumGetInt16(keys[j]), false);

		if (j == 0)
			appendStringInfoString(buf, quote_identifier(colName));
		else
			appendStringInfo(buf, ", %s%s",
							 (withPeriod && j == nKeys - 1) ? "PERIOD " : "",
							 quote_identifier(colName));
	}

	return nKeys;
}


/* ----------
 * pg_get_expr			- Decompile an expression tree
 *
 * Input: an expression tree in nodeToString form, and a relation OID
 *
 * Output: reverse-listed expression
 *
 * Currently, the expression can only refer to a single relation, namely
 * the one specified by the second parameter.  This is sufficient for
 * partial indexes, column default expressions, etc.  We also support
 * Var-free expressions, for which the OID can be InvalidOid.
 *
 * If the OID is nonzero but not actually valid, don't throw an error,
 * just return NULL.  This is a bit questionable, but it's what we've
 * done historically, and it can help avoid unwanted failures when
 * examining catalog entries for just-deleted relations.
 *
 * We expect this function to work, or throw a reasonably clean error,
 * for any node tree that can appear in a catalog pg_node_tree column.
 * Query trees, such as those appearing in pg_rewrite.ev_action, are
 * not supported.  Nor are expressions in more than one relation, which
 * can appear in places like pg_rewrite.ev_qual.
 * ----------
 */
Datum
pg_get_expr(PG_FUNCTION_ARGS)
{
	text	   *expr = PG_GETARG_TEXT_PP(0);
	Oid			relid = PG_GETARG_OID(1);
	text	   *result;
	int			prettyFlags;

	prettyFlags = PRETTYFLAG_INDENT;

	result = pg_get_expr_worker(expr, relid, prettyFlags);
	if (result)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
pg_get_expr_ext(PG_FUNCTION_ARGS)
{
	text	   *expr = PG_GETARG_TEXT_PP(0);
	Oid			relid = PG_GETARG_OID(1);
	bool		pretty = PG_GETARG_BOOL(2);
	text	   *result;
	int			prettyFlags;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	result = pg_get_expr_worker(expr, relid, prettyFlags);
	if (result)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

static text *
pg_get_expr_worker(text *expr, Oid relid, int prettyFlags)
{
	Node	   *node;
	Node	   *tst;
	Relids		relids;
	List	   *context;
	char	   *exprstr;
	Relation	rel = NULL;
	char	   *str;

	/* Convert input pg_node_tree (really TEXT) object to C string */
	exprstr = text_to_cstring(expr);

	/* Convert expression to node tree */
	node = (Node *) stringToNode(exprstr);

	pfree(exprstr);

	/*
	 * Throw error if the input is a querytree rather than an expression tree.
	 * While we could support queries here, there seems no very good reason
	 * to.  In most such catalog columns, we'll see a List of Query nodes, or
	 * even nested Lists, so drill down to a non-List node before checking.
	 */
	tst = node;
	while (tst && IsA(tst, List))
		tst = linitial((List *) tst);
	if (tst && IsA(tst, Query))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input is a query, not an expression")));

	/*
	 * Throw error if the expression contains Vars we won't be able to
	 * deparse.
	 */
	relids = pull_varnos(NULL, node);
	if (OidIsValid(relid))
	{
		if (!bms_is_subset(relids, bms_make_singleton(1)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("expression contains variables of more than one relation")));
	}
	else
	{
		if (!bms_is_empty(relids))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("expression contains variables")));
	}

	/*
	 * Prepare deparse context if needed.  If we are deparsing with a relid,
	 * we need to transiently open and lock the rel, to make sure it won't go
	 * away underneath us.  (set_relation_column_names would lock it anyway,
	 * so this isn't really introducing any new behavior.)
	 */
	if (OidIsValid(relid))
	{
		rel = try_relation_open(relid, AccessShareLock);
		if (rel == NULL)
			return NULL;
		context = deparse_context_for(RelationGetRelationName(rel), relid);
	}
	else
		context = NIL;

	/* Deparse */
	str = deparse_expression_pretty(node, context, false, false,
									prettyFlags, 0);

	if (rel != NULL)
		relation_close(rel, AccessShareLock);

	return string_to_text(str);
}


/* ----------
 * pg_get_userbyid		- Get a user name by roleid and
 *				  fallback to 'unknown (OID=n)'
 * ----------
 */
Datum
pg_get_userbyid(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Name		result;
	HeapTuple	roletup;
	Form_pg_authid role_rec;

	/*
	 * Allocate space for the result
	 */
	result = (Name) palloc(NAMEDATALEN);
	memset(NameStr(*result), 0, NAMEDATALEN);

	/*
	 * Get the pg_authid entry and print the result
	 */
	roletup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (HeapTupleIsValid(roletup))
	{
		role_rec = (Form_pg_authid) GETSTRUCT(roletup);
		*result = role_rec->rolname;
		ReleaseSysCache(roletup);
	}
	else
		sprintf(NameStr(*result), "unknown (OID=%u)", roleid);

	PG_RETURN_NAME(result);
}


/*
 * pg_get_serial_sequence
 *		Get the name of the sequence used by an identity or serial column,
 *		formatted suitably for passing to setval, nextval or currval.
 *		First parameter is not treated as double-quoted, second parameter
 *		is --- see documentation for reason.
 */
Datum
pg_get_serial_sequence(PG_FUNCTION_ARGS)
{
	text	   *tablename = PG_GETARG_TEXT_PP(0);
	text	   *columnname = PG_GETARG_TEXT_PP(1);
	RangeVar   *tablerv;
	Oid			tableOid;
	char	   *column;
	AttrNumber	attnum;
	Oid			sequenceId = InvalidOid;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	/* Look up table name.  Can't lock it - we might not have privileges. */
	tablerv = makeRangeVarFromNameList(textToQualifiedNameList(tablename));
	tableOid = RangeVarGetRelid(tablerv, NoLock, false);

	/* Get the number of the column */
	column = text_to_cstring(columnname);

	attnum = get_attnum(tableOid, column);
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						column, tablerv->relname)));

	/* Search the dependency table for the dependent sequence */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(tableOid));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(attnum));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		/*
		 * Look for an auto dependency (serial column) or internal dependency
		 * (identity column) of a sequence on a column.  (We need the relkind
		 * test because indexes can also have auto dependencies on columns.)
		 */
		if (deprec->classid == RelationRelationId &&
			deprec->objsubid == 0 &&
			(deprec->deptype == DEPENDENCY_AUTO ||
			 deprec->deptype == DEPENDENCY_INTERNAL) &&
			get_rel_relkind(deprec->objid) == RELKIND_SEQUENCE)
		{
			sequenceId = deprec->objid;
			break;
		}
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	if (OidIsValid(sequenceId))
	{
		char	   *result;

		result = generate_qualified_relation_name(sequenceId);

		PG_RETURN_TEXT_P(string_to_text(result));
	}

	PG_RETURN_NULL();
}


/*
 * pg_get_functiondef
 *		Returns the complete "CREATE OR REPLACE FUNCTION ..." statement for
 *		the specified function.
 *
 * Note: if you change the output format of this function, be careful not
 * to break psql's rules (in \ef and \sf) for identifying the start of the
 * function body.  To wit: the function body starts on a line that begins with
 * "AS ", "BEGIN ", or "RETURN ", and no preceding line will look like that.
 */
Datum
pg_get_functiondef(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	StringInfoData buf;
	StringInfoData dq;
	HeapTuple	proctup;
	Form_pg_proc proc;
	bool		isfunction;
	Datum		tmp;
	bool		isnull;
	const char *prosrc;
	const char *name;
	const char *nsp;
	float4		procost;
	int			oldlen;

	initStringInfo(&buf);

	/* Look up the function */
	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		PG_RETURN_NULL();

	proc = (Form_pg_proc) GETSTRUCT(proctup);
	name = NameStr(proc->proname);

	if (proc->prokind == PROKIND_AGGREGATE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an aggregate function", name)));

	isfunction = (proc->prokind != PROKIND_PROCEDURE);

	/*
	 * We always qualify the function name, to ensure the right function gets
	 * replaced.
	 */
	nsp = get_namespace_name_or_temp(proc->pronamespace);
	appendStringInfo(&buf, "CREATE OR REPLACE %s %s(",
					 isfunction ? "FUNCTION" : "PROCEDURE",
					 quote_qualified_identifier(nsp, name));
	(void) print_function_arguments(&buf, proctup, false, true);
	appendStringInfoString(&buf, ")\n");
	if (isfunction)
	{
		appendStringInfoString(&buf, " RETURNS ");
		print_function_rettype(&buf, proctup);
		appendStringInfoChar(&buf, '\n');
	}

	print_function_trftypes(&buf, proctup);

	appendStringInfo(&buf, " LANGUAGE %s\n",
					 quote_identifier(get_language_name(proc->prolang, false)));

	/* Emit some miscellaneous options on one line */
	oldlen = buf.len;

	if (proc->prokind == PROKIND_WINDOW)
		appendStringInfoString(&buf, " WINDOW");
	switch (proc->provolatile)
	{
		case PROVOLATILE_IMMUTABLE:
			appendStringInfoString(&buf, " IMMUTABLE");
			break;
		case PROVOLATILE_STABLE:
			appendStringInfoString(&buf, " STABLE");
			break;
		case PROVOLATILE_VOLATILE:
			break;
	}

	switch (proc->proparallel)
	{
		case PROPARALLEL_SAFE:
			appendStringInfoString(&buf, " PARALLEL SAFE");
			break;
		case PROPARALLEL_RESTRICTED:
			appendStringInfoString(&buf, " PARALLEL RESTRICTED");
			break;
		case PROPARALLEL_UNSAFE:
			break;
	}

	if (proc->proisstrict)
		appendStringInfoString(&buf, " STRICT");
	if (proc->prosecdef)
		appendStringInfoString(&buf, " SECURITY DEFINER");
	if (proc->proleakproof)
		appendStringInfoString(&buf, " LEAKPROOF");

	/* This code for the default cost and rows should match functioncmds.c */
	if (proc->prolang == INTERNALlanguageId ||
		proc->prolang == ClanguageId)
		procost = 1;
	else
		procost = 100;
	if (proc->procost != procost)
		appendStringInfo(&buf, " COST %g", proc->procost);

	if (proc->prorows > 0 && proc->prorows != 1000)
		appendStringInfo(&buf, " ROWS %g", proc->prorows);

	if (proc->prosupport)
	{
		Oid			argtypes[1];

		/*
		 * We should qualify the support function's name if it wouldn't be
		 * resolved by lookup in the current search path.
		 */
		argtypes[0] = INTERNALOID;
		appendStringInfo(&buf, " SUPPORT %s",
						 generate_function_name(proc->prosupport, 1,
												NIL, argtypes,
												false, NULL, false));
	}

	if (oldlen != buf.len)
		appendStringInfoChar(&buf, '\n');

	/* Emit any proconfig options, one per line */
	tmp = SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_proconfig, &isnull);
	if (!isnull)
	{
		ArrayType  *a = DatumGetArrayTypeP(tmp);
		int			i;

		Assert(ARR_ELEMTYPE(a) == TEXTOID);
		Assert(ARR_NDIM(a) == 1);
		Assert(ARR_LBOUND(a)[0] == 1);

		for (i = 1; i <= ARR_DIMS(a)[0]; i++)
		{
			Datum		d;

			d = array_ref(a, 1, &i,
						  -1 /* varlenarray */ ,
						  -1 /* TEXT's typlen */ ,
						  false /* TEXT's typbyval */ ,
						  TYPALIGN_INT /* TEXT's typalign */ ,
						  &isnull);
			if (!isnull)
			{
				char	   *configitem = TextDatumGetCString(d);
				char	   *pos;

				pos = strchr(configitem, '=');
				if (pos == NULL)
					continue;
				*pos++ = '\0';

				appendStringInfo(&buf, " SET %s TO ",
								 quote_identifier(configitem));

				/*
				 * Variables that are marked GUC_LIST_QUOTE were already fully
				 * quoted by flatten_set_variable_args() before they were put
				 * into the proconfig array.  However, because the quoting
				 * rules used there aren't exactly like SQL's, we have to
				 * break the list value apart and then quote the elements as
				 * string literals.  (The elements may be double-quoted as-is,
				 * but we can't just feed them to the SQL parser; it would do
				 * the wrong thing with elements that are zero-length or
				 * longer than NAMEDATALEN.)
				 *
				 * Variables that are not so marked should just be emitted as
				 * simple string literals.  If the variable is not known to
				 * guc.c, we'll do that; this makes it unsafe to use
				 * GUC_LIST_QUOTE for extension variables.
				 */
				if (GetConfigOptionFlags(configitem, true) & GUC_LIST_QUOTE)
				{
					List	   *namelist;
					ListCell   *lc;

					/* Parse string into list of identifiers */
					if (!SplitGUCList(pos, ',', &namelist))
					{
						/* this shouldn't fail really */
						elog(ERROR, "invalid list syntax in proconfig item");
					}
					foreach(lc, namelist)
					{
						char	   *curname = (char *) lfirst(lc);

						simple_quote_literal(&buf, curname);
						if (lnext(namelist, lc))
							appendStringInfoString(&buf, ", ");
					}
				}
				else
					simple_quote_literal(&buf, pos);
				appendStringInfoChar(&buf, '\n');
			}
		}
	}

	/* And finally the function definition ... */
	(void) SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_prosqlbody, &isnull);
	if (proc->prolang == SQLlanguageId && !isnull)
	{
		print_function_sqlbody(&buf, proctup);
	}
	else
	{
		appendStringInfoString(&buf, "AS ");

		tmp = SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_probin, &isnull);
		if (!isnull)
		{
			simple_quote_literal(&buf, TextDatumGetCString(tmp));
			appendStringInfoString(&buf, ", "); /* assume prosrc isn't null */
		}

		tmp = SysCacheGetAttrNotNull(PROCOID, proctup, Anum_pg_proc_prosrc);
		prosrc = TextDatumGetCString(tmp);

		/*
		 * We always use dollar quoting.  Figure out a suitable delimiter.
		 *
		 * Since the user is likely to be editing the function body string, we
		 * shouldn't use a short delimiter that he might easily create a
		 * conflict with.  Hence prefer "$function$"/"$procedure$", but extend
		 * if needed.
		 */
		initStringInfo(&dq);
		appendStringInfoChar(&dq, '$');
		appendStringInfoString(&dq, (isfunction ? "function" : "procedure"));
		while (strstr(prosrc, dq.data) != NULL)
			appendStringInfoChar(&dq, 'x');
		appendStringInfoChar(&dq, '$');

		appendBinaryStringInfo(&buf, dq.data, dq.len);
		appendStringInfoString(&buf, prosrc);
		appendBinaryStringInfo(&buf, dq.data, dq.len);
	}

	appendStringInfoChar(&buf, '\n');

	ReleaseSysCache(proctup);

	PG_RETURN_TEXT_P(string_to_text(buf.data));
}

/*
 * pg_get_function_arguments
 *		Get a nicely-formatted list of arguments for a function.
 *		This is everything that would go between the parentheses in
 *		CREATE FUNCTION.
 */
Datum
pg_get_function_arguments(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	StringInfoData buf;
	HeapTuple	proctup;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		PG_RETURN_NULL();

	initStringInfo(&buf);

	(void) print_function_arguments(&buf, proctup, false, true);

	ReleaseSysCache(proctup);

	PG_RETURN_TEXT_P(string_to_text(buf.data));
}

/*
 * pg_get_function_identity_arguments
 *		Get a formatted list of arguments for a function.
 *		This is everything that would go between the parentheses in
 *		ALTER FUNCTION, etc.  In particular, don't print defaults.
 */
Datum
pg_get_function_identity_arguments(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	StringInfoData buf;
	HeapTuple	proctup;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		PG_RETURN_NULL();

	initStringInfo(&buf);

	(void) print_function_arguments(&buf, proctup, false, false);

	ReleaseSysCache(proctup);

	PG_RETURN_TEXT_P(string_to_text(buf.data));
}

/*
 * pg_get_function_result
 *		Get a nicely-formatted version of the result type of a function.
 *		This is what would appear after RETURNS in CREATE FUNCTION.
 */
Datum
pg_get_function_result(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	StringInfoData buf;
	HeapTuple	proctup;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		PG_RETURN_NULL();

	if (((Form_pg_proc) GETSTRUCT(proctup))->prokind == PROKIND_PROCEDURE)
	{
		ReleaseSysCache(proctup);
		PG_RETURN_NULL();
	}

	initStringInfo(&buf);

	print_function_rettype(&buf, proctup);

	ReleaseSysCache(proctup);

	PG_RETURN_TEXT_P(string_to_text(buf.data));
}

/*
 * Guts of pg_get_function_result: append the function's return type
 * to the specified buffer.
 */
static void
print_function_rettype(StringInfo buf, HeapTuple proctup)
{
	Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(proctup);
	int			ntabargs = 0;
	StringInfoData rbuf;

	initStringInfo(&rbuf);

	if (proc->proretset)
	{
		/* It might be a table function; try to print the arguments */
		appendStringInfoString(&rbuf, "TABLE(");
		ntabargs = print_function_arguments(&rbuf, proctup, true, false);
		if (ntabargs > 0)
			appendStringInfoChar(&rbuf, ')');
		else
			resetStringInfo(&rbuf);
	}

	if (ntabargs == 0)
	{
		/* Not a table function, so do the normal thing */
		if (proc->proretset)
			appendStringInfoString(&rbuf, "SETOF ");
		appendStringInfoString(&rbuf, format_type_be(proc->prorettype));
	}

	appendBinaryStringInfo(buf, rbuf.data, rbuf.len);
}

/*
 * Common code for pg_get_function_arguments and pg_get_function_result:
 * append the desired subset of arguments to buf.  We print only TABLE
 * arguments when print_table_args is true, and all the others when it's false.
 * We print argument defaults only if print_defaults is true.
 * Function return value is the number of arguments printed.
 */
static int
print_function_arguments(StringInfo buf, HeapTuple proctup,
						 bool print_table_args, bool print_defaults)
{
	Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(proctup);
	int			numargs;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	int			insertorderbyat = -1;
	int			argsprinted;
	int			inputargno;
	int			nlackdefaults;
	List	   *argdefaults = NIL;
	ListCell   *nextargdefault = NULL;
	int			i;

	numargs = get_func_arg_info(proctup,
								&argtypes, &argnames, &argmodes);

	nlackdefaults = numargs;
	if (print_defaults && proc->pronargdefaults > 0)
	{
		Datum		proargdefaults;
		bool		isnull;

		proargdefaults = SysCacheGetAttr(PROCOID, proctup,
										 Anum_pg_proc_proargdefaults,
										 &isnull);
		if (!isnull)
		{
			char	   *str;

			str = TextDatumGetCString(proargdefaults);
			argdefaults = castNode(List, stringToNode(str));
			pfree(str);
			nextargdefault = list_head(argdefaults);
			/* nlackdefaults counts only *input* arguments lacking defaults */
			nlackdefaults = proc->pronargs - list_length(argdefaults);
		}
	}

	/* Check for special treatment of ordered-set aggregates */
	if (proc->prokind == PROKIND_AGGREGATE)
	{
		HeapTuple	aggtup;
		Form_pg_aggregate agg;

		aggtup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(proc->oid));
		if (!HeapTupleIsValid(aggtup))
			elog(ERROR, "cache lookup failed for aggregate %u",
				 proc->oid);
		agg = (Form_pg_aggregate) GETSTRUCT(aggtup);
		if (AGGKIND_IS_ORDERED_SET(agg->aggkind))
			insertorderbyat = agg->aggnumdirectargs;
		ReleaseSysCache(aggtup);
	}

	argsprinted = 0;
	inputargno = 0;
	for (i = 0; i < numargs; i++)
	{
		Oid			argtype = argtypes[i];
		char	   *argname = argnames ? argnames[i] : NULL;
		char		argmode = argmodes ? argmodes[i] : PROARGMODE_IN;
		const char *modename;
		bool		isinput;

		switch (argmode)
		{
			case PROARGMODE_IN:

				/*
				 * For procedures, explicitly mark all argument modes, so as
				 * to avoid ambiguity with the SQL syntax for DROP PROCEDURE.
				 */
				if (proc->prokind == PROKIND_PROCEDURE)
					modename = "IN ";
				else
					modename = "";
				isinput = true;
				break;
			case PROARGMODE_INOUT:
				modename = "INOUT ";
				isinput = true;
				break;
			case PROARGMODE_OUT:
				modename = "OUT ";
				isinput = false;
				break;
			case PROARGMODE_VARIADIC:
				modename = "VARIADIC ";
				isinput = true;
				break;
			case PROARGMODE_TABLE:
				modename = "";
				isinput = false;
				break;
			default:
				elog(ERROR, "invalid parameter mode '%c'", argmode);
				modename = NULL;	/* keep compiler quiet */
				isinput = false;
				break;
		}
		if (isinput)
			inputargno++;		/* this is a 1-based counter */

		if (print_table_args != (argmode == PROARGMODE_TABLE))
			continue;

		if (argsprinted == insertorderbyat)
		{
			if (argsprinted)
				appendStringInfoChar(buf, ' ');
			appendStringInfoString(buf, "ORDER BY ");
		}
		else if (argsprinted)
			appendStringInfoString(buf, ", ");

		appendStringInfoString(buf, modename);
		if (argname && argname[0])
			appendStringInfo(buf, "%s ", quote_identifier(argname));
		appendStringInfoString(buf, format_type_be(argtype));
		if (print_defaults && isinput && inputargno > nlackdefaults)
		{
			Node	   *expr;

			Assert(nextargdefault != NULL);
			expr = (Node *) lfirst(nextargdefault);
			nextargdefault = lnext(argdefaults, nextargdefault);

			appendStringInfo(buf, " DEFAULT %s",
							 deparse_expression(expr, NIL, false, false));
		}
		argsprinted++;

		/* nasty hack: print the last arg twice for variadic ordered-set agg */
		if (argsprinted == insertorderbyat && i == numargs - 1)
		{
			i--;
			/* aggs shouldn't have defaults anyway, but just to be sure ... */
			print_defaults = false;
		}
	}

	return argsprinted;
}

static bool
is_input_argument(int nth, const char *argmodes)
{
	return (!argmodes
			|| argmodes[nth] == PROARGMODE_IN
			|| argmodes[nth] == PROARGMODE_INOUT
			|| argmodes[nth] == PROARGMODE_VARIADIC);
}

/*
 * Append used transformed types to specified buffer
 */
static void
print_function_trftypes(StringInfo buf, HeapTuple proctup)
{
	Oid		   *trftypes;
	int			ntypes;

	ntypes = get_func_trftypes(proctup, &trftypes);
	if (ntypes > 0)
	{
		int			i;

		appendStringInfoString(buf, " TRANSFORM ");
		for (i = 0; i < ntypes; i++)
		{
			if (i != 0)
				appendStringInfoString(buf, ", ");
			appendStringInfo(buf, "FOR TYPE %s", format_type_be(trftypes[i]));
		}
		appendStringInfoChar(buf, '\n');
	}
}

/*
 * Get textual representation of a function argument's default value.  The
 * second argument of this function is the argument number among all arguments
 * (i.e. proallargtypes, *not* proargtypes), starting with 1, because that's
 * how information_schema.sql uses it.
 */
Datum
pg_get_function_arg_default(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	int32		nth_arg = PG_GETARG_INT32(1);
	HeapTuple	proctup;
	Form_pg_proc proc;
	int			numargs;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	int			i;
	List	   *argdefaults;
	Node	   *node;
	char	   *str;
	int			nth_inputarg;
	Datum		proargdefaults;
	bool		isnull;
	int			nth_default;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		PG_RETURN_NULL();

	numargs = get_func_arg_info(proctup, &argtypes, &argnames, &argmodes);
	if (nth_arg < 1 || nth_arg > numargs || !is_input_argument(nth_arg - 1, argmodes))
	{
		ReleaseSysCache(proctup);
		PG_RETURN_NULL();
	}

	nth_inputarg = 0;
	for (i = 0; i < nth_arg; i++)
		if (is_input_argument(i, argmodes))
			nth_inputarg++;

	proargdefaults = SysCacheGetAttr(PROCOID, proctup,
									 Anum_pg_proc_proargdefaults,
									 &isnull);
	if (isnull)
	{
		ReleaseSysCache(proctup);
		PG_RETURN_NULL();
	}

	str = TextDatumGetCString(proargdefaults);
	argdefaults = castNode(List, stringToNode(str));
	pfree(str);

	proc = (Form_pg_proc) GETSTRUCT(proctup);

	/*
	 * Calculate index into proargdefaults: proargdefaults corresponds to the
	 * last N input arguments, where N = pronargdefaults.
	 */
	nth_default = nth_inputarg - 1 - (proc->pronargs - proc->pronargdefaults);

	if (nth_default < 0 || nth_default >= list_length(argdefaults))
	{
		ReleaseSysCache(proctup);
		PG_RETURN_NULL();
	}
	node = list_nth(argdefaults, nth_default);
	str = deparse_expression(node, NIL, false, false);

	ReleaseSysCache(proctup);

	PG_RETURN_TEXT_P(string_to_text(str));
}

static void
print_function_sqlbody(StringInfo buf, HeapTuple proctup)
{
	int			numargs;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	deparse_namespace dpns = {0};
	Datum		tmp;
	Node	   *n;

	dpns.funcname = pstrdup(NameStr(((Form_pg_proc) GETSTRUCT(proctup))->proname));
	numargs = get_func_arg_info(proctup,
								&argtypes, &argnames, &argmodes);
	dpns.numargs = numargs;
	dpns.argnames = argnames;

	tmp = SysCacheGetAttrNotNull(PROCOID, proctup, Anum_pg_proc_prosqlbody);
	n = stringToNode(TextDatumGetCString(tmp));

	if (IsA(n, List))
	{
		List	   *stmts;
		ListCell   *lc;

		stmts = linitial(castNode(List, n));

		appendStringInfoString(buf, "BEGIN ATOMIC\n");

		foreach(lc, stmts)
		{
			Query	   *query = lfirst_node(Query, lc);

			/* It seems advisable to get at least AccessShareLock on rels */
			AcquireRewriteLocks(query, false, false);
			get_query_def(query, buf, list_make1(&dpns), NULL, false,
						  PRETTYFLAG_INDENT, WRAP_COLUMN_DEFAULT, 1);
			appendStringInfoChar(buf, ';');
			appendStringInfoChar(buf, '\n');
		}

		appendStringInfoString(buf, "END");
	}
	else
	{
		Query	   *query = castNode(Query, n);

		/* It seems advisable to get at least AccessShareLock on rels */
		AcquireRewriteLocks(query, false, false);
		get_query_def(query, buf, list_make1(&dpns), NULL, false,
					  0, WRAP_COLUMN_DEFAULT, 0);
	}
}

Datum
pg_get_function_sqlbody(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	StringInfoData buf;
	HeapTuple	proctup;
	bool		isnull;

	initStringInfo(&buf);

	/* Look up the function */
	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		PG_RETURN_NULL();

	(void) SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_prosqlbody, &isnull);
	if (isnull)
	{
		ReleaseSysCache(proctup);
		PG_RETURN_NULL();
	}

	print_function_sqlbody(&buf, proctup);

	ReleaseSysCache(proctup);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}


/*
 * deparse_expression			- General utility for deparsing expressions
 *
 * calls deparse_expression_pretty with all prettyPrinting disabled
 */
char *
deparse_expression(Node *expr, List *dpcontext,
				   bool forceprefix, bool showimplicit)
{
	return deparse_expression_pretty(expr, dpcontext, forceprefix,
									 showimplicit, 0, 0);
}

/* ----------
 * deparse_expression_pretty	- General utility for deparsing expressions
 *
 * expr is the node tree to be deparsed.  It must be a transformed expression
 * tree (ie, not the raw output of gram.y).
 *
 * dpcontext is a list of deparse_namespace nodes representing the context
 * for interpreting Vars in the node tree.  It can be NIL if no Vars are
 * expected.
 *
 * forceprefix is true to force all Vars to be prefixed with their table names.
 *
 * showimplicit is true to force all implicit casts to be shown explicitly.
 *
 * Tries to pretty up the output according to prettyFlags and startIndent.
 *
 * The result is a palloc'd string.
 * ----------
 */
static char *
deparse_expression_pretty(Node *expr, List *dpcontext,
						  bool forceprefix, bool showimplicit,
						  int prettyFlags, int startIndent)
{
	StringInfoData buf;
	deparse_context context;

	initStringInfo(&buf);
	context.buf = &buf;
	context.namespaces = dpcontext;
	context.resultDesc = NULL;
	context.targetList = NIL;
	context.windowClause = NIL;
	context.varprefix = forceprefix;
	context.prettyFlags = prettyFlags;
	context.wrapColumn = WRAP_COLUMN_DEFAULT;
	context.indentLevel = startIndent;
	context.colNamesVisible = true;
	context.inGroupBy = false;
	context.varInOrderBy = false;
	context.appendparents = NULL;

	get_rule_expr(expr, &context, showimplicit);

	return buf.data;
}

/* ----------
 * deparse_context_for			- Build deparse context for a single relation
 *
 * Given the reference name (alias) and OID of a relation, build deparsing
 * context for an expression referencing only that relation (as varno 1,
 * varlevelsup 0).  This is sufficient for many uses of deparse_expression.
 * ----------
 */
List *
deparse_context_for(const char *aliasname, Oid relid)
{
	deparse_namespace *dpns;
	RangeTblEntry *rte;

	dpns = (deparse_namespace *) palloc0(sizeof(deparse_namespace));

	/* Build a minimal RTE for the rel */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = relid;
	rte->relkind = RELKIND_RELATION;	/* no need for exactness here */
	rte->rellockmode = AccessShareLock;
	rte->alias = makeAlias(aliasname, NIL);
	rte->eref = rte->alias;
	rte->lateral = false;
	rte->inh = false;
	rte->inFromCl = true;

	/* Build one-element rtable */
	dpns->rtable = list_make1(rte);
	dpns->subplans = NIL;
	dpns->ctes = NIL;
	dpns->appendrels = NULL;
	set_rtable_names(dpns, NIL, NULL);
	set_simple_column_names(dpns);

	/* Return a one-deep namespace stack */
	return list_make1(dpns);
}

/*
 * deparse_context_for_plan_tree - Build deparse context for a Plan tree
 *
 * When deparsing an expression in a Plan tree, we use the plan's rangetable
 * to resolve names of simple Vars.  The initialization of column names for
 * this is rather expensive if the rangetable is large, and it'll be the same
 * for every expression in the Plan tree; so we do it just once and re-use
 * the result of this function for each expression.  (Note that the result
 * is not usable until set_deparse_context_plan() is applied to it.)
 *
 * In addition to the PlannedStmt, pass the per-RTE alias names
 * assigned by a previous call to select_rtable_names_for_explain.
 */
List *
deparse_context_for_plan_tree(PlannedStmt *pstmt, List *rtable_names)
{
	deparse_namespace *dpns;

	dpns = (deparse_namespace *) palloc0(sizeof(deparse_namespace));

	/* Initialize fields that stay the same across the whole plan tree */
	dpns->rtable = pstmt->rtable;
	dpns->rtable_names = rtable_names;
	dpns->subplans = pstmt->subplans;
	dpns->ctes = NIL;
	if (pstmt->appendRelations)
	{
		/* Set up the array, indexed by child relid */
		int			ntables = list_length(dpns->rtable);
		ListCell   *lc;

		dpns->appendrels = (AppendRelInfo **)
			palloc0((ntables + 1) * sizeof(AppendRelInfo *));
		foreach(lc, pstmt->appendRelations)
		{
			AppendRelInfo *appinfo = lfirst_node(AppendRelInfo, lc);
			Index		crelid = appinfo->child_relid;

			Assert(crelid > 0 && crelid <= ntables);
			Assert(dpns->appendrels[crelid] == NULL);
			dpns->appendrels[crelid] = appinfo;
		}
	}
	else
		dpns->appendrels = NULL;	/* don't need it */

	/*
	 * Set up column name aliases, ignoring any join RTEs; they don't matter
	 * because plan trees don't contain any join alias Vars.
	 */
	set_simple_column_names(dpns);

	/* Return a one-deep namespace stack */
	return list_make1(dpns);
}

/*
 * set_deparse_context_plan - Specify Plan node containing expression
 *
 * When deparsing an expression in a Plan tree, we might have to resolve
 * OUTER_VAR, INNER_VAR, or INDEX_VAR references.  To do this, the caller must
 * provide the parent Plan node.  Then OUTER_VAR and INNER_VAR references
 * can be resolved by drilling down into the left and right child plans.
 * Similarly, INDEX_VAR references can be resolved by reference to the
 * indextlist given in a parent IndexOnlyScan node, or to the scan tlist in
 * ForeignScan and CustomScan nodes.  (Note that we don't currently support
 * deparsing of indexquals in regular IndexScan or BitmapIndexScan nodes;
 * for those, we can only deparse the indexqualorig fields, which won't
 * contain INDEX_VAR Vars.)
 *
 * The ancestors list is a list of the Plan's parent Plan and SubPlan nodes,
 * the most-closely-nested first.  This is needed to resolve PARAM_EXEC
 * Params.  Note we assume that all the Plan nodes share the same rtable.
 *
 * For a ModifyTable plan, we might also need to resolve references to OLD/NEW
 * variables in the RETURNING list, so we copy the alias names of the OLD and
 * NEW rows from the ModifyTable plan node.
 *
 * Once this function has been called, deparse_expression() can be called on
 * subsidiary expression(s) of the specified Plan node.  To deparse
 * expressions of a different Plan node in the same Plan tree, re-call this
 * function to identify the new parent Plan node.
 *
 * The result is the same List passed in; this is a notational convenience.
 */
List *
set_deparse_context_plan(List *dpcontext, Plan *plan, List *ancestors)
{
	deparse_namespace *dpns;

	/* Should always have one-entry namespace list for Plan deparsing */
	Assert(list_length(dpcontext) == 1);
	dpns = (deparse_namespace *) linitial(dpcontext);

	/* Set our attention on the specific plan node passed in */
	dpns->ancestors = ancestors;
	set_deparse_plan(dpns, plan);

	/* For ModifyTable, set aliases for OLD and NEW in RETURNING */
	if (IsA(plan, ModifyTable))
	{
		dpns->ret_old_alias = ((ModifyTable *) plan)->returningOldAlias;
		dpns->ret_new_alias = ((ModifyTable *) plan)->returningNewAlias;
	}

	return dpcontext;
}

/*
 * select_rtable_names_for_explain	- Select RTE aliases for EXPLAIN
 *
 * Determine the relation aliases we'll use during an EXPLAIN operation.
 * This is just a frontend to set_rtable_names.  We have to expose the aliases
 * to EXPLAIN because EXPLAIN needs to know the right alias names to print.
 */
List *
select_rtable_names_for_explain(List *rtable, Bitmapset *rels_used)
{
	deparse_namespace dpns;

	memset(&dpns, 0, sizeof(dpns));
	dpns.rtable = rtable;
	dpns.subplans = NIL;
	dpns.ctes = NIL;
	dpns.appendrels = NULL;
	set_rtable_names(&dpns, NIL, rels_used);
	/* We needn't bother computing column aliases yet */

	return dpns.rtable_names;
}

/*
 * set_rtable_names: select RTE aliases to be used in printing a query
 *
 * We fill in dpns->rtable_names with a list of names that is one-for-one with
 * the already-filled dpns->rtable list.  Each RTE name is unique among those
 * in the new namespace plus any ancestor namespaces listed in
 * parent_namespaces.
 *
 * If rels_used isn't NULL, only RTE indexes listed in it are given aliases.
 *
 * Note that this function is only concerned with relation names, not column
 * names.
 */
static void
set_rtable_names(deparse_namespace *dpns, List *parent_namespaces,
				 Bitmapset *rels_used)
{
	HASHCTL		hash_ctl;
	HTAB	   *names_hash;
	NameHashEntry *hentry;
	bool		found;
	int			rtindex;
	ListCell   *lc;

	dpns->rtable_names = NIL;
	/* nothing more to do if empty rtable */
	if (dpns->rtable == NIL)
		return;

	/*
	 * We use a hash table to hold known names, so that this process is O(N)
	 * not O(N^2) for N names.
	 */
	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(NameHashEntry);
	hash_ctl.hcxt = CurrentMemoryContext;
	names_hash = hash_create("set_rtable_names names",
							 list_length(dpns->rtable),
							 &hash_ctl,
							 HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

	/* Preload the hash table with names appearing in parent_namespaces */
	foreach(lc, parent_namespaces)
	{
		deparse_namespace *olddpns = (deparse_namespace *) lfirst(lc);
		ListCell   *lc2;

		foreach(lc2, olddpns->rtable_names)
		{
			char	   *oldname = (char *) lfirst(lc2);

			if (oldname == NULL)
				continue;
			hentry = (NameHashEntry *) hash_search(names_hash,
												   oldname,
												   HASH_ENTER,
												   &found);
			/* we do not complain about duplicate names in parent namespaces */
			hentry->counter = 0;
		}
	}

	/* Now we can scan the rtable */
	rtindex = 1;
	foreach(lc, dpns->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		char	   *refname;

		/* Just in case this takes an unreasonable amount of time ... */
		CHECK_FOR_INTERRUPTS();

		if (rels_used && !bms_is_member(rtindex, rels_used))
		{
			/* Ignore unreferenced RTE */
			refname = NULL;
		}
		else if (rte->alias)
		{
			/* If RTE has a user-defined alias, prefer that */
			refname = rte->alias->aliasname;
		}
		else if (rte->rtekind == RTE_RELATION)
		{
			/* Use the current actual name of the relation */
			refname = get_rel_name(rte->relid);
		}
		else if (rte->rtekind == RTE_JOIN)
		{
			/* Unnamed join has no refname */
			refname = NULL;
		}
		else
		{
			/* Otherwise use whatever the parser assigned */
			refname = rte->eref->aliasname;
		}

		/*
		 * If the selected name isn't unique, append digits to make it so, and
		 * make a new hash entry for it once we've got a unique name.  For a
		 * very long input name, we might have to truncate to stay within
		 * NAMEDATALEN.
		 */
		if (refname)
		{
			hentry = (NameHashEntry *) hash_search(names_hash,
												   refname,
												   HASH_ENTER,
												   &found);
			if (found)
			{
				/* Name already in use, must choose a new one */
				int			refnamelen = strlen(refname);
				char	   *modname = (char *) palloc(refnamelen + 16);
				NameHashEntry *hentry2;

				do
				{
					hentry->counter++;
					for (;;)
					{
						memcpy(modname, refname, refnamelen);
						sprintf(modname + refnamelen, "_%d", hentry->counter);
						if (strlen(modname) < NAMEDATALEN)
							break;
						/* drop chars from refname to keep all the digits */
						refnamelen = pg_mbcliplen(refname, refnamelen,
												  refnamelen - 1);
					}
					hentry2 = (NameHashEntry *) hash_search(names_hash,
															modname,
															HASH_ENTER,
															&found);
				} while (found);
				hentry2->counter = 0;	/* init new hash entry */
				refname = modname;
			}
			else
			{
				/* Name not previously used, need only initialize hentry */
				hentry->counter = 0;
			}
		}

		dpns->rtable_names = lappend(dpns->rtable_names, refname);
		rtindex++;
	}

	hash_destroy(names_hash);
}

/*
 * set_deparse_for_query: set up deparse_namespace for deparsing a Query tree
 *
 * For convenience, this is defined to initialize the deparse_namespace struct
 * from scratch.
 */
static void
set_deparse_for_query(deparse_namespace *dpns, Query *query,
					  List *parent_namespaces)
{
	ListCell   *lc;
	ListCell   *lc2;

	/* Initialize *dpns and fill rtable/ctes links */
	memset(dpns, 0, sizeof(deparse_namespace));
	dpns->rtable = query->rtable;
	dpns->subplans = NIL;
	dpns->ctes = query->cteList;
	dpns->appendrels = NULL;
	dpns->ret_old_alias = query->returningOldAlias;
	dpns->ret_new_alias = query->returningNewAlias;

	/* Assign a unique relation alias to each RTE */
	set_rtable_names(dpns, parent_namespaces, NULL);

	/* Initialize dpns->rtable_columns to contain zeroed structs */
	dpns->rtable_columns = NIL;
	while (list_length(dpns->rtable_columns) < list_length(dpns->rtable))
		dpns->rtable_columns = lappend(dpns->rtable_columns,
									   palloc0(sizeof(deparse_columns)));

	/* If it's a utility query, it won't have a jointree */
	if (query->jointree)
	{
		/* Detect whether global uniqueness of USING names is needed */
		dpns->unique_using =
			has_dangerous_join_using(dpns, (Node *) query->jointree);

		/*
		 * Select names for columns merged by USING, via a recursive pass over
		 * the query jointree.
		 */
		set_using_names(dpns, (Node *) query->jointree, NIL);
	}

	/*
	 * Now assign remaining column aliases for each RTE.  We do this in a
	 * linear scan of the rtable, so as to process RTEs whether or not they
	 * are in the jointree (we mustn't miss NEW.*, INSERT target relations,
	 * etc).  JOIN RTEs must be processed after their children, but this is
	 * okay because they appear later in the rtable list than their children
	 * (cf Asserts in identify_join_columns()).
	 */
	forboth(lc, dpns->rtable, lc2, dpns->rtable_columns)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		deparse_columns *colinfo = (deparse_columns *) lfirst(lc2);

		if (rte->rtekind == RTE_JOIN)
			set_join_column_names(dpns, rte, colinfo);
		else
			set_relation_column_names(dpns, rte, colinfo);
	}
}

/*
 * set_simple_column_names: fill in column aliases for non-query situations
 *
 * This handles EXPLAIN and cases where we only have relation RTEs.  Without
 * a join tree, we can't do anything smart about join RTEs, but we don't
 * need to, because EXPLAIN should never see join alias Vars anyway.
 * If we find a join RTE we'll just skip it, leaving its deparse_columns
 * struct all-zero.  If somehow we try to deparse a join alias Var, we'll
 * error out cleanly because the struct's num_cols will be zero.
 */
static void
set_simple_column_names(deparse_namespace *dpns)
{
	ListCell   *lc;
	ListCell   *lc2;

	/* Initialize dpns->rtable_columns to contain zeroed structs */
	dpns->rtable_columns = NIL;
	while (list_length(dpns->rtable_columns) < list_length(dpns->rtable))
		dpns->rtable_columns = lappend(dpns->rtable_columns,
									   palloc0(sizeof(deparse_columns)));

	/* Assign unique column aliases within each non-join RTE */
	forboth(lc, dpns->rtable, lc2, dpns->rtable_columns)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		deparse_columns *colinfo = (deparse_columns *) lfirst(lc2);

		if (rte->rtekind != RTE_JOIN)
			set_relation_column_names(dpns, rte, colinfo);
	}
}

/*
 * has_dangerous_join_using: search jointree for unnamed JOIN USING
 *
 * Merged columns of a JOIN USING may act differently from either of the input
 * columns, either because they are merged with COALESCE (in a FULL JOIN) or
 * because an implicit coercion of the underlying input column is required.
 * In such a case the column must be referenced as a column of the JOIN not as
 * a column of either input.  And this is problematic if the join is unnamed
 * (alias-less): we cannot qualify the column's name with an RTE name, since
 * there is none.  (Forcibly assigning an alias to the join is not a solution,
 * since that will prevent legal references to tables below the join.)
 * To ensure that every column in the query is unambiguously referenceable,
 * we must assign such merged columns names that are globally unique across
 * the whole query, aliasing other columns out of the way as necessary.
 *
 * Because the ensuing re-aliasing is fairly damaging to the readability of
 * the query, we don't do this unless we have to.  So, we must pre-scan
 * the join tree to see if we have to, before starting set_using_names().
 */
static bool
has_dangerous_join_using(deparse_namespace *dpns, Node *jtnode)
{
	if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do here */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *lc;

		foreach(lc, f->fromlist)
		{
			if (has_dangerous_join_using(dpns, (Node *) lfirst(lc)))
				return true;
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/* Is it an unnamed JOIN with USING? */
		if (j->alias == NULL && j->usingClause)
		{
			/*
			 * Yes, so check each join alias var to see if any of them are not
			 * simple references to underlying columns.  If so, we have a
			 * dangerous situation and must pick unique aliases.
			 */
			RangeTblEntry *jrte = rt_fetch(j->rtindex, dpns->rtable);

			/* We need only examine the merged columns */
			for (int i = 0; i < jrte->joinmergedcols; i++)
			{
				Node	   *aliasvar = list_nth(jrte->joinaliasvars, i);

				if (!IsA(aliasvar, Var))
					return true;
			}
		}

		/* Nope, but inspect children */
		if (has_dangerous_join_using(dpns, j->larg))
			return true;
		if (has_dangerous_join_using(dpns, j->rarg))
			return true;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return false;
}

/*
 * set_using_names: select column aliases to be used for merged USING columns
 *
 * We do this during a recursive descent of the query jointree.
 * dpns->unique_using must already be set to determine the global strategy.
 *
 * Column alias info is saved in the dpns->rtable_columns list, which is
 * assumed to be filled with pre-zeroed deparse_columns structs.
 *
 * parentUsing is a list of all USING aliases assigned in parent joins of
 * the current jointree node.  (The passed-in list must not be modified.)
 *
 * Note that we do not use per-deparse_columns hash tables in this function.
 * The number of names that need to be assigned should be small enough that
 * we don't need to trouble with that.
 */
static void
set_using_names(deparse_namespace *dpns, Node *jtnode, List *parentUsing)
{
	if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do now */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *lc;

		foreach(lc, f->fromlist)
			set_using_names(dpns, (Node *) lfirst(lc), parentUsing);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		RangeTblEntry *rte = rt_fetch(j->rtindex, dpns->rtable);
		deparse_columns *colinfo = deparse_columns_fetch(j->rtindex, dpns);
		int		   *leftattnos;
		int		   *rightattnos;
		deparse_columns *leftcolinfo;
		deparse_columns *rightcolinfo;
		int			i;
		ListCell   *lc;

		/* Get info about the shape of the join */
		identify_join_columns(j, rte, colinfo);
		leftattnos = colinfo->leftattnos;
		rightattnos = colinfo->rightattnos;

		/* Look up the not-yet-filled-in child deparse_columns structs */
		leftcolinfo = deparse_columns_fetch(colinfo->leftrti, dpns);
		rightcolinfo = deparse_columns_fetch(colinfo->rightrti, dpns);

		/*
		 * If this join is unnamed, then we cannot substitute new aliases at
		 * this level, so any name requirements pushed down to here must be
		 * pushed down again to the children.
		 */
		if (rte->alias == NULL)
		{
			for (i = 0; i < colinfo->num_cols; i++)
			{
				char	   *colname = colinfo->colnames[i];

				if (colname == NULL)
					continue;

				/* Push down to left column, unless it's a system column */
				if (leftattnos[i] > 0)
				{
					expand_colnames_array_to(leftcolinfo, leftattnos[i]);
					leftcolinfo->colnames[leftattnos[i] - 1] = colname;
				}

				/* Same on the righthand side */
				if (rightattnos[i] > 0)
				{
					expand_colnames_array_to(rightcolinfo, rightattnos[i]);
					rightcolinfo->colnames[rightattnos[i] - 1] = colname;
				}
			}
		}

		/*
		 * If there's a USING clause, select the USING column names and push
		 * those names down to the children.  We have two strategies:
		 *
		 * If dpns->unique_using is true, we force all USING names to be
		 * unique across the whole query level.  In principle we'd only need
		 * the names of dangerous USING columns to be globally unique, but to
		 * safely assign all USING names in a single pass, we have to enforce
		 * the same uniqueness rule for all of them.  However, if a USING
		 * column's name has been pushed down from the parent, we should use
		 * it as-is rather than making a uniqueness adjustment.  This is
		 * necessary when we're at an unnamed join, and it creates no risk of
		 * ambiguity.  Also, if there's a user-written output alias for a
		 * merged column, we prefer to use that rather than the input name;
		 * this simplifies the logic and seems likely to lead to less aliasing
		 * overall.
		 *
		 * If dpns->unique_using is false, we only need USING names to be
		 * unique within their own join RTE.  We still need to honor
		 * pushed-down names, though.
		 *
		 * Though significantly different in results, these two strategies are
		 * implemented by the same code, with only the difference of whether
		 * to put assigned names into dpns->using_names.
		 */
		if (j->usingClause)
		{
			/* Copy the input parentUsing list so we don't modify it */
			parentUsing = list_copy(parentUsing);

			/* USING names must correspond to the first join output columns */
			expand_colnames_array_to(colinfo, list_length(j->usingClause));
			i = 0;
			foreach(lc, j->usingClause)
			{
				char	   *colname = strVal(lfirst(lc));

				/* Assert it's a merged column */
				Assert(leftattnos[i] != 0 && rightattnos[i] != 0);

				/* Adopt passed-down name if any, else select unique name */
				if (colinfo->colnames[i] != NULL)
					colname = colinfo->colnames[i];
				else
				{
					/* Prefer user-written output alias if any */
					if (rte->alias && i < list_length(rte->alias->colnames))
						colname = strVal(list_nth(rte->alias->colnames, i));
					/* Make it appropriately unique */
					colname = make_colname_unique(colname, dpns, colinfo);
					if (dpns->unique_using)
						dpns->using_names = lappend(dpns->using_names,
													colname);
					/* Save it as output column name, too */
					colinfo->colnames[i] = colname;
				}

				/* Remember selected names for use later */
				colinfo->usingNames = lappend(colinfo->usingNames, colname);
				parentUsing = lappend(parentUsing, colname);

				/* Push down to left column, unless it's a system column */
				if (leftattnos[i] > 0)
				{
					expand_colnames_array_to(leftcolinfo, leftattnos[i]);
					leftcolinfo->colnames[leftattnos[i] - 1] = colname;
				}

				/* Same on the righthand side */
				if (rightattnos[i] > 0)
				{
					expand_colnames_array_to(rightcolinfo, rightattnos[i]);
					rightcolinfo->colnames[rightattnos[i] - 1] = colname;
				}

				i++;
			}
		}

		/* Mark child deparse_columns structs with correct parentUsing info */
		leftcolinfo->parentUsing = parentUsing;
		rightcolinfo->parentUsing = parentUsing;

		/* Now recursively assign USING column names in children */
		set_using_names(dpns, j->larg, parentUsing);
		set_using_names(dpns, j->rarg, parentUsing);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * set_relation_column_names: select column aliases for a non-join RTE
 *
 * Column alias info is saved in *colinfo, which is assumed to be pre-zeroed.
 * If any colnames entries are already filled in, those override local
 * choices.
 */
static void
set_relation_column_names(deparse_namespace *dpns, RangeTblEntry *rte,
						  deparse_columns *colinfo)
{
	int			ncolumns;
	char	  **real_colnames;
	bool		changed_any;
	int			noldcolumns;
	int			i;
	int			j;

	/*
	 * Construct an array of the current "real" column names of the RTE.
	 * real_colnames[] will be indexed by physical column number, with NULL
	 * entries for dropped columns.
	 */
	if (rte->rtekind == RTE_RELATION)
	{
		/* Relation --- look to the system catalogs for up-to-date info */
		Relation	rel;
		TupleDesc	tupdesc;

		rel = relation_open(rte->relid, AccessShareLock);
		tupdesc = RelationGetDescr(rel);

		ncolumns = tupdesc->natts;
		real_colnames = (char **) palloc(ncolumns * sizeof(char *));

		for (i = 0; i < ncolumns; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (attr->attisdropped)
				real_colnames[i] = NULL;
			else
				real_colnames[i] = pstrdup(NameStr(attr->attname));
		}
		relation_close(rel, AccessShareLock);
	}
	else
	{
		/* Otherwise get the column names from eref or expandRTE() */
		List	   *colnames;
		ListCell   *lc;

		/*
		 * Functions returning composites have the annoying property that some
		 * of the composite type's columns might have been dropped since the
		 * query was parsed.  If possible, use expandRTE() to handle that
		 * case, since it has the tedious logic needed to find out about
		 * dropped columns.  However, if we're explaining a plan, then we
		 * don't have rte->functions because the planner thinks that won't be
		 * needed later, and that breaks expandRTE().  So in that case we have
		 * to rely on rte->eref, which may lead us to report a dropped
		 * column's old name; that seems close enough for EXPLAIN's purposes.
		 *
		 * For non-RELATION, non-FUNCTION RTEs, we can just look at rte->eref,
		 * which should be sufficiently up-to-date: no other RTE types can
		 * have columns get dropped from under them after parsing.
		 */
		if (rte->rtekind == RTE_FUNCTION && rte->functions != NIL)
		{
			/* Since we're not creating Vars, rtindex etc. don't matter */
			expandRTE(rte, 1, 0, VAR_RETURNING_DEFAULT, -1,
					  true /* include dropped */ , &colnames, NULL);
		}
		else
			colnames = rte->eref->colnames;

		ncolumns = list_length(colnames);
		real_colnames = (char **) palloc(ncolumns * sizeof(char *));

		i = 0;
		foreach(lc, colnames)
		{
			/*
			 * If the column name we find here is an empty string, then it's a
			 * dropped column, so change to NULL.
			 */
			char	   *cname = strVal(lfirst(lc));

			if (cname[0] == '\0')
				cname = NULL;
			real_colnames[i] = cname;
			i++;
		}
	}

	/*
	 * Ensure colinfo->colnames has a slot for each column.  (It could be long
	 * enough already, if we pushed down a name for the last column.)  Note:
	 * it's possible that there are now more columns than there were when the
	 * query was parsed, ie colnames could be longer than rte->eref->colnames.
	 * We must assign unique aliases to the new columns too, else there could
	 * be unresolved conflicts when the view/rule is reloaded.
	 */
	expand_colnames_array_to(colinfo, ncolumns);
	Assert(colinfo->num_cols == ncolumns);

	/*
	 * Make sufficiently large new_colnames and is_new_col arrays, too.
	 *
	 * Note: because we leave colinfo->num_new_cols zero until after the loop,
	 * colname_is_unique will not consult that array, which is fine because it
	 * would only be duplicate effort.
	 */
	colinfo->new_colnames = (char **) palloc(ncolumns * sizeof(char *));
	colinfo->is_new_col = (bool *) palloc(ncolumns * sizeof(bool));

	/* If the RTE is wide enough, use a hash table to avoid O(N^2) costs */
	build_colinfo_names_hash(colinfo);

	/*
	 * Scan the columns, select a unique alias for each one, and store it in
	 * colinfo->colnames and colinfo->new_colnames.  The former array has NULL
	 * entries for dropped columns, the latter omits them.  Also mark
	 * new_colnames entries as to whether they are new since parse time; this
	 * is the case for entries beyond the length of rte->eref->colnames.
	 */
	noldcolumns = list_length(rte->eref->colnames);
	changed_any = false;
	j = 0;
	for (i = 0; i < ncolumns; i++)
	{
		char	   *real_colname = real_colnames[i];
		char	   *colname = colinfo->colnames[i];

		/* Skip dropped columns */
		if (real_colname == NULL)
		{
			Assert(colname == NULL);	/* colnames[i] is already NULL */
			continue;
		}

		/* If alias already assigned, that's what to use */
		if (colname == NULL)
		{
			/* If user wrote an alias, prefer that over real column name */
			if (rte->alias && i < list_length(rte->alias->colnames))
				colname = strVal(list_nth(rte->alias->colnames, i));
			else
				colname = real_colname;

			/* Unique-ify and insert into colinfo */
			colname = make_colname_unique(colname, dpns, colinfo);

			colinfo->colnames[i] = colname;
			add_to_names_hash(colinfo, colname);
		}

		/* Put names of non-dropped columns in new_colnames[] too */
		colinfo->new_colnames[j] = colname;
		/* And mark them as new or not */
		colinfo->is_new_col[j] = (i >= noldcolumns);
		j++;

		/* Remember if any assigned aliases differ from "real" name */
		if (!changed_any && strcmp(colname, real_colname) != 0)
			changed_any = true;
	}

	/* We're now done needing the colinfo's names_hash */
	destroy_colinfo_names_hash(colinfo);

	/*
	 * Set correct length for new_colnames[] array.  (Note: if columns have
	 * been added, colinfo->num_cols includes them, which is not really quite
	 * right but is harmless, since any new columns must be at the end where
	 * they won't affect varattnos of pre-existing columns.)
	 */
	colinfo->num_new_cols = j;

	/*
	 * For a relation RTE, we need only print the alias column names if any
	 * are different from the underlying "real" names.  For a function RTE,
	 * always emit a complete column alias list; this is to protect against
	 * possible instability of the default column names (eg, from altering
	 * parameter names).  For tablefunc RTEs, we never print aliases, because
	 * the column names are part of the clause itself.  For other RTE types,
	 * print if we changed anything OR if there were user-written column
	 * aliases (since the latter would be part of the underlying "reality").
	 */
	if (rte->rtekind == RTE_RELATION)
		colinfo->printaliases = changed_any;
	else if (rte->rtekind == RTE_FUNCTION)
		colinfo->printaliases = true;
	else if (rte->rtekind == RTE_TABLEFUNC)
		colinfo->printaliases = false;
	else if (rte->alias && rte->alias->colnames != NIL)
		colinfo->printaliases = true;
	else
		colinfo->printaliases = changed_any;
}

/*
 * set_join_column_names: select column aliases for a join RTE
 *
 * Column alias info is saved in *colinfo, which is assumed to be pre-zeroed.
 * If any colnames entries are already filled in, those override local
 * choices.  Also, names for USING columns were already chosen by
 * set_using_names().  We further expect that column alias selection has been
 * completed for both input RTEs.
 */
static void
set_join_column_names(deparse_namespace *dpns, RangeTblEntry *rte,
					  deparse_columns *colinfo)
{
	deparse_columns *leftcolinfo;
	deparse_columns *rightcolinfo;
	bool		changed_any;
	int			noldcolumns;
	int			nnewcolumns;
	Bitmapset  *leftmerged = NULL;
	Bitmapset  *rightmerged = NULL;
	int			i;
	int			j;
	int			ic;
	int			jc;

	/* Look up the previously-filled-in child deparse_columns structs */
	leftcolinfo = deparse_columns_fetch(colinfo->leftrti, dpns);
	rightcolinfo = deparse_columns_fetch(colinfo->rightrti, dpns);

	/*
	 * Ensure colinfo->colnames has a slot for each column.  (It could be long
	 * enough already, if we pushed down a name for the last column.)  Note:
	 * it's possible that one or both inputs now have more columns than there
	 * were when the query was parsed, but we'll deal with that below.  We
	 * only need entries in colnames for pre-existing columns.
	 */
	noldcolumns = list_length(rte->eref->colnames);
	expand_colnames_array_to(colinfo, noldcolumns);
	Assert(colinfo->num_cols == noldcolumns);

	/* If the RTE is wide enough, use a hash table to avoid O(N^2) costs */
	build_colinfo_names_hash(colinfo);

	/*
	 * Scan the join output columns, select an alias for each one, and store
	 * it in colinfo->colnames.  If there are USING columns, set_using_names()
	 * already selected their names, so we can start the loop at the first
	 * non-merged column.
	 */
	changed_any = false;
	for (i = list_length(colinfo->usingNames); i < noldcolumns; i++)
	{
		char	   *colname = colinfo->colnames[i];
		char	   *real_colname;

		/* Join column must refer to at least one input column */
		Assert(colinfo->leftattnos[i] != 0 || colinfo->rightattnos[i] != 0);

		/* Get the child column name */
		if (colinfo->leftattnos[i] > 0)
			real_colname = leftcolinfo->colnames[colinfo->leftattnos[i] - 1];
		else if (colinfo->rightattnos[i] > 0)
			real_colname = rightcolinfo->colnames[colinfo->rightattnos[i] - 1];
		else
		{
			/* We're joining system columns --- use eref name */
			real_colname = strVal(list_nth(rte->eref->colnames, i));
		}

		/* If child col has been dropped, no need to assign a join colname */
		if (real_colname == NULL)
		{
			colinfo->colnames[i] = NULL;
			continue;
		}

		/* In an unnamed join, just report child column names as-is */
		if (rte->alias == NULL)
		{
			colinfo->colnames[i] = real_colname;
			add_to_names_hash(colinfo, real_colname);
			continue;
		}

		/* If alias already assigned, that's what to use */
		if (colname == NULL)
		{
			/* If user wrote an alias, prefer that over real column name */
			if (rte->alias && i < list_length(rte->alias->colnames))
				colname = strVal(list_nth(rte->alias->colnames, i));
			else
				colname = real_colname;

			/* Unique-ify and insert into colinfo */
			colname = make_colname_unique(colname, dpns, colinfo);

			colinfo->colnames[i] = colname;
			add_to_names_hash(colinfo, colname);
		}

		/* Remember if any assigned aliases differ from "real" name */
		if (!changed_any && strcmp(colname, real_colname) != 0)
			changed_any = true;
	}

	/*
	 * Calculate number of columns the join would have if it were re-parsed
	 * now, and create storage for the new_colnames and is_new_col arrays.
	 *
	 * Note: colname_is_unique will be consulting new_colnames[] during the
	 * loops below, so its not-yet-filled entries must be zeroes.
	 */
	nnewcolumns = leftcolinfo->num_new_cols + rightcolinfo->num_new_cols -
		list_length(colinfo->usingNames);
	colinfo->num_new_cols = nnewcolumns;
	colinfo->new_colnames = (char **) palloc0(nnewcolumns * sizeof(char *));
	colinfo->is_new_col = (bool *) palloc0(nnewcolumns * sizeof(bool));

	/*
	 * Generating the new_colnames array is a bit tricky since any new columns
	 * added since parse time must be inserted in the right places.  This code
	 * must match the parser, which will order a join's columns as merged
	 * columns first (in USING-clause order), then non-merged columns from the
	 * left input (in attnum order), then non-merged columns from the right
	 * input (ditto).  If one of the inputs is itself a join, its columns will
	 * be ordered according to the same rule, which means newly-added columns
	 * might not be at the end.  We can figure out what's what by consulting
	 * the leftattnos and rightattnos arrays plus the input is_new_col arrays.
	 *
	 * In these loops, i indexes leftattnos/rightattnos (so it's join varattno
	 * less one), j indexes new_colnames/is_new_col, and ic/jc have similar
	 * meanings for the current child RTE.
	 */

	/* Handle merged columns; they are first and can't be new */
	i = j = 0;
	while (i < noldcolumns &&
		   colinfo->leftattnos[i] != 0 &&
		   colinfo->rightattnos[i] != 0)
	{
		/* column name is already determined and known unique */
		colinfo->new_colnames[j] = colinfo->colnames[i];
		colinfo->is_new_col[j] = false;

		/* build bitmapsets of child attnums of merged columns */
		if (colinfo->leftattnos[i] > 0)
			leftmerged = bms_add_member(leftmerged, colinfo->leftattnos[i]);
		if (colinfo->rightattnos[i] > 0)
			rightmerged = bms_add_member(rightmerged, colinfo->rightattnos[i]);

		i++, j++;
	}

	/* Handle non-merged left-child columns */
	ic = 0;
	for (jc = 0; jc < leftcolinfo->num_new_cols; jc++)
	{
		char	   *child_colname = leftcolinfo->new_colnames[jc];

		if (!leftcolinfo->is_new_col[jc])
		{
			/* Advance ic to next non-dropped old column of left child */
			while (ic < leftcolinfo->num_cols &&
				   leftcolinfo->colnames[ic] == NULL)
				ic++;
			Assert(ic < leftcolinfo->num_cols);
			ic++;
			/* If it is a merged column, we already processed it */
			if (bms_is_member(ic, leftmerged))
				continue;
			/* Else, advance i to the corresponding existing join column */
			while (i < colinfo->num_cols &&
				   colinfo->colnames[i] == NULL)
				i++;
			Assert(i < colinfo->num_cols);
			Assert(ic == colinfo->leftattnos[i]);
			/* Use the already-assigned name of this column */
			colinfo->new_colnames[j] = colinfo->colnames[i];
			i++;
		}
		else
		{
			/*
			 * Unique-ify the new child column name and assign, unless we're
			 * in an unnamed join, in which case just copy
			 */
			if (rte->alias != NULL)
			{
				colinfo->new_colnames[j] =
					make_colname_unique(child_colname, dpns, colinfo);
				if (!changed_any &&
					strcmp(colinfo->new_colnames[j], child_colname) != 0)
					changed_any = true;
			}
			else
				colinfo->new_colnames[j] = child_colname;
			add_to_names_hash(colinfo, colinfo->new_colnames[j]);
		}

		colinfo->is_new_col[j] = leftcolinfo->is_new_col[jc];
		j++;
	}

	/* Handle non-merged right-child columns in exactly the same way */
	ic = 0;
	for (jc = 0; jc < rightcolinfo->num_new_cols; jc++)
	{
		char	   *child_colname = rightcolinfo->new_colnames[jc];

		if (!rightcolinfo->is_new_col[jc])
		{
			/* Advance ic to next non-dropped old column of right child */
			while (ic < rightcolinfo->num_cols &&
				   rightcolinfo->colnames[ic] == NULL)
				ic++;
			Assert(ic < rightcolinfo->num_cols);
			ic++;
			/* If it is a merged column, we already processed it */
			if (bms_is_member(ic, rightmerged))
				continue;
			/* Else, advance i to the corresponding existing join column */
			while (i < colinfo->num_cols &&
				   colinfo->colnames[i] == NULL)
				i++;
			Assert(i < colinfo->num_cols);
			Assert(ic == colinfo->rightattnos[i]);
			/* Use the already-assigned name of this column */
			colinfo->new_colnames[j] = colinfo->colnames[i];
			i++;
		}
		else
		{
			/*
			 * Unique-ify the new child column name and assign, unless we're
			 * in an unnamed join, in which case just copy
			 */
			if (rte->alias != NULL)
			{
				colinfo->new_colnames[j] =
					make_colname_unique(child_colname, dpns, colinfo);
				if (!changed_any &&
					strcmp(colinfo->new_colnames[j], child_colname) != 0)
					changed_any = true;
			}
			else
				colinfo->new_colnames[j] = child_colname;
			add_to_names_hash(colinfo, colinfo->new_colnames[j]);
		}

		colinfo->is_new_col[j] = rightcolinfo->is_new_col[jc];
		j++;
	}

	/* Assert we processed the right number of columns */
#ifdef USE_ASSERT_CHECKING
	while (i < colinfo->num_cols && colinfo->colnames[i] == NULL)
		i++;
	Assert(i == colinfo->num_cols);
	Assert(j == nnewcolumns);
#endif

	/* We're now done needing the colinfo's names_hash */
	destroy_colinfo_names_hash(colinfo);

	/*
	 * For a named join, print column aliases if we changed any from the child
	 * names.  Unnamed joins cannot print aliases.
	 */
	if (rte->alias != NULL)
		colinfo->printaliases = changed_any;
	else
		colinfo->printaliases = false;
}

/*
 * colname_is_unique: is colname distinct from already-chosen column names?
 *
 * dpns is query-wide info, colinfo is for the column's RTE
 */
static bool
colname_is_unique(const char *colname, deparse_namespace *dpns,
				  deparse_columns *colinfo)
{
	int			i;
	ListCell   *lc;

	/*
	 * If we have a hash table, consult that instead of linearly scanning the
	 * colinfo's strings.
	 */
	if (colinfo->names_hash)
	{
		if (hash_search(colinfo->names_hash,
						colname,
						HASH_FIND,
						NULL) != NULL)
			return false;
	}
	else
	{
		/* Check against already-assigned column aliases within RTE */
		for (i = 0; i < colinfo->num_cols; i++)
		{
			char	   *oldname = colinfo->colnames[i];

			if (oldname && strcmp(oldname, colname) == 0)
				return false;
		}

		/*
		 * If we're building a new_colnames array, check that too (this will
		 * be partially but not completely redundant with the previous checks)
		 */
		for (i = 0; i < colinfo->num_new_cols; i++)
		{
			char	   *oldname = colinfo->new_colnames[i];

			if (oldname && strcmp(oldname, colname) == 0)
				return false;
		}

		/*
		 * Also check against names already assigned for parent-join USING
		 * cols
		 */
		foreach(lc, colinfo->parentUsing)
		{
			char	   *oldname = (char *) lfirst(lc);

			if (strcmp(oldname, colname) == 0)
				return false;
		}
	}

	/*
	 * Also check against USING-column names that must be globally unique.
	 * These are not hashed, but there should be few of them.
	 */
	foreach(lc, dpns->using_names)
	{
		char	   *oldname = (char *) lfirst(lc);

		if (strcmp(oldname, colname) == 0)
			return false;
	}

	return true;
}

/*
 * make_colname_unique: modify colname if necessary to make it unique
 *
 * dpns is query-wide info, colinfo is for the column's RTE
 */
static char *
make_colname_unique(char *colname, deparse_namespace *dpns,
					deparse_columns *colinfo)
{
	/*
	 * If the selected name isn't unique, append digits to make it so.  For a
	 * very long input name, we might have to truncate to stay within
	 * NAMEDATALEN.
	 */
	if (!colname_is_unique(colname, dpns, colinfo))
	{
		int			colnamelen = strlen(colname);
		char	   *modname = (char *) palloc(colnamelen + 16);
		int			i = 0;

		do
		{
			i++;
			for (;;)
			{
				memcpy(modname, colname, colnamelen);
				sprintf(modname + colnamelen, "_%d", i);
				if (strlen(modname) < NAMEDATALEN)
					break;
				/* drop chars from colname to keep all the digits */
				colnamelen = pg_mbcliplen(colname, colnamelen,
										  colnamelen - 1);
			}
		} while (!colname_is_unique(modname, dpns, colinfo));
		colname = modname;
	}
	return colname;
}

/*
 * expand_colnames_array_to: make colinfo->colnames at least n items long
 *
 * Any added array entries are initialized to zero.
 */
static void
expand_colnames_array_to(deparse_columns *colinfo, int n)
{
	if (n > colinfo->num_cols)
	{
		if (colinfo->colnames == NULL)
			colinfo->colnames = palloc0_array(char *, n);
		else
			colinfo->colnames = repalloc0_array(colinfo->colnames, char *, colinfo->num_cols, n);
		colinfo->num_cols = n;
	}
}

/*
 * build_colinfo_names_hash: optionally construct a hash table for colinfo
 */
static void
build_colinfo_names_hash(deparse_columns *colinfo)
{
	HASHCTL		hash_ctl;
	int			i;
	ListCell   *lc;

	/*
	 * Use a hash table only for RTEs with at least 32 columns.  (The cutoff
	 * is somewhat arbitrary, but let's choose it so that this code does get
	 * exercised in the regression tests.)
	 */
	if (colinfo->num_cols < 32)
		return;

	/*
	 * Set up the hash table.  The entries are just strings with no other
	 * payload.
	 */
	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = NAMEDATALEN;
	hash_ctl.hcxt = CurrentMemoryContext;
	colinfo->names_hash = hash_create("deparse_columns names",
									  colinfo->num_cols + colinfo->num_new_cols,
									  &hash_ctl,
									  HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

	/*
	 * Preload the hash table with any names already present (these would have
	 * come from set_using_names).
	 */
	for (i = 0; i < colinfo->num_cols; i++)
	{
		char	   *oldname = colinfo->colnames[i];

		if (oldname)
			add_to_names_hash(colinfo, oldname);
	}

	for (i = 0; i < colinfo->num_new_cols; i++)
	{
		char	   *oldname = colinfo->new_colnames[i];

		if (oldname)
			add_to_names_hash(colinfo, oldname);
	}

	foreach(lc, colinfo->parentUsing)
	{
		char	   *oldname = (char *) lfirst(lc);

		add_to_names_hash(colinfo, oldname);
	}
}

/*
 * add_to_names_hash: add a string to the names_hash, if we're using one
 */
static void
add_to_names_hash(deparse_columns *colinfo, const char *name)
{
	if (colinfo->names_hash)
		(void) hash_search(colinfo->names_hash,
						   name,
						   HASH_ENTER,
						   NULL);
}

/*
 * destroy_colinfo_names_hash: destroy hash table when done with it
 */
static void
destroy_colinfo_names_hash(deparse_columns *colinfo)
{
	if (colinfo->names_hash)
	{
		hash_destroy(colinfo->names_hash);
		colinfo->names_hash = NULL;
	}
}

/*
 * identify_join_columns: figure out where columns of a join come from
 *
 * Fills the join-specific fields of the colinfo struct, except for
 * usingNames which is filled later.
 */
static void
identify_join_columns(JoinExpr *j, RangeTblEntry *jrte,
					  deparse_columns *colinfo)
{
	int			numjoincols;
	int			jcolno;
	int			rcolno;
	ListCell   *lc;

	/* Extract left/right child RT indexes */
	if (IsA(j->larg, RangeTblRef))
		colinfo->leftrti = ((RangeTblRef *) j->larg)->rtindex;
	else if (IsA(j->larg, JoinExpr))
		colinfo->leftrti = ((JoinExpr *) j->larg)->rtindex;
	else
		elog(ERROR, "unrecognized node type in jointree: %d",
			 (int) nodeTag(j->larg));
	if (IsA(j->rarg, RangeTblRef))
		colinfo->rightrti = ((RangeTblRef *) j->rarg)->rtindex;
	else if (IsA(j->rarg, JoinExpr))
		colinfo->rightrti = ((JoinExpr *) j->rarg)->rtindex;
	else
		elog(ERROR, "unrecognized node type in jointree: %d",
			 (int) nodeTag(j->rarg));

	/* Assert children will be processed earlier than join in second pass */
	Assert(colinfo->leftrti < j->rtindex);
	Assert(colinfo->rightrti < j->rtindex);

	/* Initialize result arrays with zeroes */
	numjoincols = list_length(jrte->joinaliasvars);
	Assert(numjoincols == list_length(jrte->eref->colnames));
	colinfo->leftattnos = (int *) palloc0(numjoincols * sizeof(int));
	colinfo->rightattnos = (int *) palloc0(numjoincols * sizeof(int));

	/*
	 * Deconstruct RTE's joinleftcols/joinrightcols into desired format.
	 * Recall that the column(s) merged due to USING are the first column(s)
	 * of the join output.  We need not do anything special while scanning
	 * joinleftcols, but while scanning joinrightcols we must distinguish
	 * merged from unmerged columns.
	 */
	jcolno = 0;
	foreach(lc, jrte->joinleftcols)
	{
		int			leftattno = lfirst_int(lc);

		colinfo->leftattnos[jcolno++] = leftattno;
	}
	rcolno = 0;
	foreach(lc, jrte->joinrightcols)
	{
		int			rightattno = lfirst_int(lc);

		if (rcolno < jrte->joinmergedcols)	/* merged column? */
			colinfo->rightattnos[rcolno] = rightattno;
		else
			colinfo->rightattnos[jcolno++] = rightattno;
		rcolno++;
	}
	Assert(jcolno == numjoincols);
}

/*
 * get_rtable_name: convenience function to get a previously assigned RTE alias
 *
 * The RTE must belong to the topmost namespace level in "context".
 */
static char *
get_rtable_name(int rtindex, deparse_context *context)
{
	deparse_namespace *dpns = (deparse_namespace *) linitial(context->namespaces);

	Assert(rtindex > 0 && rtindex <= list_length(dpns->rtable_names));
	return (char *) list_nth(dpns->rtable_names, rtindex - 1);
}

/*
 * set_deparse_plan: set up deparse_namespace to parse subexpressions
 * of a given Plan node
 *
 * This sets the plan, outer_plan, inner_plan, outer_tlist, inner_tlist,
 * and index_tlist fields.  Caller must already have adjusted the ancestors
 * list if necessary.  Note that the rtable, subplans, and ctes fields do
 * not need to change when shifting attention to different plan nodes in a
 * single plan tree.
 */
static void
set_deparse_plan(deparse_namespace *dpns, Plan *plan)
{
	dpns->plan = plan;

	/*
	 * We special-case Append and MergeAppend to pretend that the first child
	 * plan is the OUTER referent; we have to interpret OUTER Vars in their
	 * tlists according to one of the children, and the first one is the most
	 * natural choice.
	 */
	if (IsA(plan, Append))
		dpns->outer_plan = linitial(((Append *) plan)->appendplans);
	else if (IsA(plan, MergeAppend))
		dpns->outer_plan = linitial(((MergeAppend *) plan)->mergeplans);
	else
		dpns->outer_plan = outerPlan(plan);

	if (dpns->outer_plan)
		dpns->outer_tlist = dpns->outer_plan->targetlist;
	else
		dpns->outer_tlist = NIL;

	/*
	 * For a SubqueryScan, pretend the subplan is INNER referent.  (We don't
	 * use OUTER because that could someday conflict with the normal meaning.)
	 * Likewise, for a CteScan, pretend the subquery's plan is INNER referent.
	 * For a WorkTableScan, locate the parent RecursiveUnion plan node and use
	 * that as INNER referent.
	 *
	 * For MERGE, pretend the ModifyTable's source plan (its outer plan) is
	 * INNER referent.  This is the join from the target relation to the data
	 * source, and all INNER_VAR Vars in other parts of the query refer to its
	 * targetlist.
	 *
	 * For ON CONFLICT .. UPDATE we just need the inner tlist to point to the
	 * excluded expression's tlist. (Similar to the SubqueryScan we don't want
	 * to reuse OUTER, it's used for RETURNING in some modify table cases,
	 * although not INSERT .. CONFLICT).
	 */
	if (IsA(plan, SubqueryScan))
		dpns->inner_plan = ((SubqueryScan *) plan)->subplan;
	else if (IsA(plan, CteScan))
		dpns->inner_plan = list_nth(dpns->subplans,
									((CteScan *) plan)->ctePlanId - 1);
	else if (IsA(plan, WorkTableScan))
		dpns->inner_plan = find_recursive_union(dpns,
												(WorkTableScan *) plan);
	else if (IsA(plan, ModifyTable))
	{
		if (((ModifyTable *) plan)->operation == CMD_MERGE)
			dpns->inner_plan = outerPlan(plan);
		else
			dpns->inner_plan = plan;
	}
	else
		dpns->inner_plan = innerPlan(plan);

	if (IsA(plan, ModifyTable) && ((ModifyTable *) plan)->operation == CMD_INSERT)
		dpns->inner_tlist = ((ModifyTable *) plan)->exclRelTlist;
	else if (dpns->inner_plan)
		dpns->inner_tlist = dpns->inner_plan->targetlist;
	else
		dpns->inner_tlist = NIL;

	/* Set up referent for INDEX_VAR Vars, if needed */
	if (IsA(plan, IndexOnlyScan))
		dpns->index_tlist = ((IndexOnlyScan *) plan)->indextlist;
	else if (IsA(plan, ForeignScan))
		dpns->index_tlist = ((ForeignScan *) plan)->fdw_scan_tlist;
	else if (IsA(plan, CustomScan))
		dpns->index_tlist = ((CustomScan *) plan)->custom_scan_tlist;
	else
		dpns->index_tlist = NIL;
}

/*
 * Locate the ancestor plan node that is the RecursiveUnion generating
 * the WorkTableScan's work table.  We can match on wtParam, since that
 * should be unique within the plan tree.
 */
static Plan *
find_recursive_union(deparse_namespace *dpns, WorkTableScan *wtscan)
{
	ListCell   *lc;

	foreach(lc, dpns->ancestors)
	{
		Plan	   *ancestor = (Plan *) lfirst(lc);

		if (IsA(ancestor, RecursiveUnion) &&
			((RecursiveUnion *) ancestor)->wtParam == wtscan->wtParam)
			return ancestor;
	}
	elog(ERROR, "could not find RecursiveUnion for WorkTableScan with wtParam %d",
		 wtscan->wtParam);
	return NULL;
}

/*
 * push_child_plan: temporarily transfer deparsing attention to a child plan
 *
 * When expanding an OUTER_VAR or INNER_VAR reference, we must adjust the
 * deparse context in case the referenced expression itself uses
 * OUTER_VAR/INNER_VAR.  We modify the top stack entry in-place to avoid
 * affecting levelsup issues (although in a Plan tree there really shouldn't
 * be any).
 *
 * Caller must provide a local deparse_namespace variable to save the
 * previous state for pop_child_plan.
 */
static void
push_child_plan(deparse_namespace *dpns, Plan *plan,
				deparse_namespace *save_dpns)
{
	/* Save state for restoration later */
	*save_dpns = *dpns;

	/* Link current plan node into ancestors list */
	dpns->ancestors = lcons(dpns->plan, dpns->ancestors);

	/* Set attention on selected child */
	set_deparse_plan(dpns, plan);
}

/*
 * pop_child_plan: undo the effects of push_child_plan
 */
static void
pop_child_plan(deparse_namespace *dpns, deparse_namespace *save_dpns)
{
	List	   *ancestors;

	/* Get rid of ancestors list cell added by push_child_plan */
	ancestors = list_delete_first(dpns->ancestors);

	/* Restore fields changed by push_child_plan */
	*dpns = *save_dpns;

	/* Make sure dpns->ancestors is right (may be unnecessary) */
	dpns->ancestors = ancestors;
}

/*
 * push_ancestor_plan: temporarily transfer deparsing attention to an
 * ancestor plan
 *
 * When expanding a Param reference, we must adjust the deparse context
 * to match the plan node that contains the expression being printed;
 * otherwise we'd fail if that expression itself contains a Param or
 * OUTER_VAR/INNER_VAR/INDEX_VAR variable.
 *
 * The target ancestor is conveniently identified by the ListCell holding it
 * in dpns->ancestors.
 *
 * Caller must provide a local deparse_namespace variable to save the
 * previous state for pop_ancestor_plan.
 */
static void
push_ancestor_plan(deparse_namespace *dpns, ListCell *ancestor_cell,
				   deparse_namespace *save_dpns)
{
	Plan	   *plan = (Plan *) lfirst(ancestor_cell);

	/* Save state for restoration later */
	*save_dpns = *dpns;

	/* Build a new ancestor list with just this node's ancestors */
	dpns->ancestors =
		list_copy_tail(dpns->ancestors,
					   list_cell_number(dpns->ancestors, ancestor_cell) + 1);

	/* Set attention on selected ancestor */
	set_deparse_plan(dpns, plan);
}

/*
 * pop_ancestor_plan: undo the effects of push_ancestor_plan
 */
static void
pop_ancestor_plan(deparse_namespace *dpns, deparse_namespace *save_dpns)
{
	/* Free the ancestor list made in push_ancestor_plan */
	list_free(dpns->ancestors);

	/* Restore fields changed by push_ancestor_plan */
	*dpns = *save_dpns;
}


/* ----------
 * make_ruledef			- reconstruct the CREATE RULE command
 *				  for a given pg_rewrite tuple
 * ----------
 */
static void
make_ruledef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc,
			 int prettyFlags)
{
	char	   *rulename;
	char		ev_type;
	Oid			ev_class;
	bool		is_instead;
	char	   *ev_qual;
	char	   *ev_action;
	List	   *actions;
	Relation	ev_relation;
	TupleDesc	viewResultDesc = NULL;
	int			fno;
	Datum		dat;
	bool		isnull;

	/*
	 * Get the attribute values from the rules tuple
	 */
	fno = SPI_fnumber(rulettc, "rulename");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	rulename = NameStr(*(DatumGetName(dat)));

	fno = SPI_fnumber(rulettc, "ev_type");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	ev_type = DatumGetChar(dat);

	fno = SPI_fnumber(rulettc, "ev_class");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	ev_class = DatumGetObjectId(dat);

	fno = SPI_fnumber(rulettc, "is_instead");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	is_instead = DatumGetBool(dat);

	fno = SPI_fnumber(rulettc, "ev_qual");
	ev_qual = SPI_getvalue(ruletup, rulettc, fno);
	Assert(ev_qual != NULL);

	fno = SPI_fnumber(rulettc, "ev_action");
	ev_action = SPI_getvalue(ruletup, rulettc, fno);
	Assert(ev_action != NULL);
	actions = (List *) stringToNode(ev_action);
	if (actions == NIL)
		elog(ERROR, "invalid empty ev_action list");

	ev_relation = table_open(ev_class, AccessShareLock);

	/*
	 * Build the rules definition text
	 */
	appendStringInfo(buf, "CREATE RULE %s AS",
					 quote_identifier(rulename));

	if (prettyFlags & PRETTYFLAG_INDENT)
		appendStringInfoString(buf, "\n    ON ");
	else
		appendStringInfoString(buf, " ON ");

	/* The event the rule is fired for */
	switch (ev_type)
	{
		case '1':
			appendStringInfoString(buf, "SELECT");
			viewResultDesc = RelationGetDescr(ev_relation);
			break;

		case '2':
			appendStringInfoString(buf, "UPDATE");
			break;

		case '3':
			appendStringInfoString(buf, "INSERT");
			break;

		case '4':
			appendStringInfoString(buf, "DELETE");
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rule \"%s\" has unsupported event type %d",
							rulename, ev_type)));
			break;
	}

	/* The relation the rule is fired on */
	appendStringInfo(buf, " TO %s",
					 (prettyFlags & PRETTYFLAG_SCHEMA) ?
					 generate_relation_name(ev_class, NIL) :
					 generate_qualified_relation_name(ev_class));

	/* If the rule has an event qualification, add it */
	if (strcmp(ev_qual, "<>") != 0)
	{
		Node	   *qual;
		Query	   *query;
		deparse_context context;
		deparse_namespace dpns;

		if (prettyFlags & PRETTYFLAG_INDENT)
			appendStringInfoString(buf, "\n  ");
		appendStringInfoString(buf, " WHERE ");

		qual = stringToNode(ev_qual);

		/*
		 * We need to make a context for recognizing any Vars in the qual
		 * (which can only be references to OLD and NEW).  Use the rtable of
		 * the first query in the action list for this purpose.
		 */
		query = (Query *) linitial(actions);

		/*
		 * If the action is INSERT...SELECT, OLD/NEW have been pushed down
		 * into the SELECT, and that's what we need to look at. (Ugly kluge
		 * ... try to fix this when we redesign querytrees.)
		 */
		query = getInsertSelectQuery(query, NULL);

		/* Must acquire locks right away; see notes in get_query_def() */
		AcquireRewriteLocks(query, false, false);

		context.buf = buf;
		context.namespaces = list_make1(&dpns);
		context.resultDesc = NULL;
		context.targetList = NIL;
		context.windowClause = NIL;
		context.varprefix = (list_length(query->rtable) != 1);
		context.prettyFlags = prettyFlags;
		context.wrapColumn = WRAP_COLUMN_DEFAULT;
		context.indentLevel = PRETTYINDENT_STD;
		context.colNamesVisible = true;
		context.inGroupBy = false;
		context.varInOrderBy = false;
		context.appendparents = NULL;

		set_deparse_for_query(&dpns, query, NIL);

		get_rule_expr(qual, &context, false);
	}

	appendStringInfoString(buf, " DO ");

	/* The INSTEAD keyword (if so) */
	if (is_instead)
		appendStringInfoString(buf, "INSTEAD ");

	/* Finally the rules actions */
	if (list_length(actions) > 1)
	{
		ListCell   *action;
		Query	   *query;

		appendStringInfoChar(buf, '(');
		foreach(action, actions)
		{
			query = (Query *) lfirst(action);
			get_query_def(query, buf, NIL, viewResultDesc, true,
						  prettyFlags, WRAP_COLUMN_DEFAULT, 0);
			if (prettyFlags)
				appendStringInfoString(buf, ";\n");
			else
				appendStringInfoString(buf, "; ");
		}
		appendStringInfoString(buf, ");");
	}
	else
	{
		Query	   *query;

		query = (Query *) linitial(actions);
		get_query_def(query, buf, NIL, viewResultDesc, true,
					  prettyFlags, WRAP_COLUMN_DEFAULT, 0);
		appendStringInfoChar(buf, ';');
	}

	table_close(ev_relation, AccessShareLock);
}


/* ----------
 * make_viewdef			- reconstruct the SELECT part of a
 *				  view rewrite rule
 * ----------
 */
static void
make_viewdef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc,
			 int prettyFlags, int wrapColumn)
{
	Query	   *query;
	char		ev_type;
	Oid			ev_class;
	bool		is_instead;
	char	   *ev_qual;
	char	   *ev_action;
	List	   *actions;
	Relation	ev_relation;
	int			fno;
	Datum		dat;
	bool		isnull;

	/*
	 * Get the attribute values from the rules tuple
	 */
	fno = SPI_fnumber(rulettc, "ev_type");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	ev_type = DatumGetChar(dat);

	fno = SPI_fnumber(rulettc, "ev_class");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	ev_class = DatumGetObjectId(dat);

	fno = SPI_fnumber(rulettc, "is_instead");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	is_instead = DatumGetBool(dat);

	fno = SPI_fnumber(rulettc, "ev_qual");
	ev_qual = SPI_getvalue(ruletup, rulettc, fno);
	Assert(ev_qual != NULL);

	fno = SPI_fnumber(rulettc, "ev_action");
	ev_action = SPI_getvalue(ruletup, rulettc, fno);
	Assert(ev_action != NULL);
	actions = (List *) stringToNode(ev_action);

	if (list_length(actions) != 1)
	{
		/* keep output buffer empty and leave */
		return;
	}

	query = (Query *) linitial(actions);

	if (ev_type != '1' || !is_instead ||
		strcmp(ev_qual, "<>") != 0 || query->commandType != CMD_SELECT)
	{
		/* keep output buffer empty and leave */
		return;
	}

	ev_relation = table_open(ev_class, AccessShareLock);

	get_query_def(query, buf, NIL, RelationGetDescr(ev_relation), true,
				  prettyFlags, wrapColumn, 0);
	appendStringInfoChar(buf, ';');

	table_close(ev_relation, AccessShareLock);
}


/* ----------
 * get_query_def			- Parse back one query parsetree
 *
 * query: parsetree to be displayed
 * buf: output text is appended to buf
 * parentnamespace: list (initially empty) of outer-level deparse_namespace's
 * resultDesc: if not NULL, the output tuple descriptor for the view
 *		represented by a SELECT query.  We use the column names from it
 *		to label SELECT output columns, in preference to names in the query
 * colNamesVisible: true if the surrounding context cares about the output
 *		column names at all (as, for example, an EXISTS() context does not);
 *		when false, we can suppress dummy column labels such as "?column?"
 * prettyFlags: bitmask of PRETTYFLAG_XXX options
 * wrapColumn: maximum line length, or -1 to disable wrapping
 * startIndent: initial indentation amount
 * ----------
 */
static void
get_query_def(Query *query, StringInfo buf, List *parentnamespace,
			  TupleDesc resultDesc, bool colNamesVisible,
			  int prettyFlags, int wrapColumn, int startIndent)
{
	deparse_context context;
	deparse_namespace dpns;
	int			rtable_size;

	/* Guard against excessively long or deeply-nested queries */
	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	rtable_size = query->hasGroupRTE ?
		list_length(query->rtable) - 1 :
		list_length(query->rtable);

	/*
	 * Replace any Vars in the query's targetlist and havingQual that
	 * reference GROUP outputs with the underlying grouping expressions.
	 */
	if (query->hasGroupRTE)
	{
		query->targetList = (List *)
			flatten_group_exprs(NULL, query, (Node *) query->targetList);
		query->havingQual =
			flatten_group_exprs(NULL, query, query->havingQual);
	}

	/*
	 * Before we begin to examine the query, acquire locks on referenced
	 * relations, and fix up deleted columns in JOIN RTEs.  This ensures
	 * consistent results.  Note we assume it's OK to scribble on the passed
	 * querytree!
	 *
	 * We are only deparsing the query (we are not about to execute it), so we
	 * only need AccessShareLock on the relations it mentions.
	 */
	AcquireRewriteLocks(query, false, false);

	context.buf = buf;
	context.namespaces = lcons(&dpns, list_copy(parentnamespace));
	context.resultDesc = NULL;
	context.targetList = NIL;
	context.windowClause = NIL;
	context.varprefix = (parentnamespace != NIL ||
						 rtable_size != 1);
	context.prettyFlags = prettyFlags;
	context.wrapColumn = wrapColumn;
	context.indentLevel = startIndent;
	context.colNamesVisible = colNamesVisible;
	context.inGroupBy = false;
	context.varInOrderBy = false;
	context.appendparents = NULL;

	set_deparse_for_query(&dpns, query, parentnamespace);

	switch (query->commandType)
	{
		case CMD_SELECT:
			/* We set context.resultDesc only if it's a SELECT */
			context.resultDesc = resultDesc;
			get_select_query_def(query, &context);
			break;

		case CMD_UPDATE:
			get_update_query_def(query, &context);
			break;

		case CMD_INSERT:
			get_insert_query_def(query, &context);
			break;

		case CMD_DELETE:
			get_delete_query_def(query, &context);
			break;

		case CMD_MERGE:
			get_merge_query_def(query, &context);
			break;

		case CMD_NOTHING:
			appendStringInfoString(buf, "NOTHING");
			break;

		case CMD_UTILITY:
			get_utility_query_def(query, &context);
			break;

		default:
			elog(ERROR, "unrecognized query command type: %d",
				 query->commandType);
			break;
	}
}

/* ----------
 * get_values_def			- Parse back a VALUES list
 * ----------
 */
static void
get_values_def(List *values_lists, deparse_context *context)
{
	StringInfo	buf = context->buf;
	bool		first_list = true;
	ListCell   *vtl;

	appendStringInfoString(buf, "VALUES ");

	foreach(vtl, values_lists)
	{
		List	   *sublist = (List *) lfirst(vtl);
		bool		first_col = true;
		ListCell   *lc;

		if (first_list)
			first_list = false;
		else
			appendStringInfoString(buf, ", ");

		appendStringInfoChar(buf, '(');
		foreach(lc, sublist)
		{
			Node	   *col = (Node *) lfirst(lc);

			if (first_col)
				first_col = false;
			else
				appendStringInfoChar(buf, ',');

			/*
			 * Print the value.  Whole-row Vars need special treatment.
			 */
			get_rule_expr_toplevel(col, context, false);
		}
		appendStringInfoChar(buf, ')');
	}
}

/* ----------
 * get_with_clause			- Parse back a WITH clause
 * ----------
 */
static void
get_with_clause(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	const char *sep;
	ListCell   *l;

	if (query->cteList == NIL)
		return;

	if (PRETTY_INDENT(context))
	{
		context->indentLevel += PRETTYINDENT_STD;
		appendStringInfoChar(buf, ' ');
	}

	if (query->hasRecursive)
		sep = "WITH RECURSIVE ";
	else
		sep = "WITH ";
	foreach(l, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(l);

		appendStringInfoString(buf, sep);
		appendStringInfoString(buf, quote_identifier(cte->ctename));
		if (cte->aliascolnames)
		{
			bool		first = true;
			ListCell   *col;

			appendStringInfoChar(buf, '(');
			foreach(col, cte->aliascolnames)
			{
				if (first)
					first = false;
				else
					appendStringInfoString(buf, ", ");
				appendStringInfoString(buf,
									   quote_identifier(strVal(lfirst(col))));
			}
			appendStringInfoChar(buf, ')');
		}
		appendStringInfoString(buf, " AS ");
		switch (cte->ctematerialized)
		{
			case CTEMaterializeDefault:
				break;
			case CTEMaterializeAlways:
				appendStringInfoString(buf, "MATERIALIZED ");
				break;
			case CTEMaterializeNever:
				appendStringInfoString(buf, "NOT MATERIALIZED ");
				break;
		}
		appendStringInfoChar(buf, '(');
		if (PRETTY_INDENT(context))
			appendContextKeyword(context, "", 0, 0, 0);
		get_query_def((Query *) cte->ctequery, buf, context->namespaces, NULL,
					  true,
					  context->prettyFlags, context->wrapColumn,
					  context->indentLevel);
		if (PRETTY_INDENT(context))
			appendContextKeyword(context, "", 0, 0, 0);
		appendStringInfoChar(buf, ')');

		if (cte->search_clause)
		{
			bool		first = true;
			ListCell   *lc;

			appendStringInfo(buf, " SEARCH %s FIRST BY ",
							 cte->search_clause->search_breadth_first ? "BREADTH" : "DEPTH");

			foreach(lc, cte->search_clause->search_col_list)
			{
				if (first)
					first = false;
				else
					appendStringInfoString(buf, ", ");
				appendStringInfoString(buf,
									   quote_identifier(strVal(lfirst(lc))));
			}

			appendStringInfo(buf, " SET %s", quote_identifier(cte->search_clause->search_seq_column));
		}

		if (cte->cycle_clause)
		{
			bool		first = true;
			ListCell   *lc;

			appendStringInfoString(buf, " CYCLE ");

			foreach(lc, cte->cycle_clause->cycle_col_list)
			{
				if (first)
					first = false;
				else
					appendStringInfoString(buf, ", ");
				appendStringInfoString(buf,
									   quote_identifier(strVal(lfirst(lc))));
			}

			appendStringInfo(buf, " SET %s", quote_identifier(cte->cycle_clause->cycle_mark_column));

			{
				Const	   *cmv = castNode(Const, cte->cycle_clause->cycle_mark_value);
				Const	   *cmd = castNode(Const, cte->cycle_clause->cycle_mark_default);

				if (!(cmv->consttype == BOOLOID && !cmv->constisnull && DatumGetBool(cmv->constvalue) == true &&
					  cmd->consttype == BOOLOID && !cmd->constisnull && DatumGetBool(cmd->constvalue) == false))
				{
					appendStringInfoString(buf, " TO ");
					get_rule_expr(cte->cycle_clause->cycle_mark_value, context, false);
					appendStringInfoString(buf, " DEFAULT ");
					get_rule_expr(cte->cycle_clause->cycle_mark_default, context, false);
				}
			}

			appendStringInfo(buf, " USING %s", quote_identifier(cte->cycle_clause->cycle_path_column));
		}

		sep = ", ";
	}

	if (PRETTY_INDENT(context))
	{
		context->indentLevel -= PRETTYINDENT_STD;
		appendContextKeyword(context, "", 0, 0, 0);
	}
	else
		appendStringInfoChar(buf, ' ');
}

/* ----------
 * get_select_query_def			- Parse back a SELECT parsetree
 * ----------
 */
static void
get_select_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	bool		force_colno;
	ListCell   *l;

	/* Insert the WITH clause if given */
	get_with_clause(query, context);

	/* Subroutines may need to consult the SELECT targetlist and windowClause */
	context->targetList = query->targetList;
	context->windowClause = query->windowClause;

	/*
	 * If the Query node has a setOperations tree, then it's the top level of
	 * a UNION/INTERSECT/EXCEPT query; only the WITH, ORDER BY and LIMIT
	 * fields are interesting in the top query itself.
	 */
	if (query->setOperations)
	{
		get_setop_query(query->setOperations, query, context);
		/* ORDER BY clauses must be simple in this case */
		force_colno = true;
	}
	else
	{
		get_basic_select_query(query, context);
		force_colno = false;
	}

	/* Add the ORDER BY clause if given */
	if (query->sortClause != NIL)
	{
		appendContextKeyword(context, " ORDER BY ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_rule_orderby(query->sortClause, query->targetList,
						 force_colno, context);
	}

	/*
	 * Add the LIMIT/OFFSET clauses if given. If non-default options, use the
	 * standard spelling of LIMIT.
	 */
	if (query->limitOffset != NULL)
	{
		appendContextKeyword(context, " OFFSET ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
		get_rule_expr(query->limitOffset, context, false);
	}
	if (query->limitCount != NULL)
	{
		if (query->limitOption == LIMIT_OPTION_WITH_TIES)
		{
			appendContextKeyword(context, " FETCH FIRST ",
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
			get_rule_expr(query->limitCount, context, false);
			appendStringInfoString(buf, " ROWS WITH TIES");
		}
		else
		{
			appendContextKeyword(context, " LIMIT ",
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
			if (IsA(query->limitCount, Const) &&
				((Const *) query->limitCount)->constisnull)
				appendStringInfoString(buf, "ALL");
			else
				get_rule_expr(query->limitCount, context, false);
		}
	}

	/* Add FOR [KEY] UPDATE/SHARE clauses if present */
	if (query->hasForUpdate)
	{
		foreach(l, query->rowMarks)
		{
			RowMarkClause *rc = (RowMarkClause *) lfirst(l);

			/* don't print implicit clauses */
			if (rc->pushedDown)
				continue;

			switch (rc->strength)
			{
				case LCS_NONE:
					/* we intentionally throw an error for LCS_NONE */
					elog(ERROR, "unrecognized LockClauseStrength %d",
						 (int) rc->strength);
					break;
				case LCS_FORKEYSHARE:
					appendContextKeyword(context, " FOR KEY SHARE",
										 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
					break;
				case LCS_FORSHARE:
					appendContextKeyword(context, " FOR SHARE",
										 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
					break;
				case LCS_FORNOKEYUPDATE:
					appendContextKeyword(context, " FOR NO KEY UPDATE",
										 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
					break;
				case LCS_FORUPDATE:
					appendContextKeyword(context, " FOR UPDATE",
										 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
					break;
			}

			appendStringInfo(buf, " OF %s",
							 quote_identifier(get_rtable_name(rc->rti,
															  context)));
			if (rc->waitPolicy == LockWaitError)
				appendStringInfoString(buf, " NOWAIT");
			else if (rc->waitPolicy == LockWaitSkip)
				appendStringInfoString(buf, " SKIP LOCKED");
		}
	}
}

/*
 * Detect whether query looks like SELECT ... FROM VALUES(),
 * with no need to rename the output columns of the VALUES RTE.
 * If so, return the VALUES RTE.  Otherwise return NULL.
 */
static RangeTblEntry *
get_simple_values_rte(Query *query, TupleDesc resultDesc)
{
	RangeTblEntry *result = NULL;
	ListCell   *lc;

	/*
	 * We want to detect a match even if the Query also contains OLD or NEW
	 * rule RTEs.  So the idea is to scan the rtable and see if there is only
	 * one inFromCl RTE that is a VALUES RTE.
	 */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_VALUES && rte->inFromCl)
		{
			if (result)
				return NULL;	/* multiple VALUES (probably not possible) */
			result = rte;
		}
		else if (rte->rtekind == RTE_RELATION && !rte->inFromCl)
			continue;			/* ignore rule entries */
		else
			return NULL;		/* something else -> not simple VALUES */
	}

	/*
	 * We don't need to check the targetlist in any great detail, because
	 * parser/analyze.c will never generate a "bare" VALUES RTE --- they only
	 * appear inside auto-generated sub-queries with very restricted
	 * structure.  However, DefineView might have modified the tlist by
	 * injecting new column aliases, or we might have some other column
	 * aliases forced by a resultDesc.  We can only simplify if the RTE's
	 * column names match the names that get_target_list() would select.
	 */
	if (result)
	{
		ListCell   *lcn;
		int			colno;

		if (list_length(query->targetList) != list_length(result->eref->colnames))
			return NULL;		/* this probably cannot happen */
		colno = 0;
		forboth(lc, query->targetList, lcn, result->eref->colnames)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			char	   *cname = strVal(lfirst(lcn));
			char	   *colname;

			if (tle->resjunk)
				return NULL;	/* this probably cannot happen */

			/* compute name that get_target_list would use for column */
			colno++;
			if (resultDesc && colno <= resultDesc->natts)
				colname = NameStr(TupleDescAttr(resultDesc, colno - 1)->attname);
			else
				colname = tle->resname;

			/* does it match the VALUES RTE? */
			if (colname == NULL || strcmp(colname, cname) != 0)
				return NULL;	/* column name has been changed */
		}
	}

	return result;
}

static void
get_basic_select_query(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *values_rte;
	char	   *sep;
	ListCell   *l;

	if (PRETTY_INDENT(context))
	{
		context->indentLevel += PRETTYINDENT_STD;
		appendStringInfoChar(buf, ' ');
	}

	/*
	 * If the query looks like SELECT * FROM (VALUES ...), then print just the
	 * VALUES part.  This reverses what transformValuesClause() did at parse
	 * time.
	 */
	values_rte = get_simple_values_rte(query, context->resultDesc);
	if (values_rte)
	{
		get_values_def(values_rte->values_lists, context);
		return;
	}

	/*
	 * Build up the query string - first we say SELECT
	 */
	if (query->isReturn)
		appendStringInfoString(buf, "RETURN");
	else
		appendStringInfoString(buf, "SELECT");

	/* Add the DISTINCT clause if given */
	if (query->distinctClause != NIL)
	{
		if (query->hasDistinctOn)
		{
			appendStringInfoString(buf, " DISTINCT ON (");
			sep = "";
			foreach(l, query->distinctClause)
			{
				SortGroupClause *srt = (SortGroupClause *) lfirst(l);

				appendStringInfoString(buf, sep);
				get_rule_sortgroupclause(srt->tleSortGroupRef, query->targetList,
										 false, context);
				sep = ", ";
			}
			appendStringInfoChar(buf, ')');
		}
		else
			appendStringInfoString(buf, " DISTINCT");
	}

	/* Then we tell what to select (the targetlist) */
	get_target_list(query->targetList, context);

	/* Add the FROM clause if needed */
	get_from_clause(query, " FROM ", context);

	/* Add the WHERE clause if given */
	if (query->jointree->quals != NULL)
	{
		appendContextKeyword(context, " WHERE ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_rule_expr(query->jointree->quals, context, false);
	}

	/* Add the GROUP BY clause if given */
	if (query->groupClause != NULL || query->groupingSets != NULL)
	{
		bool		save_ingroupby;

		appendContextKeyword(context, " GROUP BY ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		if (query->groupDistinct)
			appendStringInfoString(buf, "DISTINCT ");

		save_ingroupby = context->inGroupBy;
		context->inGroupBy = true;

		if (query->groupingSets == NIL)
		{
			sep = "";
			foreach(l, query->groupClause)
			{
				SortGroupClause *grp = (SortGroupClause *) lfirst(l);

				appendStringInfoString(buf, sep);
				get_rule_sortgroupclause(grp->tleSortGroupRef, query->targetList,
										 false, context);
				sep = ", ";
			}
		}
		else
		{
			sep = "";
			foreach(l, query->groupingSets)
			{
				GroupingSet *grp = lfirst(l);

				appendStringInfoString(buf, sep);
				get_rule_groupingset(grp, query->targetList, true, context);
				sep = ", ";
			}
		}

		context->inGroupBy = save_ingroupby;
	}

	/* Add the HAVING clause if given */
	if (query->havingQual != NULL)
	{
		appendContextKeyword(context, " HAVING ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
		get_rule_expr(query->havingQual, context, false);
	}

	/* Add the WINDOW clause if needed */
	if (query->windowClause != NIL)
		get_rule_windowclause(query, context);
}

/* ----------
 * get_target_list			- Parse back a SELECT target list
 *
 * This is also used for RETURNING lists in INSERT/UPDATE/DELETE/MERGE.
 * ----------
 */
static void
get_target_list(List *targetList, deparse_context *context)
{
	StringInfo	buf = context->buf;
	StringInfoData targetbuf;
	bool		last_was_multiline = false;
	char	   *sep;
	int			colno;
	ListCell   *l;

	/* we use targetbuf to hold each TLE's text temporarily */
	initStringInfo(&targetbuf);

	sep = " ";
	colno = 0;
	foreach(l, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		char	   *colname;
		char	   *attname;

		if (tle->resjunk)
			continue;			/* ignore junk entries */

		appendStringInfoString(buf, sep);
		sep = ", ";
		colno++;

		/*
		 * Put the new field text into targetbuf so we can decide after we've
		 * got it whether or not it needs to go on a new line.
		 */
		resetStringInfo(&targetbuf);
		context->buf = &targetbuf;

		/*
		 * We special-case Var nodes rather than using get_rule_expr. This is
		 * needed because get_rule_expr will display a whole-row Var as
		 * "foo.*", which is the preferred notation in most contexts, but at
		 * the top level of a SELECT list it's not right (the parser will
		 * expand that notation into multiple columns, yielding behavior
		 * different from a whole-row Var).  We need to call get_variable
		 * directly so that we can tell it to do the right thing, and so that
		 * we can get the attribute name which is the default AS label.
		 */
		if (tle->expr && (IsA(tle->expr, Var)))
		{
			attname = get_variable((Var *) tle->expr, 0, true, context);
		}
		else
		{
			get_rule_expr((Node *) tle->expr, context, true);

			/*
			 * When colNamesVisible is true, we should always show the
			 * assigned column name explicitly.  Otherwise, show it only if
			 * it's not FigureColname's fallback.
			 */
			attname = context->colNamesVisible ? NULL : "?column?";
		}

		/*
		 * Figure out what the result column should be called.  In the context
		 * of a view, use the view's tuple descriptor (so as to pick up the
		 * effects of any column RENAME that's been done on the view).
		 * Otherwise, just use what we can find in the TLE.
		 */
		if (context->resultDesc && colno <= context->resultDesc->natts)
			colname = NameStr(TupleDescAttr(context->resultDesc,
											colno - 1)->attname);
		else
			colname = tle->resname;

		/* Show AS unless the column's name is correct as-is */
		if (colname)			/* resname could be NULL */
		{
			if (attname == NULL || strcmp(attname, colname) != 0)
				appendStringInfo(&targetbuf, " AS %s", quote_identifier(colname));
		}

		/* Restore context's output buffer */
		context->buf = buf;

		/* Consider line-wrapping if enabled */
		if (PRETTY_INDENT(context) && context->wrapColumn >= 0)
		{
			int			leading_nl_pos;

			/* Does the new field start with a new line? */
			if (targetbuf.len > 0 && targetbuf.data[0] == '\n')
				leading_nl_pos = 0;
			else
				leading_nl_pos = -1;

			/* If so, we shouldn't add anything */
			if (leading_nl_pos >= 0)
			{
				/* instead, remove any trailing spaces currently in buf */
				removeStringInfoSpaces(buf);
			}
			else
			{
				char	   *trailing_nl;

				/* Locate the start of the current line in the output buffer */
				trailing_nl = strrchr(buf->data, '\n');
				if (trailing_nl == NULL)
					trailing_nl = buf->data;
				else
					trailing_nl++;

				/*
				 * Add a newline, plus some indentation, if the new field is
				 * not the first and either the new field would cause an
				 * overflow or the last field used more than one line.
				 */
				if (colno > 1 &&
					((strlen(trailing_nl) + targetbuf.len > context->wrapColumn) ||
					 last_was_multiline))
					appendContextKeyword(context, "", -PRETTYINDENT_STD,
										 PRETTYINDENT_STD, PRETTYINDENT_VAR);
			}

			/* Remember this field's multiline status for next iteration */
			last_was_multiline =
				(strchr(targetbuf.data + leading_nl_pos + 1, '\n') != NULL);
		}

		/* Add the new field */
		appendBinaryStringInfo(buf, targetbuf.data, targetbuf.len);
	}

	/* clean up */
	pfree(targetbuf.data);
}

static void
get_returning_clause(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;

	if (query->returningList)
	{
		bool		have_with = false;

		appendContextKeyword(context, " RETURNING",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);

		/* Add WITH (OLD/NEW) options, if they're not the defaults */
		if (query->returningOldAlias && strcmp(query->returningOldAlias, "old") != 0)
		{
			appendStringInfo(buf, " WITH (OLD AS %s",
							 quote_identifier(query->returningOldAlias));
			have_with = true;
		}
		if (query->returningNewAlias && strcmp(query->returningNewAlias, "new") != 0)
		{
			if (have_with)
				appendStringInfo(buf, ", NEW AS %s",
								 quote_identifier(query->returningNewAlias));
			else
			{
				appendStringInfo(buf, " WITH (NEW AS %s",
								 quote_identifier(query->returningNewAlias));
				have_with = true;
			}
		}
		if (have_with)
			appendStringInfoChar(buf, ')');

		/* Add the returning expressions themselves */
		get_target_list(query->returningList, context);
	}
}

static void
get_setop_query(Node *setOp, Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	bool		need_paren;

	/* Guard against excessively long or deeply-nested queries */
	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, query->rtable);
		Query	   *subquery = rte->subquery;

		Assert(subquery != NULL);

		/*
		 * We need parens if WITH, ORDER BY, FOR UPDATE, or LIMIT; see gram.y.
		 * Also add parens if the leaf query contains its own set operations.
		 * (That shouldn't happen unless one of the other clauses is also
		 * present, see transformSetOperationTree; but let's be safe.)
		 */
		need_paren = (subquery->cteList ||
					  subquery->sortClause ||
					  subquery->rowMarks ||
					  subquery->limitOffset ||
					  subquery->limitCount ||
					  subquery->setOperations);
		if (need_paren)
			appendStringInfoChar(buf, '(');
		get_query_def(subquery, buf, context->namespaces,
					  context->resultDesc, context->colNamesVisible,
					  context->prettyFlags, context->wrapColumn,
					  context->indentLevel);
		if (need_paren)
			appendStringInfoChar(buf, ')');
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;
		int			subindent;
		bool		save_colnamesvisible;

		/*
		 * We force parens when nesting two SetOperationStmts, except when the
		 * lefthand input is another setop of the same kind.  Syntactically,
		 * we could omit parens in rather more cases, but it seems best to use
		 * parens to flag cases where the setop operator changes.  If we use
		 * parens, we also increase the indentation level for the child query.
		 *
		 * There are some cases in which parens are needed around a leaf query
		 * too, but those are more easily handled at the next level down (see
		 * code above).
		 */
		if (IsA(op->larg, SetOperationStmt))
		{
			SetOperationStmt *lop = (SetOperationStmt *) op->larg;

			if (op->op == lop->op && op->all == lop->all)
				need_paren = false;
			else
				need_paren = true;
		}
		else
			need_paren = false;

		if (need_paren)
		{
			appendStringInfoChar(buf, '(');
			subindent = PRETTYINDENT_STD;
			appendContextKeyword(context, "", subindent, 0, 0);
		}
		else
			subindent = 0;

		get_setop_query(op->larg, query, context);

		if (need_paren)
			appendContextKeyword(context, ") ", -subindent, 0, 0);
		else if (PRETTY_INDENT(context))
			appendContextKeyword(context, "", -subindent, 0, 0);
		else
			appendStringInfoChar(buf, ' ');

		switch (op->op)
		{
			case SETOP_UNION:
				appendStringInfoString(buf, "UNION ");
				break;
			case SETOP_INTERSECT:
				appendStringInfoString(buf, "INTERSECT ");
				break;
			case SETOP_EXCEPT:
				appendStringInfoString(buf, "EXCEPT ");
				break;
			default:
				elog(ERROR, "unrecognized set op: %d",
					 (int) op->op);
		}
		if (op->all)
			appendStringInfoString(buf, "ALL ");

		/* Always parenthesize if RHS is another setop */
		need_paren = IsA(op->rarg, SetOperationStmt);

		/*
		 * The indentation code here is deliberately a bit different from that
		 * for the lefthand input, because we want the line breaks in
		 * different places.
		 */
		if (need_paren)
		{
			appendStringInfoChar(buf, '(');
			subindent = PRETTYINDENT_STD;
		}
		else
			subindent = 0;
		appendContextKeyword(context, "", subindent, 0, 0);

		/*
		 * The output column names of the RHS sub-select don't matter.
		 */
		save_colnamesvisible = context->colNamesVisible;
		context->colNamesVisible = false;

		get_setop_query(op->rarg, query, context);

		context->colNamesVisible = save_colnamesvisible;

		if (PRETTY_INDENT(context))
			context->indentLevel -= subindent;
		if (need_paren)
			appendContextKeyword(context, ")", 0, 0, 0);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
}

/*
 * Display a sort/group clause.
 *
 * Also returns the expression tree, so caller need not find it again.
 */
static Node *
get_rule_sortgroupclause(Index ref, List *tlist, bool force_colno,
						 deparse_context *context)
{
	StringInfo	buf = context->buf;
	TargetEntry *tle;
	Node	   *expr;

	tle = get_sortgroupref_tle(ref, tlist);
	expr = (Node *) tle->expr;

	/*
	 * Use column-number form if requested by caller.  Otherwise, if
	 * expression is a constant, force it to be dumped with an explicit cast
	 * as decoration --- this is because a simple integer constant is
	 * ambiguous (and will be misinterpreted by findTargetlistEntrySQL92()) if
	 * we dump it without any decoration.  Similarly, if it's just a Var,
	 * there is risk of misinterpretation if the column name is reassigned in
	 * the SELECT list, so we may need to force table qualification.  And, if
	 * it's anything more complex than a simple Var, then force extra parens
	 * around it, to ensure it can't be misinterpreted as a cube() or rollup()
	 * construct.
	 */
	if (force_colno)
	{
		Assert(!tle->resjunk);
		appendStringInfo(buf, "%d", tle->resno);
	}
	else if (!expr)
		 /* do nothing, probably can't happen */ ;
	else if (IsA(expr, Const))
		get_const_expr((Const *) expr, context, 1);
	else if (IsA(expr, Var))
	{
		/* Tell get_variable to check for name conflict */
		bool		save_varinorderby = context->varInOrderBy;

		context->varInOrderBy = true;
		(void) get_variable((Var *) expr, 0, false, context);
		context->varInOrderBy = save_varinorderby;
	}
	else
	{
		/*
		 * We must force parens for function-like expressions even if
		 * PRETTY_PAREN is off, since those are the ones in danger of
		 * misparsing. For other expressions we need to force them only if
		 * PRETTY_PAREN is on, since otherwise the expression will output them
		 * itself. (We can't skip the parens.)
		 */
		bool		need_paren = (PRETTY_PAREN(context)
								  || IsA(expr, FuncExpr)
								  || IsA(expr, Aggref)
								  || IsA(expr, WindowFunc)
								  || IsA(expr, JsonConstructorExpr));

		if (need_paren)
			appendStringInfoChar(context->buf, '(');
		get_rule_expr(expr, context, true);
		if (need_paren)
			appendStringInfoChar(context->buf, ')');
	}

	return expr;
}

/*
 * Display a GroupingSet
 */
static void
get_rule_groupingset(GroupingSet *gset, List *targetlist,
					 bool omit_parens, deparse_context *context)
{
	ListCell   *l;
	StringInfo	buf = context->buf;
	bool		omit_child_parens = true;
	char	   *sep = "";

	switch (gset->kind)
	{
		case GROUPING_SET_EMPTY:
			appendStringInfoString(buf, "()");
			return;

		case GROUPING_SET_SIMPLE:
			{
				if (!omit_parens || list_length(gset->content) != 1)
					appendStringInfoChar(buf, '(');

				foreach(l, gset->content)
				{
					Index		ref = lfirst_int(l);

					appendStringInfoString(buf, sep);
					get_rule_sortgroupclause(ref, targetlist,
											 false, context);
					sep = ", ";
				}

				if (!omit_parens || list_length(gset->content) != 1)
					appendStringInfoChar(buf, ')');
			}
			return;

		case GROUPING_SET_ROLLUP:
			appendStringInfoString(buf, "ROLLUP(");
			break;
		case GROUPING_SET_CUBE:
			appendStringInfoString(buf, "CUBE(");
			break;
		case GROUPING_SET_SETS:
			appendStringInfoString(buf, "GROUPING SETS (");
			omit_child_parens = false;
			break;
	}

	foreach(l, gset->content)
	{
		appendStringInfoString(buf, sep);
		get_rule_groupingset(lfirst(l), targetlist, omit_child_parens, context);
		sep = ", ";
	}

	appendStringInfoChar(buf, ')');
}

/*
 * Display an ORDER BY list.
 */
static void
get_rule_orderby(List *orderList, List *targetList,
				 bool force_colno, deparse_context *context)
{
	StringInfo	buf = context->buf;
	const char *sep;
	ListCell   *l;

	sep = "";
	foreach(l, orderList)
	{
		SortGroupClause *srt = (SortGroupClause *) lfirst(l);
		Node	   *sortexpr;
		Oid			sortcoltype;
		TypeCacheEntry *typentry;

		appendStringInfoString(buf, sep);
		sortexpr = get_rule_sortgroupclause(srt->tleSortGroupRef, targetList,
											force_colno, context);
		sortcoltype = exprType(sortexpr);
		/* See whether operator is default < or > for datatype */
		typentry = lookup_type_cache(sortcoltype,
									 TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);
		if (srt->sortop == typentry->lt_opr)
		{
			/* ASC is default, so emit nothing for it */
			if (srt->nulls_first)
				appendStringInfoString(buf, " NULLS FIRST");
		}
		else if (srt->sortop == typentry->gt_opr)
		{
			appendStringInfoString(buf, " DESC");
			/* DESC defaults to NULLS FIRST */
			if (!srt->nulls_first)
				appendStringInfoString(buf, " NULLS LAST");
		}
		else
		{
			appendStringInfo(buf, " USING %s",
							 generate_operator_name(srt->sortop,
													sortcoltype,
													sortcoltype));
			/* be specific to eliminate ambiguity */
			if (srt->nulls_first)
				appendStringInfoString(buf, " NULLS FIRST");
			else
				appendStringInfoString(buf, " NULLS LAST");
		}
		sep = ", ";
	}
}

/*
 * Display a WINDOW clause.
 *
 * Note that the windowClause list might contain only anonymous window
 * specifications, in which case we should print nothing here.
 */
static void
get_rule_windowclause(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	const char *sep;
	ListCell   *l;

	sep = NULL;
	foreach(l, query->windowClause)
	{
		WindowClause *wc = (WindowClause *) lfirst(l);

		if (wc->name == NULL)
			continue;			/* ignore anonymous windows */

		if (sep == NULL)
			appendContextKeyword(context, " WINDOW ",
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		else
			appendStringInfoString(buf, sep);

		appendStringInfo(buf, "%s AS ", quote_identifier(wc->name));

		get_rule_windowspec(wc, query->targetList, context);

		sep = ", ";
	}
}

/*
 * Display a window definition
 */
static void
get_rule_windowspec(WindowClause *wc, List *targetList,
					deparse_context *context)
{
	StringInfo	buf = context->buf;
	bool		needspace = false;
	const char *sep;
	ListCell   *l;

	appendStringInfoChar(buf, '(');
	if (wc->refname)
	{
		appendStringInfoString(buf, quote_identifier(wc->refname));
		needspace = true;
	}
	/* partition clauses are always inherited, so only print if no refname */
	if (wc->partitionClause && !wc->refname)
	{
		if (needspace)
			appendStringInfoChar(buf, ' ');
		appendStringInfoString(buf, "PARTITION BY ");
		sep = "";
		foreach(l, wc->partitionClause)
		{
			SortGroupClause *grp = (SortGroupClause *) lfirst(l);

			appendStringInfoString(buf, sep);
			get_rule_sortgroupclause(grp->tleSortGroupRef, targetList,
									 false, context);
			sep = ", ";
		}
		needspace = true;
	}
	/* print ordering clause only if not inherited */
	if (wc->orderClause && !wc->copiedOrder)
	{
		if (needspace)
			appendStringInfoChar(buf, ' ');
		appendStringInfoString(buf, "ORDER BY ");
		get_rule_orderby(wc->orderClause, targetList, false, context);
		needspace = true;
	}
	/* framing clause is never inherited, so print unless it's default */
	if (wc->frameOptions & FRAMEOPTION_NONDEFAULT)
	{
		if (needspace)
			appendStringInfoChar(buf, ' ');
		if (wc->frameOptions & FRAMEOPTION_RANGE)
			appendStringInfoString(buf, "RANGE ");
		else if (wc->frameOptions & FRAMEOPTION_ROWS)
			appendStringInfoString(buf, "ROWS ");
		else if (wc->frameOptions & FRAMEOPTION_GROUPS)
			appendStringInfoString(buf, "GROUPS ");
		else
			Assert(false);
		if (wc->frameOptions & FRAMEOPTION_BETWEEN)
			appendStringInfoString(buf, "BETWEEN ");
		if (wc->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING)
			appendStringInfoString(buf, "UNBOUNDED PRECEDING ");
		else if (wc->frameOptions & FRAMEOPTION_START_CURRENT_ROW)
			appendStringInfoString(buf, "CURRENT ROW ");
		else if (wc->frameOptions & FRAMEOPTION_START_OFFSET)
		{
			get_rule_expr(wc->startOffset, context, false);
			if (wc->frameOptions & FRAMEOPTION_START_OFFSET_PRECEDING)
				appendStringInfoString(buf, " PRECEDING ");
			else if (wc->frameOptions & FRAMEOPTION_START_OFFSET_FOLLOWING)
				appendStringInfoString(buf, " FOLLOWING ");
			else
				Assert(false);
		}
		else
			Assert(false);
		if (wc->frameOptions & FRAMEOPTION_BETWEEN)
		{
			appendStringInfoString(buf, "AND ");
			if (wc->frameOptions & FRAMEOPTION_END_UNBOUNDED_FOLLOWING)
				appendStringInfoString(buf, "UNBOUNDED FOLLOWING ");
			else if (wc->frameOptions & FRAMEOPTION_END_CURRENT_ROW)
				appendStringInfoString(buf, "CURRENT ROW ");
			else if (wc->frameOptions & FRAMEOPTION_END_OFFSET)
			{
				get_rule_expr(wc->endOffset, context, false);
				if (wc->frameOptions & FRAMEOPTION_END_OFFSET_PRECEDING)
					appendStringInfoString(buf, " PRECEDING ");
				else if (wc->frameOptions & FRAMEOPTION_END_OFFSET_FOLLOWING)
					appendStringInfoString(buf, " FOLLOWING ");
				else
					Assert(false);
			}
			else
				Assert(false);
		}
		if (wc->frameOptions & FRAMEOPTION_EXCLUDE_CURRENT_ROW)
			appendStringInfoString(buf, "EXCLUDE CURRENT ROW ");
		else if (wc->frameOptions & FRAMEOPTION_EXCLUDE_GROUP)
			appendStringInfoString(buf, "EXCLUDE GROUP ");
		else if (wc->frameOptions & FRAMEOPTION_EXCLUDE_TIES)
			appendStringInfoString(buf, "EXCLUDE TIES ");
		/* we will now have a trailing space; remove it */
		buf->len--;
	}
	appendStringInfoChar(buf, ')');
}

/* ----------
 * get_insert_query_def			- Parse back an INSERT parsetree
 * ----------
 */
static void
get_insert_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *select_rte = NULL;
	RangeTblEntry *values_rte = NULL;
	RangeTblEntry *rte;
	char	   *sep;
	ListCell   *l;
	List	   *strippedexprs;

	/* Insert the WITH clause if given */
	get_with_clause(query, context);

	/*
	 * If it's an INSERT ... SELECT or multi-row VALUES, there will be a
	 * single RTE for the SELECT or VALUES.  Plain VALUES has neither.
	 */
	foreach(l, query->rtable)
	{
		rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind == RTE_SUBQUERY)
		{
			if (select_rte)
				elog(ERROR, "too many subquery RTEs in INSERT");
			select_rte = rte;
		}

		if (rte->rtekind == RTE_VALUES)
		{
			if (values_rte)
				elog(ERROR, "too many values RTEs in INSERT");
			values_rte = rte;
		}
	}
	if (select_rte && values_rte)
		elog(ERROR, "both subquery and values RTEs in INSERT");

	/*
	 * Start the query with INSERT INTO relname
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	Assert(rte->rtekind == RTE_RELATION);

	if (PRETTY_INDENT(context))
	{
		context->indentLevel += PRETTYINDENT_STD;
		appendStringInfoChar(buf, ' ');
	}
	appendStringInfo(buf, "INSERT INTO %s",
					 generate_relation_name(rte->relid, NIL));

	/* Print the relation alias, if needed; INSERT requires explicit AS */
	get_rte_alias(rte, query->resultRelation, true, context);

	/* always want a space here */
	appendStringInfoChar(buf, ' ');

	/*
	 * Add the insert-column-names list.  Any indirection decoration needed on
	 * the column names can be inferred from the top targetlist.
	 */
	strippedexprs = NIL;
	sep = "";
	if (query->targetList)
		appendStringInfoChar(buf, '(');
	foreach(l, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
			continue;			/* ignore junk entries */

		appendStringInfoString(buf, sep);
		sep = ", ";

		/*
		 * Put out name of target column; look in the catalogs, not at
		 * tle->resname, since resname will fail to track RENAME.
		 */
		appendStringInfoString(buf,
							   quote_identifier(get_attname(rte->relid,
															tle->resno,
															false)));

		/*
		 * Print any indirection needed (subfields or subscripts), and strip
		 * off the top-level nodes representing the indirection assignments.
		 * Add the stripped expressions to strippedexprs.  (If it's a
		 * single-VALUES statement, the stripped expressions are the VALUES to
		 * print below.  Otherwise they're just Vars and not really
		 * interesting.)
		 */
		strippedexprs = lappend(strippedexprs,
								processIndirection((Node *) tle->expr,
												   context));
	}
	if (query->targetList)
		appendStringInfoString(buf, ") ");

	if (query->override)
	{
		if (query->override == OVERRIDING_SYSTEM_VALUE)
			appendStringInfoString(buf, "OVERRIDING SYSTEM VALUE ");
		else if (query->override == OVERRIDING_USER_VALUE)
			appendStringInfoString(buf, "OVERRIDING USER VALUE ");
	}

	if (select_rte)
	{
		/* Add the SELECT */
		get_query_def(select_rte->subquery, buf, context->namespaces, NULL,
					  false,
					  context->prettyFlags, context->wrapColumn,
					  context->indentLevel);
	}
	else if (values_rte)
	{
		/* Add the multi-VALUES expression lists */
		get_values_def(values_rte->values_lists, context);
	}
	else if (strippedexprs)
	{
		/* Add the single-VALUES expression list */
		appendContextKeyword(context, "VALUES (",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 2);
		get_rule_list_toplevel(strippedexprs, context, false);
		appendStringInfoChar(buf, ')');
	}
	else
	{
		/* No expressions, so it must be DEFAULT VALUES */
		appendStringInfoString(buf, "DEFAULT VALUES");
	}

	/* Add ON CONFLICT if present */
	if (query->onConflict)
	{
		OnConflictExpr *confl = query->onConflict;

		appendStringInfoString(buf, " ON CONFLICT");

		if (confl->arbiterElems)
		{
			/* Add the single-VALUES expression list */
			appendStringInfoChar(buf, '(');
			get_rule_expr((Node *) confl->arbiterElems, context, false);
			appendStringInfoChar(buf, ')');

			/* Add a WHERE clause (for partial indexes) if given */
			if (confl->arbiterWhere != NULL)
			{
				bool		save_varprefix;

				/*
				 * Force non-prefixing of Vars, since parser assumes that they
				 * belong to target relation.  WHERE clause does not use
				 * InferenceElem, so this is separately required.
				 */
				save_varprefix = context->varprefix;
				context->varprefix = false;

				appendContextKeyword(context, " WHERE ",
									 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
				get_rule_expr(confl->arbiterWhere, context, false);

				context->varprefix = save_varprefix;
			}
		}
		else if (OidIsValid(confl->constraint))
		{
			char	   *constraint = get_constraint_name(confl->constraint);

			if (!constraint)
				elog(ERROR, "cache lookup failed for constraint %u",
					 confl->constraint);
			appendStringInfo(buf, " ON CONSTRAINT %s",
							 quote_identifier(constraint));
		}

		if (confl->action == ONCONFLICT_NOTHING)
		{
			appendStringInfoString(buf, " DO NOTHING");
		}
		else
		{
			appendStringInfoString(buf, " DO UPDATE SET ");
			/* Deparse targetlist */
			get_update_query_targetlist_def(query, confl->onConflictSet,
											context, rte);

			/* Add a WHERE clause if given */
			if (confl->onConflictWhere != NULL)
			{
				appendContextKeyword(context, " WHERE ",
									 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
				get_rule_expr(confl->onConflictWhere, context, false);
			}
		}
	}

	/* Add RETURNING if present */
	if (query->returningList)
		get_returning_clause(query, context);
}


/* ----------
 * get_update_query_def			- Parse back an UPDATE parsetree
 * ----------
 */
static void
get_update_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *rte;

	/* Insert the WITH clause if given */
	get_with_clause(query, context);

	/*
	 * Start the query with UPDATE relname SET
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	Assert(rte->rtekind == RTE_RELATION);
	if (PRETTY_INDENT(context))
	{
		appendStringInfoChar(buf, ' ');
		context->indentLevel += PRETTYINDENT_STD;
	}
	appendStringInfo(buf, "UPDATE %s%s",
					 only_marker(rte),
					 generate_relation_name(rte->relid, NIL));

	/* Print the relation alias, if needed */
	get_rte_alias(rte, query->resultRelation, false, context);

	appendStringInfoString(buf, " SET ");

	/* Deparse targetlist */
	get_update_query_targetlist_def(query, query->targetList, context, rte);

	/* Add the FROM clause if needed */
	get_from_clause(query, " FROM ", context);

	/* Add a WHERE clause if given */
	if (query->jointree->quals != NULL)
	{
		appendContextKeyword(context, " WHERE ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_rule_expr(query->jointree->quals, context, false);
	}

	/* Add RETURNING if present */
	if (query->returningList)
		get_returning_clause(query, context);
}


/* ----------
 * get_update_query_targetlist_def			- Parse back an UPDATE targetlist
 * ----------
 */
static void
get_update_query_targetlist_def(Query *query, List *targetList,
								deparse_context *context, RangeTblEntry *rte)
{
	StringInfo	buf = context->buf;
	ListCell   *l;
	ListCell   *next_ma_cell;
	int			remaining_ma_columns;
	const char *sep;
	SubLink    *cur_ma_sublink;
	List	   *ma_sublinks;

	/*
	 * Prepare to deal with MULTIEXPR assignments: collect the source SubLinks
	 * into a list.  We expect them to appear, in ID order, in resjunk tlist
	 * entries.
	 */
	ma_sublinks = NIL;
	if (query->hasSubLinks)		/* else there can't be any */
	{
		foreach(l, targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(l);

			if (tle->resjunk && IsA(tle->expr, SubLink))
			{
				SubLink    *sl = (SubLink *) tle->expr;

				if (sl->subLinkType == MULTIEXPR_SUBLINK)
				{
					ma_sublinks = lappend(ma_sublinks, sl);
					Assert(sl->subLinkId == list_length(ma_sublinks));
				}
			}
		}
	}
	next_ma_cell = list_head(ma_sublinks);
	cur_ma_sublink = NULL;
	remaining_ma_columns = 0;

	/* Add the comma separated list of 'attname = value' */
	sep = "";
	foreach(l, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Node	   *expr;

		if (tle->resjunk)
			continue;			/* ignore junk entries */

		/* Emit separator (OK whether we're in multiassignment or not) */
		appendStringInfoString(buf, sep);
		sep = ", ";

		/*
		 * Check to see if we're starting a multiassignment group: if so,
		 * output a left paren.
		 */
		if (next_ma_cell != NULL && cur_ma_sublink == NULL)
		{
			/*
			 * We must dig down into the expr to see if it's a PARAM_MULTIEXPR
			 * Param.  That could be buried under FieldStores and
			 * SubscriptingRefs and CoerceToDomains (cf processIndirection()),
			 * and underneath those there could be an implicit type coercion.
			 * Because we would ignore implicit type coercions anyway, we
			 * don't need to be as careful as processIndirection() is about
			 * descending past implicit CoerceToDomains.
			 */
			expr = (Node *) tle->expr;
			while (expr)
			{
				if (IsA(expr, FieldStore))
				{
					FieldStore *fstore = (FieldStore *) expr;

					expr = (Node *) linitial(fstore->newvals);
				}
				else if (IsA(expr, SubscriptingRef))
				{
					SubscriptingRef *sbsref = (SubscriptingRef *) expr;

					if (sbsref->refassgnexpr == NULL)
						break;

					expr = (Node *) sbsref->refassgnexpr;
				}
				else if (IsA(expr, CoerceToDomain))
				{
					CoerceToDomain *cdomain = (CoerceToDomain *) expr;

					if (cdomain->coercionformat != COERCE_IMPLICIT_CAST)
						break;
					expr = (Node *) cdomain->arg;
				}
				else
					break;
			}
			expr = strip_implicit_coercions(expr);

			if (expr && IsA(expr, Param) &&
				((Param *) expr)->paramkind == PARAM_MULTIEXPR)
			{
				cur_ma_sublink = (SubLink *) lfirst(next_ma_cell);
				next_ma_cell = lnext(ma_sublinks, next_ma_cell);
				remaining_ma_columns = count_nonjunk_tlist_entries(((Query *) cur_ma_sublink->subselect)->targetList);
				Assert(((Param *) expr)->paramid ==
					   ((cur_ma_sublink->subLinkId << 16) | 1));
				appendStringInfoChar(buf, '(');
			}
		}

		/*
		 * Put out name of target column; look in the catalogs, not at
		 * tle->resname, since resname will fail to track RENAME.
		 */
		appendStringInfoString(buf,
							   quote_identifier(get_attname(rte->relid,
															tle->resno,
															false)));

		/*
		 * Print any indirection needed (subfields or subscripts), and strip
		 * off the top-level nodes representing the indirection assignments.
		 */
		expr = processIndirection((Node *) tle->expr, context);

		/*
		 * If we're in a multiassignment, skip printing anything more, unless
		 * this is the last column; in which case, what we print should be the
		 * sublink, not the Param.
		 */
		if (cur_ma_sublink != NULL)
		{
			if (--remaining_ma_columns > 0)
				continue;		/* not the last column of multiassignment */
			appendStringInfoChar(buf, ')');
			expr = (Node *) cur_ma_sublink;
			cur_ma_sublink = NULL;
		}

		appendStringInfoString(buf, " = ");

		get_rule_expr(expr, context, false);
	}
}


/* ----------
 * get_delete_query_def			- Parse back a DELETE parsetree
 * ----------
 */
static void
get_delete_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *rte;

	/* Insert the WITH clause if given */
	get_with_clause(query, context);

	/*
	 * Start the query with DELETE FROM relname
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	Assert(rte->rtekind == RTE_RELATION);
	if (PRETTY_INDENT(context))
	{
		appendStringInfoChar(buf, ' ');
		context->indentLevel += PRETTYINDENT_STD;
	}
	appendStringInfo(buf, "DELETE FROM %s%s",
					 only_marker(rte),
					 generate_relation_name(rte->relid, NIL));

	/* Print the relation alias, if needed */
	get_rte_alias(rte, query->resultRelation, false, context);

	/* Add the USING clause if given */
	get_from_clause(query, " USING ", context);

	/* Add a WHERE clause if given */
	if (query->jointree->quals != NULL)
	{
		appendContextKeyword(context, " WHERE ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_rule_expr(query->jointree->quals, context, false);
	}

	/* Add RETURNING if present */
	if (query->returningList)
		get_returning_clause(query, context);
}


/* ----------
 * get_merge_query_def				- Parse back a MERGE parsetree
 * ----------
 */
static void
get_merge_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *rte;
	ListCell   *lc;
	bool		haveNotMatchedBySource;

	/* Insert the WITH clause if given */
	get_with_clause(query, context);

	/*
	 * Start the query with MERGE INTO relname
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	Assert(rte->rtekind == RTE_RELATION);
	if (PRETTY_INDENT(context))
	{
		appendStringInfoChar(buf, ' ');
		context->indentLevel += PRETTYINDENT_STD;
	}
	appendStringInfo(buf, "MERGE INTO %s%s",
					 only_marker(rte),
					 generate_relation_name(rte->relid, NIL));

	/* Print the relation alias, if needed */
	get_rte_alias(rte, query->resultRelation, false, context);

	/* Print the source relation and join clause */
	get_from_clause(query, " USING ", context);
	appendContextKeyword(context, " ON ",
						 -PRETTYINDENT_STD, PRETTYINDENT_STD, 2);
	get_rule_expr(query->mergeJoinCondition, context, false);

	/*
	 * Test for any NOT MATCHED BY SOURCE actions.  If there are none, then
	 * any NOT MATCHED BY TARGET actions are output as "WHEN NOT MATCHED", per
	 * SQL standard.  Otherwise, we have a non-SQL-standard query, so output
	 * "BY SOURCE" / "BY TARGET" qualifiers for all NOT MATCHED actions, to be
	 * more explicit.
	 */
	haveNotMatchedBySource = false;
	foreach(lc, query->mergeActionList)
	{
		MergeAction *action = lfirst_node(MergeAction, lc);

		if (action->matchKind == MERGE_WHEN_NOT_MATCHED_BY_SOURCE)
		{
			haveNotMatchedBySource = true;
			break;
		}
	}

	/* Print each merge action */
	foreach(lc, query->mergeActionList)
	{
		MergeAction *action = lfirst_node(MergeAction, lc);

		appendContextKeyword(context, " WHEN ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 2);
		switch (action->matchKind)
		{
			case MERGE_WHEN_MATCHED:
				appendStringInfoString(buf, "MATCHED");
				break;
			case MERGE_WHEN_NOT_MATCHED_BY_SOURCE:
				appendStringInfoString(buf, "NOT MATCHED BY SOURCE");
				break;
			case MERGE_WHEN_NOT_MATCHED_BY_TARGET:
				if (haveNotMatchedBySource)
					appendStringInfoString(buf, "NOT MATCHED BY TARGET");
				else
					appendStringInfoString(buf, "NOT MATCHED");
				break;
			default:
				elog(ERROR, "unrecognized matchKind: %d",
					 (int) action->matchKind);
		}

		if (action->qual)
		{
			appendContextKeyword(context, " AND ",
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 3);
			get_rule_expr(action->qual, context, false);
		}
		appendContextKeyword(context, " THEN ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 3);

		if (action->commandType == CMD_INSERT)
		{
			/* This generally matches get_insert_query_def() */
			List	   *strippedexprs = NIL;
			const char *sep = "";
			ListCell   *lc2;

			appendStringInfoString(buf, "INSERT");

			if (action->targetList)
				appendStringInfoString(buf, " (");
			foreach(lc2, action->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc2);

				Assert(!tle->resjunk);

				appendStringInfoString(buf, sep);
				sep = ", ";

				appendStringInfoString(buf,
									   quote_identifier(get_attname(rte->relid,
																	tle->resno,
																	false)));
				strippedexprs = lappend(strippedexprs,
										processIndirection((Node *) tle->expr,
														   context));
			}
			if (action->targetList)
				appendStringInfoChar(buf, ')');

			if (action->override)
			{
				if (action->override == OVERRIDING_SYSTEM_VALUE)
					appendStringInfoString(buf, " OVERRIDING SYSTEM VALUE");
				else if (action->override == OVERRIDING_USER_VALUE)
					appendStringInfoString(buf, " OVERRIDING USER VALUE");
			}

			if (strippedexprs)
			{
				appendContextKeyword(context, " VALUES (",
									 -PRETTYINDENT_STD, PRETTYINDENT_STD, 4);
				get_rule_list_toplevel(strippedexprs, context, false);
				appendStringInfoChar(buf, ')');
			}
			else
				appendStringInfoString(buf, " DEFAULT VALUES");
		}
		else if (action->commandType == CMD_UPDATE)
		{
			appendStringInfoString(buf, "UPDATE SET ");
			get_update_query_targetlist_def(query, action->targetList,
											context, rte);
		}
		else if (action->commandType == CMD_DELETE)
			appendStringInfoString(buf, "DELETE");
		else if (action->commandType == CMD_NOTHING)
			appendStringInfoString(buf, "DO NOTHING");
	}

	/* Add RETURNING if present */
	if (query->returningList)
		get_returning_clause(query, context);
}


/* ----------
 * get_utility_query_def			- Parse back a UTILITY parsetree
 * ----------
 */
static void
get_utility_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;

	if (query->utilityStmt && IsA(query->utilityStmt, NotifyStmt))
	{
		NotifyStmt *stmt = (NotifyStmt *) query->utilityStmt;

		appendContextKeyword(context, "",
							 0, PRETTYINDENT_STD, 1);
		appendStringInfo(buf, "NOTIFY %s",
						 quote_identifier(stmt->conditionname));
		if (stmt->payload)
		{
			appendStringInfoString(buf, ", ");
			simple_quote_literal(buf, stmt->payload);
		}
	}
	else
	{
		/* Currently only NOTIFY utility commands can appear in rules */
		elog(ERROR, "unexpected utility statement type");
	}
}

/*
 * Display a Var appropriately.
 *
 * In some cases (currently only when recursing into an unnamed join)
 * the Var's varlevelsup has to be interpreted with respect to a context
 * above the current one; levelsup indicates the offset.
 *
 * If istoplevel is true, the Var is at the top level of a SELECT's
 * targetlist, which means we need special treatment of whole-row Vars.
 * Instead of the normal "tab.*", we'll print "tab.*::typename", which is a
 * dirty hack to prevent "tab.*" from being expanded into multiple columns.
 * (The parser will strip the useless coercion, so no inefficiency is added in
 * dump and reload.)  We used to print just "tab" in such cases, but that is
 * ambiguous and will yield the wrong result if "tab" is also a plain column
 * name in the query.
 *
 * Returns the attname of the Var, or NULL if the Var has no attname (because
 * it is a whole-row Var or a subplan output reference).
 */
static char *
get_variable(Var *var, int levelsup, bool istoplevel, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *rte;
	AttrNumber	attnum;
	int			netlevelsup;
	deparse_namespace *dpns;
	int			varno;
	AttrNumber	varattno;
	deparse_columns *colinfo;
	char	   *refname;
	char	   *attname;
	bool		need_prefix;

	/* Find appropriate nesting depth */
	netlevelsup = var->varlevelsup + levelsup;
	if (netlevelsup >= list_length(context->namespaces))
		elog(ERROR, "bogus varlevelsup: %d offset %d",
			 var->varlevelsup, levelsup);
	dpns = (deparse_namespace *) list_nth(context->namespaces,
										  netlevelsup);

	/*
	 * If we have a syntactic referent for the Var, and we're working from a
	 * parse tree, prefer to use the syntactic referent.  Otherwise, fall back
	 * on the semantic referent.  (Forcing use of the semantic referent when
	 * printing plan trees is a design choice that's perhaps more motivated by
	 * backwards compatibility than anything else.  But it does have the
	 * advantage of making plans more explicit.)
	 */
	if (var->varnosyn > 0 && dpns->plan == NULL)
	{
		varno = var->varnosyn;
		varattno = var->varattnosyn;
	}
	else
	{
		varno = var->varno;
		varattno = var->varattno;
	}

	/*
	 * Try to find the relevant RTE in this rtable.  In a plan tree, it's
	 * likely that varno is OUTER_VAR or INNER_VAR, in which case we must dig
	 * down into the subplans, or INDEX_VAR, which is resolved similarly. Also
	 * find the aliases previously assigned for this RTE.
	 */
	if (varno >= 1 && varno <= list_length(dpns->rtable))
	{
		/*
		 * We might have been asked to map child Vars to some parent relation.
		 */
		if (context->appendparents && dpns->appendrels)
		{
			int			pvarno = varno;
			AttrNumber	pvarattno = varattno;
			AppendRelInfo *appinfo = dpns->appendrels[pvarno];
			bool		found = false;

			/* Only map up to inheritance parents, not UNION ALL appendrels */
			while (appinfo &&
				   rt_fetch(appinfo->parent_relid,
							dpns->rtable)->rtekind == RTE_RELATION)
			{
				found = false;
				if (pvarattno > 0)	/* system columns stay as-is */
				{
					if (pvarattno > appinfo->num_child_cols)
						break;	/* safety check */
					pvarattno = appinfo->parent_colnos[pvarattno - 1];
					if (pvarattno == 0)
						break;	/* Var is local to child */
				}

				pvarno = appinfo->parent_relid;
				found = true;

				/* If the parent is itself a child, continue up. */
				Assert(pvarno > 0 && pvarno <= list_length(dpns->rtable));
				appinfo = dpns->appendrels[pvarno];
			}

			/*
			 * If we found an ancestral rel, and that rel is included in
			 * appendparents, print that column not the original one.
			 */
			if (found && bms_is_member(pvarno, context->appendparents))
			{
				varno = pvarno;
				varattno = pvarattno;
			}
		}

		rte = rt_fetch(varno, dpns->rtable);

		/* might be returning old/new column value */
		if (var->varreturningtype == VAR_RETURNING_OLD)
			refname = dpns->ret_old_alias;
		else if (var->varreturningtype == VAR_RETURNING_NEW)
			refname = dpns->ret_new_alias;
		else
			refname = (char *) list_nth(dpns->rtable_names, varno - 1);

		colinfo = deparse_columns_fetch(varno, dpns);
		attnum = varattno;
	}
	else
	{
		resolve_special_varno((Node *) var, context,
							  get_special_variable, NULL);
		return NULL;
	}

	/*
	 * The planner will sometimes emit Vars referencing resjunk elements of a
	 * subquery's target list (this is currently only possible if it chooses
	 * to generate a "physical tlist" for a SubqueryScan or CteScan node).
	 * Although we prefer to print subquery-referencing Vars using the
	 * subquery's alias, that's not possible for resjunk items since they have
	 * no alias.  So in that case, drill down to the subplan and print the
	 * contents of the referenced tlist item.  This works because in a plan
	 * tree, such Vars can only occur in a SubqueryScan or CteScan node, and
	 * we'll have set dpns->inner_plan to reference the child plan node.
	 */
	if ((rte->rtekind == RTE_SUBQUERY || rte->rtekind == RTE_CTE) &&
		attnum > list_length(rte->eref->colnames) &&
		dpns->inner_plan)
	{
		TargetEntry *tle;
		deparse_namespace save_dpns;

		tle = get_tle_by_resno(dpns->inner_tlist, attnum);
		if (!tle)
			elog(ERROR, "invalid attnum %d for relation \"%s\"",
				 attnum, rte->eref->aliasname);

		Assert(netlevelsup == 0);
		push_child_plan(dpns, dpns->inner_plan, &save_dpns);

		/*
		 * Force parentheses because our caller probably assumed a Var is a
		 * simple expression.
		 */
		if (!IsA(tle->expr, Var))
			appendStringInfoChar(buf, '(');
		get_rule_expr((Node *) tle->expr, context, true);
		if (!IsA(tle->expr, Var))
			appendStringInfoChar(buf, ')');

		pop_child_plan(dpns, &save_dpns);
		return NULL;
	}

	/*
	 * If it's an unnamed join, look at the expansion of the alias variable.
	 * If it's a simple reference to one of the input vars, then recursively
	 * print the name of that var instead.  When it's not a simple reference,
	 * we have to just print the unqualified join column name.  (This can only
	 * happen with "dangerous" merged columns in a JOIN USING; we took pains
	 * previously to make the unqualified column name unique in such cases.)
	 *
	 * This wouldn't work in decompiling plan trees, because we don't store
	 * joinaliasvars lists after planning; but a plan tree should never
	 * contain a join alias variable.
	 */
	if (rte->rtekind == RTE_JOIN && rte->alias == NULL)
	{
		if (rte->joinaliasvars == NIL)
			elog(ERROR, "cannot decompile join alias var in plan tree");
		if (attnum > 0)
		{
			Var		   *aliasvar;

			aliasvar = (Var *) list_nth(rte->joinaliasvars, attnum - 1);
			/* we intentionally don't strip implicit coercions here */
			if (aliasvar && IsA(aliasvar, Var))
			{
				return get_variable(aliasvar, var->varlevelsup + levelsup,
									istoplevel, context);
			}
		}

		/*
		 * Unnamed join has no refname.  (Note: since it's unnamed, there is
		 * no way the user could have referenced it to create a whole-row Var
		 * for it.  So we don't have to cover that case below.)
		 */
		Assert(refname == NULL);
	}

	if (attnum == InvalidAttrNumber)
		attname = NULL;
	else if (attnum > 0)
	{
		/* Get column name to use from the colinfo struct */
		if (attnum > colinfo->num_cols)
			elog(ERROR, "invalid attnum %d for relation \"%s\"",
				 attnum, rte->eref->aliasname);
		attname = colinfo->colnames[attnum - 1];

		/*
		 * If we find a Var referencing a dropped column, it seems better to
		 * print something (anything) than to fail.  In general this should
		 * not happen, but it used to be possible for some cases involving
		 * functions returning named composite types, and perhaps there are
		 * still bugs out there.
		 */
		if (attname == NULL)
			attname = "?dropped?column?";
	}
	else
	{
		/* System column - name is fixed, get it from the catalog */
		attname = get_rte_attribute_name(rte, attnum);
	}

	need_prefix = (context->varprefix || attname == NULL ||
				   var->varreturningtype != VAR_RETURNING_DEFAULT);

	/*
	 * If we're considering a plain Var in an ORDER BY (but not GROUP BY)
	 * clause, we may need to add a table-name prefix to prevent
	 * findTargetlistEntrySQL92 from misinterpreting the name as an
	 * output-column name.  To avoid cluttering the output with unnecessary
	 * prefixes, do so only if there is a name match to a SELECT tlist item
	 * that is different from the Var.
	 */
	if (context->varInOrderBy && !context->inGroupBy && !need_prefix)
	{
		int			colno = 0;

		foreach_node(TargetEntry, tle, context->targetList)
		{
			char	   *colname;

			if (tle->resjunk)
				continue;		/* ignore junk entries */
			colno++;

			/* This must match colname-choosing logic in get_target_list() */
			if (context->resultDesc && colno <= context->resultDesc->natts)
				colname = NameStr(TupleDescAttr(context->resultDesc,
												colno - 1)->attname);
			else
				colname = tle->resname;

			if (colname && strcmp(colname, attname) == 0 &&
				!equal(var, tle->expr))
			{
				need_prefix = true;
				break;
			}
		}
	}

	if (refname && need_prefix)
	{
		appendStringInfoString(buf, quote_identifier(refname));
		appendStringInfoChar(buf, '.');
	}
	if (attname)
		appendStringInfoString(buf, quote_identifier(attname));
	else
	{
		appendStringInfoChar(buf, '*');
		if (istoplevel)
			appendStringInfo(buf, "::%s",
							 format_type_with_typemod(var->vartype,
													  var->vartypmod));
	}

	return attname;
}

/*
 * Deparse a Var which references OUTER_VAR, INNER_VAR, or INDEX_VAR.  This
 * routine is actually a callback for resolve_special_varno, which handles
 * finding the correct TargetEntry.  We get the expression contained in that
 * TargetEntry and just need to deparse it, a job we can throw back on
 * get_rule_expr.
 */
static void
get_special_variable(Node *node, deparse_context *context, void *callback_arg)
{
	StringInfo	buf = context->buf;

	/*
	 * For a non-Var referent, force parentheses because our caller probably
	 * assumed a Var is a simple expression.
	 */
	if (!IsA(node, Var))
		appendStringInfoChar(buf, '(');
	get_rule_expr(node, context, true);
	if (!IsA(node, Var))
		appendStringInfoChar(buf, ')');
}

/*
 * Chase through plan references to special varnos (OUTER_VAR, INNER_VAR,
 * INDEX_VAR) until we find a real Var or some kind of non-Var node; then,
 * invoke the callback provided.
 */
static void
resolve_special_varno(Node *node, deparse_context *context,
					  rsv_callback callback, void *callback_arg)
{
	Var		   *var;
	deparse_namespace *dpns;

	/* This function is recursive, so let's be paranoid. */
	check_stack_depth();

	/* If it's not a Var, invoke the callback. */
	if (!IsA(node, Var))
	{
		(*callback) (node, context, callback_arg);
		return;
	}

	/* Find appropriate nesting depth */
	var = (Var *) node;
	dpns = (deparse_namespace *) list_nth(context->namespaces,
										  var->varlevelsup);

	/*
	 * If varno is special, recurse.  (Don't worry about varnosyn; if we're
	 * here, we already decided not to use that.)
	 */
	if (var->varno == OUTER_VAR && dpns->outer_tlist)
	{
		TargetEntry *tle;
		deparse_namespace save_dpns;
		Bitmapset  *save_appendparents;

		tle = get_tle_by_resno(dpns->outer_tlist, var->varattno);
		if (!tle)
			elog(ERROR, "bogus varattno for OUTER_VAR var: %d", var->varattno);

		/*
		 * If we're descending to the first child of an Append or MergeAppend,
		 * update appendparents.  This will affect deparsing of all Vars
		 * appearing within the eventually-resolved subexpression.
		 */
		save_appendparents = context->appendparents;

		if (IsA(dpns->plan, Append))
			context->appendparents = bms_union(context->appendparents,
											   ((Append *) dpns->plan)->apprelids);
		else if (IsA(dpns->plan, MergeAppend))
			context->appendparents = bms_union(context->appendparents,
											   ((MergeAppend *) dpns->plan)->apprelids);

		push_child_plan(dpns, dpns->outer_plan, &save_dpns);
		resolve_special_varno((Node *) tle->expr, context,
							  callback, callback_arg);
		pop_child_plan(dpns, &save_dpns);
		context->appendparents = save_appendparents;
		return;
	}
	else if (var->varno == INNER_VAR && dpns->inner_tlist)
	{
		TargetEntry *tle;
		deparse_namespace save_dpns;

		tle = get_tle_by_resno(dpns->inner_tlist, var->varattno);
		if (!tle)
			elog(ERROR, "bogus varattno for INNER_VAR var: %d", var->varattno);

		push_child_plan(dpns, dpns->inner_plan, &save_dpns);
		resolve_special_varno((Node *) tle->expr, context,
							  callback, callback_arg);
		pop_child_plan(dpns, &save_dpns);
		return;
	}
	else if (var->varno == INDEX_VAR && dpns->index_tlist)
	{
		TargetEntry *tle;

		tle = get_tle_by_resno(dpns->index_tlist, var->varattno);
		if (!tle)
			elog(ERROR, "bogus varattno for INDEX_VAR var: %d", var->varattno);

		resolve_special_varno((Node *) tle->expr, context,
							  callback, callback_arg);
		return;
	}
	else if (var->varno < 1 || var->varno > list_length(dpns->rtable))
		elog(ERROR, "bogus varno: %d", var->varno);

	/* Not special.  Just invoke the callback. */
	(*callback) (node, context, callback_arg);
}

/*
 * Get the name of a field of an expression of composite type.  The
 * expression is usually a Var, but we handle other cases too.
 *
 * levelsup is an extra offset to interpret the Var's varlevelsup correctly.
 *
 * This is fairly straightforward when the expression has a named composite
 * type; we need only look up the type in the catalogs.  However, the type
 * could also be RECORD.  Since no actual table or view column is allowed to
 * have type RECORD, a Var of type RECORD must refer to a JOIN or FUNCTION RTE
 * or to a subquery output.  We drill down to find the ultimate defining
 * expression and attempt to infer the field name from it.  We ereport if we
 * can't determine the name.
 *
 * Similarly, a PARAM of type RECORD has to refer to some expression of
 * a determinable composite type.
 */
static const char *
get_name_for_var_field(Var *var, int fieldno,
					   int levelsup, deparse_context *context)
{
	RangeTblEntry *rte;
	AttrNumber	attnum;
	int			netlevelsup;
	deparse_namespace *dpns;
	int			varno;
	AttrNumber	varattno;
	TupleDesc	tupleDesc;
	Node	   *expr;

	/*
	 * If it's a RowExpr that was expanded from a whole-row Var, use the
	 * column names attached to it.  (We could let get_expr_result_tupdesc()
	 * handle this, but it's much cheaper to just pull out the name we need.)
	 */
	if (IsA(var, RowExpr))
	{
		RowExpr    *r = (RowExpr *) var;

		if (fieldno > 0 && fieldno <= list_length(r->colnames))
			return strVal(list_nth(r->colnames, fieldno - 1));
	}

	/*
	 * If it's a Param of type RECORD, try to find what the Param refers to.
	 */
	if (IsA(var, Param))
	{
		Param	   *param = (Param *) var;
		ListCell   *ancestor_cell;

		expr = find_param_referent(param, context, &dpns, &ancestor_cell);
		if (expr)
		{
			/* Found a match, so recurse to decipher the field name */
			deparse_namespace save_dpns;
			const char *result;

			push_ancestor_plan(dpns, ancestor_cell, &save_dpns);
			result = get_name_for_var_field((Var *) expr, fieldno,
											0, context);
			pop_ancestor_plan(dpns, &save_dpns);
			return result;
		}
	}

	/*
	 * If it's a Var of type RECORD, we have to find what the Var refers to;
	 * if not, we can use get_expr_result_tupdesc().
	 */
	if (!IsA(var, Var) ||
		var->vartype != RECORDOID)
	{
		tupleDesc = get_expr_result_tupdesc((Node *) var, false);
		/* Got the tupdesc, so we can extract the field name */
		Assert(fieldno >= 1 && fieldno <= tupleDesc->natts);
		return NameStr(TupleDescAttr(tupleDesc, fieldno - 1)->attname);
	}

	/* Find appropriate nesting depth */
	netlevelsup = var->varlevelsup + levelsup;
	if (netlevelsup >= list_length(context->namespaces))
		elog(ERROR, "bogus varlevelsup: %d offset %d",
			 var->varlevelsup, levelsup);
	dpns = (deparse_namespace *) list_nth(context->namespaces,
										  netlevelsup);

	/*
	 * If we have a syntactic referent for the Var, and we're working from a
	 * parse tree, prefer to use the syntactic referent.  Otherwise, fall back
	 * on the semantic referent.  (See comments in get_variable().)
	 */
	if (var->varnosyn > 0 && dpns->plan == NULL)
	{
		varno = var->varnosyn;
		varattno = var->varattnosyn;
	}
	else
	{
		varno = var->varno;
		varattno = var->varattno;
	}

	/*
	 * Try to find the relevant RTE in this rtable.  In a plan tree, it's
	 * likely that varno is OUTER_VAR or INNER_VAR, in which case we must dig
	 * down into the subplans, or INDEX_VAR, which is resolved similarly.
	 *
	 * Note: unlike get_variable and resolve_special_varno, we need not worry
	 * about inheritance mapping: a child Var should have the same datatype as
	 * its parent, and here we're really only interested in the Var's type.
	 */
	if (varno >= 1 && varno <= list_length(dpns->rtable))
	{
		rte = rt_fetch(varno, dpns->rtable);
		attnum = varattno;
	}
	else if (varno == OUTER_VAR && dpns->outer_tlist)
	{
		TargetEntry *tle;
		deparse_namespace save_dpns;
		const char *result;

		tle = get_tle_by_resno(dpns->outer_tlist, varattno);
		if (!tle)
			elog(ERROR, "bogus varattno for OUTER_VAR var: %d", varattno);

		Assert(netlevelsup == 0);
		push_child_plan(dpns, dpns->outer_plan, &save_dpns);

		result = get_name_for_var_field((Var *) tle->expr, fieldno,
										levelsup, context);

		pop_child_plan(dpns, &save_dpns);
		return result;
	}
	else if (varno == INNER_VAR && dpns->inner_tlist)
	{
		TargetEntry *tle;
		deparse_namespace save_dpns;
		const char *result;

		tle = get_tle_by_resno(dpns->inner_tlist, varattno);
		if (!tle)
			elog(ERROR, "bogus varattno for INNER_VAR var: %d", varattno);

		Assert(netlevelsup == 0);
		push_child_plan(dpns, dpns->inner_plan, &save_dpns);

		result = get_name_for_var_field((Var *) tle->expr, fieldno,
										levelsup, context);

		pop_child_plan(dpns, &save_dpns);
		return result;
	}
	else if (varno == INDEX_VAR && dpns->index_tlist)
	{
		TargetEntry *tle;
		const char *result;

		tle = get_tle_by_resno(dpns->index_tlist, varattno);
		if (!tle)
			elog(ERROR, "bogus varattno for INDEX_VAR var: %d", varattno);

		Assert(netlevelsup == 0);

		result = get_name_for_var_field((Var *) tle->expr, fieldno,
										levelsup, context);

		return result;
	}
	else
	{
		elog(ERROR, "bogus varno: %d", varno);
		return NULL;			/* keep compiler quiet */
	}

	if (attnum == InvalidAttrNumber)
	{
		/* Var is whole-row reference to RTE, so select the right field */
		return get_rte_attribute_name(rte, fieldno);
	}

	/*
	 * This part has essentially the same logic as the parser's
	 * expandRecordVariable() function, but we are dealing with a different
	 * representation of the input context, and we only need one field name
	 * not a TupleDesc.  Also, we need special cases for finding subquery and
	 * CTE subplans when deparsing Plan trees.
	 */
	expr = (Node *) var;		/* default if we can't drill down */

	switch (rte->rtekind)
	{
		case RTE_RELATION:
		case RTE_VALUES:
		case RTE_NAMEDTUPLESTORE:
		case RTE_RESULT:

			/*
			 * This case should not occur: a column of a table, values list,
			 * or ENR shouldn't have type RECORD.  Fall through and fail (most
			 * likely) at the bottom.
			 */
			break;
		case RTE_SUBQUERY:
			/* Subselect-in-FROM: examine sub-select's output expr */
			{
				if (rte->subquery)
				{
					TargetEntry *ste = get_tle_by_resno(rte->subquery->targetList,
														attnum);

					if (ste == NULL || ste->resjunk)
						elog(ERROR, "subquery %s does not have attribute %d",
							 rte->eref->aliasname, attnum);
					expr = (Node *) ste->expr;
					if (IsA(expr, Var))
					{
						/*
						 * Recurse into the sub-select to see what its Var
						 * refers to. We have to build an additional level of
						 * namespace to keep in step with varlevelsup in the
						 * subselect; furthermore, the subquery RTE might be
						 * from an outer query level, in which case the
						 * namespace for the subselect must have that outer
						 * level as parent namespace.
						 */
						List	   *save_nslist = context->namespaces;
						List	   *parent_namespaces;
						deparse_namespace mydpns;
						const char *result;

						parent_namespaces = list_copy_tail(context->namespaces,
														   netlevelsup);

						set_deparse_for_query(&mydpns, rte->subquery,
											  parent_namespaces);

						context->namespaces = lcons(&mydpns, parent_namespaces);

						result = get_name_for_var_field((Var *) expr, fieldno,
														0, context);

						context->namespaces = save_nslist;

						return result;
					}
					/* else fall through to inspect the expression */
				}
				else
				{
					/*
					 * We're deparsing a Plan tree so we don't have complete
					 * RTE entries (in particular, rte->subquery is NULL). But
					 * the only place we'd normally see a Var directly
					 * referencing a SUBQUERY RTE is in a SubqueryScan plan
					 * node, and we can look into the child plan's tlist
					 * instead.  An exception occurs if the subquery was
					 * proven empty and optimized away: then we'd find such a
					 * Var in a childless Result node, and there's nothing in
					 * the plan tree that would let us figure out what it had
					 * originally referenced.  In that case, fall back on
					 * printing "fN", analogously to the default column names
					 * for RowExprs.
					 */
					TargetEntry *tle;
					deparse_namespace save_dpns;
					const char *result;

					if (!dpns->inner_plan)
					{
						char	   *dummy_name = palloc(32);

						Assert(dpns->plan && IsA(dpns->plan, Result));
						snprintf(dummy_name, 32, "f%d", fieldno);
						return dummy_name;
					}
					Assert(dpns->plan && IsA(dpns->plan, SubqueryScan));

					tle = get_tle_by_resno(dpns->inner_tlist, attnum);
					if (!tle)
						elog(ERROR, "bogus varattno for subquery var: %d",
							 attnum);
					Assert(netlevelsup == 0);
					push_child_plan(dpns, dpns->inner_plan, &save_dpns);

					result = get_name_for_var_field((Var *) tle->expr, fieldno,
													levelsup, context);

					pop_child_plan(dpns, &save_dpns);
					return result;
				}
			}
			break;
		case RTE_JOIN:
			/* Join RTE --- recursively inspect the alias variable */
			if (rte->joinaliasvars == NIL)
				elog(ERROR, "cannot decompile join alias var in plan tree");
			Assert(attnum > 0 && attnum <= list_length(rte->joinaliasvars));
			expr = (Node *) list_nth(rte->joinaliasvars, attnum - 1);
			Assert(expr != NULL);
			/* we intentionally don't strip implicit coercions here */
			if (IsA(expr, Var))
				return get_name_for_var_field((Var *) expr, fieldno,
											  var->varlevelsup + levelsup,
											  context);
			/* else fall through to inspect the expression */
			break;
		case RTE_FUNCTION:
		case RTE_TABLEFUNC:

			/*
			 * We couldn't get here unless a function is declared with one of
			 * its result columns as RECORD, which is not allowed.
			 */
			break;
		case RTE_CTE:
			/* CTE reference: examine subquery's output expr */
			{
				CommonTableExpr *cte = NULL;
				Index		ctelevelsup;
				ListCell   *lc;

				/*
				 * Try to find the referenced CTE using the namespace stack.
				 */
				ctelevelsup = rte->ctelevelsup + netlevelsup;
				if (ctelevelsup >= list_length(context->namespaces))
					lc = NULL;
				else
				{
					deparse_namespace *ctedpns;

					ctedpns = (deparse_namespace *)
						list_nth(context->namespaces, ctelevelsup);
					foreach(lc, ctedpns->ctes)
					{
						cte = (CommonTableExpr *) lfirst(lc);
						if (strcmp(cte->ctename, rte->ctename) == 0)
							break;
					}
				}
				if (lc != NULL)
				{
					Query	   *ctequery = (Query *) cte->ctequery;
					TargetEntry *ste = get_tle_by_resno(GetCTETargetList(cte),
														attnum);

					if (ste == NULL || ste->resjunk)
						elog(ERROR, "CTE %s does not have attribute %d",
							 rte->eref->aliasname, attnum);
					expr = (Node *) ste->expr;
					if (IsA(expr, Var))
					{
						/*
						 * Recurse into the CTE to see what its Var refers to.
						 * We have to build an additional level of namespace
						 * to keep in step with varlevelsup in the CTE;
						 * furthermore it could be an outer CTE (compare
						 * SUBQUERY case above).
						 */
						List	   *save_nslist = context->namespaces;
						List	   *parent_namespaces;
						deparse_namespace mydpns;
						const char *result;

						parent_namespaces = list_copy_tail(context->namespaces,
														   ctelevelsup);

						set_deparse_for_query(&mydpns, ctequery,
											  parent_namespaces);

						context->namespaces = lcons(&mydpns, parent_namespaces);

						result = get_name_for_var_field((Var *) expr, fieldno,
														0, context);

						context->namespaces = save_nslist;

						return result;
					}
					/* else fall through to inspect the expression */
				}
				else
				{
					/*
					 * We're deparsing a Plan tree so we don't have a CTE
					 * list.  But the only places we'd normally see a Var
					 * directly referencing a CTE RTE are in CteScan or
					 * WorkTableScan plan nodes.  For those cases,
					 * set_deparse_plan arranged for dpns->inner_plan to be
					 * the plan node that emits the CTE or RecursiveUnion
					 * result, and we can look at its tlist instead.  As
					 * above, this can fail if the CTE has been proven empty,
					 * in which case fall back to "fN".
					 */
					TargetEntry *tle;
					deparse_namespace save_dpns;
					const char *result;

					if (!dpns->inner_plan)
					{
						char	   *dummy_name = palloc(32);

						Assert(dpns->plan && IsA(dpns->plan, Result));
						snprintf(dummy_name, 32, "f%d", fieldno);
						return dummy_name;
					}
					Assert(dpns->plan && (IsA(dpns->plan, CteScan) ||
										  IsA(dpns->plan, WorkTableScan)));

					tle = get_tle_by_resno(dpns->inner_tlist, attnum);
					if (!tle)
						elog(ERROR, "bogus varattno for subquery var: %d",
							 attnum);
					Assert(netlevelsup == 0);
					push_child_plan(dpns, dpns->inner_plan, &save_dpns);

					result = get_name_for_var_field((Var *) tle->expr, fieldno,
													levelsup, context);

					pop_child_plan(dpns, &save_dpns);
					return result;
				}
			}
			break;
		case RTE_GROUP:

			/*
			 * We couldn't get here: any Vars that reference the RTE_GROUP RTE
			 * should have been replaced with the underlying grouping
			 * expressions.
			 */
			break;
	}

	/*
	 * We now have an expression we can't expand any more, so see if
	 * get_expr_result_tupdesc() can do anything with it.
	 */
	tupleDesc = get_expr_result_tupdesc(expr, false);
	/* Got the tupdesc, so we can extract the field name */
	Assert(fieldno >= 1 && fieldno <= tupleDesc->natts);
	return NameStr(TupleDescAttr(tupleDesc, fieldno - 1)->attname);
}

/*
 * Try to find the referenced expression for a PARAM_EXEC Param that might
 * reference a parameter supplied by an upper NestLoop or SubPlan plan node.
 *
 * If successful, return the expression and set *dpns_p and *ancestor_cell_p
 * appropriately for calling push_ancestor_plan().  If no referent can be
 * found, return NULL.
 */
static Node *
find_param_referent(Param *param, deparse_context *context,
					deparse_namespace **dpns_p, ListCell **ancestor_cell_p)
{
	/* Initialize output parameters to prevent compiler warnings */
	*dpns_p = NULL;
	*ancestor_cell_p = NULL;

	/*
	 * If it's a PARAM_EXEC parameter, look for a matching NestLoopParam or
	 * SubPlan argument.  This will necessarily be in some ancestor of the
	 * current expression's Plan node.
	 */
	if (param->paramkind == PARAM_EXEC)
	{
		deparse_namespace *dpns;
		Plan	   *child_plan;
		ListCell   *lc;

		dpns = (deparse_namespace *) linitial(context->namespaces);
		child_plan = dpns->plan;

		foreach(lc, dpns->ancestors)
		{
			Node	   *ancestor = (Node *) lfirst(lc);
			ListCell   *lc2;

			/*
			 * NestLoops transmit params to their inner child only.
			 */
			if (IsA(ancestor, NestLoop) &&
				child_plan == innerPlan(ancestor))
			{
				NestLoop   *nl = (NestLoop *) ancestor;

				foreach(lc2, nl->nestParams)
				{
					NestLoopParam *nlp = (NestLoopParam *) lfirst(lc2);

					if (nlp->paramno == param->paramid)
					{
						/* Found a match, so return it */
						*dpns_p = dpns;
						*ancestor_cell_p = lc;
						return (Node *) nlp->paramval;
					}
				}
			}

			/*
			 * If ancestor is a SubPlan, check the arguments it provides.
			 */
			if (IsA(ancestor, SubPlan))
			{
				SubPlan    *subplan = (SubPlan *) ancestor;
				ListCell   *lc3;
				ListCell   *lc4;

				forboth(lc3, subplan->parParam, lc4, subplan->args)
				{
					int			paramid = lfirst_int(lc3);
					Node	   *arg = (Node *) lfirst(lc4);

					if (paramid == param->paramid)
					{
						/*
						 * Found a match, so return it.  But, since Vars in
						 * the arg are to be evaluated in the surrounding
						 * context, we have to point to the next ancestor item
						 * that is *not* a SubPlan.
						 */
						ListCell   *rest;

						for_each_cell(rest, dpns->ancestors,
									  lnext(dpns->ancestors, lc))
						{
							Node	   *ancestor2 = (Node *) lfirst(rest);

							if (!IsA(ancestor2, SubPlan))
							{
								*dpns_p = dpns;
								*ancestor_cell_p = rest;
								return arg;
							}
						}
						elog(ERROR, "SubPlan cannot be outermost ancestor");
					}
				}

				/* SubPlan isn't a kind of Plan, so skip the rest */
				continue;
			}

			/*
			 * We need not consider the ancestor's initPlan list, since
			 * initplans never have any parParams.
			 */

			/* No luck, crawl up to next ancestor */
			child_plan = (Plan *) ancestor;
		}
	}

	/* No referent found */
	return NULL;
}

/*
 * Try to find a subplan/initplan that emits the value for a PARAM_EXEC Param.
 *
 * If successful, return the generating subplan/initplan and set *column_p
 * to the subplan's 0-based output column number.
 * Otherwise, return NULL.
 */
static SubPlan *
find_param_generator(Param *param, deparse_context *context, int *column_p)
{
	/* Initialize output parameter to prevent compiler warnings */
	*column_p = 0;

	/*
	 * If it's a PARAM_EXEC parameter, search the current plan node as well as
	 * ancestor nodes looking for a subplan or initplan that emits the value
	 * for the Param.  It could appear in the setParams of an initplan or
	 * MULTIEXPR_SUBLINK subplan, or in the paramIds of an ancestral SubPlan.
	 */
	if (param->paramkind == PARAM_EXEC)
	{
		SubPlan    *result;
		deparse_namespace *dpns;
		ListCell   *lc;

		dpns = (deparse_namespace *) linitial(context->namespaces);

		/* First check the innermost plan node's initplans */
		result = find_param_generator_initplan(param, dpns->plan, column_p);
		if (result)
			return result;

		/*
		 * The plan's targetlist might contain MULTIEXPR_SUBLINK SubPlans,
		 * which can be referenced by Params elsewhere in the targetlist.
		 * (Such Params should always be in the same targetlist, so there's no
		 * need to do this work at upper plan nodes.)
		 */
		foreach_node(TargetEntry, tle, dpns->plan->targetlist)
		{
			if (tle->expr && IsA(tle->expr, SubPlan))
			{
				SubPlan    *subplan = (SubPlan *) tle->expr;

				if (subplan->subLinkType == MULTIEXPR_SUBLINK)
				{
					foreach_int(paramid, subplan->setParam)
					{
						if (paramid == param->paramid)
						{
							/* Found a match, so return it. */
							*column_p = foreach_current_index(paramid);
							return subplan;
						}
					}
				}
			}
		}

		/* No luck, so check the ancestor nodes */
		foreach(lc, dpns->ancestors)
		{
			Node	   *ancestor = (Node *) lfirst(lc);

			/*
			 * If ancestor is a SubPlan, check the paramIds it provides.
			 */
			if (IsA(ancestor, SubPlan))
			{
				SubPlan    *subplan = (SubPlan *) ancestor;

				foreach_int(paramid, subplan->paramIds)
				{
					if (paramid == param->paramid)
					{
						/* Found a match, so return it. */
						*column_p = foreach_current_index(paramid);
						return subplan;
					}
				}

				/* SubPlan isn't a kind of Plan, so skip the rest */
				continue;
			}

			/*
			 * Otherwise, it's some kind of Plan node, so check its initplans.
			 */
			result = find_param_generator_initplan(param, (Plan *) ancestor,
												   column_p);
			if (result)
				return result;

			/* No luck, crawl up to next ancestor */
		}
	}

	/* No generator found */
	return NULL;
}

/*
 * Subroutine for find_param_generator: search one Plan node's initplans
 */
static SubPlan *
find_param_generator_initplan(Param *param, Plan *plan, int *column_p)
{
	foreach_node(SubPlan, subplan, plan->initPlan)
	{
		foreach_int(paramid, subplan->setParam)
		{
			if (paramid == param->paramid)
			{
				/* Found a match, so return it. */
				*column_p = foreach_current_index(paramid);
				return subplan;
			}
		}
	}
	return NULL;
}

/*
 * Display a Param appropriately.
 */
static void
get_parameter(Param *param, deparse_context *context)
{
	Node	   *expr;
	deparse_namespace *dpns;
	ListCell   *ancestor_cell;
	SubPlan    *subplan;
	int			column;

	/*
	 * If it's a PARAM_EXEC parameter, try to locate the expression from which
	 * the parameter was computed.  This stanza handles only cases in which
	 * the Param represents an input to the subplan we are currently in.
	 */
	expr = find_param_referent(param, context, &dpns, &ancestor_cell);
	if (expr)
	{
		/* Found a match, so print it */
		deparse_namespace save_dpns;
		bool		save_varprefix;
		bool		need_paren;

		/* Switch attention to the ancestor plan node */
		push_ancestor_plan(dpns, ancestor_cell, &save_dpns);

		/*
		 * Force prefixing of Vars, since they won't belong to the relation
		 * being scanned in the original plan node.
		 */
		save_varprefix = context->varprefix;
		context->varprefix = true;

		/*
		 * A Param's expansion is typically a Var, Aggref, GroupingFunc, or
		 * upper-level Param, which wouldn't need extra parentheses.
		 * Otherwise, insert parens to ensure the expression looks atomic.
		 */
		need_paren = !(IsA(expr, Var) ||
					   IsA(expr, Aggref) ||
					   IsA(expr, GroupingFunc) ||
					   IsA(expr, Param));
		if (need_paren)
			appendStringInfoChar(context->buf, '(');

		get_rule_expr(expr, context, false);

		if (need_paren)
			appendStringInfoChar(context->buf, ')');

		context->varprefix = save_varprefix;

		pop_ancestor_plan(dpns, &save_dpns);

		return;
	}

	/*
	 * Alternatively, maybe it's a subplan output, which we print as a
	 * reference to the subplan.  (We could drill down into the subplan and
	 * print the relevant targetlist expression, but that has been deemed too
	 * confusing since it would violate normal SQL scope rules.  Also, we're
	 * relying on this reference to show that the testexpr containing the
	 * Param has anything to do with that subplan at all.)
	 */
	subplan = find_param_generator(param, context, &column);
	if (subplan)
	{
		appendStringInfo(context->buf, "(%s%s).col%d",
						 subplan->useHashTable ? "hashed " : "",
						 subplan->plan_name, column + 1);

		return;
	}

	/*
	 * If it's an external parameter, see if the outermost namespace provides
	 * function argument names.
	 */
	if (param->paramkind == PARAM_EXTERN && context->namespaces != NIL)
	{
		dpns = llast(context->namespaces);
		if (dpns->argnames &&
			param->paramid > 0 &&
			param->paramid <= dpns->numargs)
		{
			char	   *argname = dpns->argnames[param->paramid - 1];

			if (argname)
			{
				bool		should_qualify = false;
				ListCell   *lc;

				/*
				 * Qualify the parameter name if there are any other deparse
				 * namespaces with range tables.  This avoids qualifying in
				 * trivial cases like "RETURN a + b", but makes it safe in all
				 * other cases.
				 */
				foreach(lc, context->namespaces)
				{
					deparse_namespace *depns = lfirst(lc);

					if (depns->rtable_names != NIL)
					{
						should_qualify = true;
						break;
					}
				}
				if (should_qualify)
				{
					appendStringInfoString(context->buf, quote_identifier(dpns->funcname));
					appendStringInfoChar(context->buf, '.');
				}

				appendStringInfoString(context->buf, quote_identifier(argname));
				return;
			}
		}
	}

	/*
	 * Not PARAM_EXEC, or couldn't find referent: just print $N.
	 *
	 * It's a bug if we get here for anything except PARAM_EXTERN Params, but
	 * in production builds printing $N seems more useful than failing.
	 */
	Assert(param->paramkind == PARAM_EXTERN);

	appendStringInfo(context->buf, "$%d", param->paramid);
}

/*
 * get_simple_binary_op_name
 *
 * helper function for isSimpleNode
 * will return single char binary operator name, or NULL if it's not
 */
static const char *
get_simple_binary_op_name(OpExpr *expr)
{
	List	   *args = expr->args;

	if (list_length(args) == 2)
	{
		/* binary operator */
		Node	   *arg1 = (Node *) linitial(args);
		Node	   *arg2 = (Node *) lsecond(args);
		const char *op;

		op = generate_operator_name(expr->opno, exprType(arg1), exprType(arg2));
		if (strlen(op) == 1)
			return op;
	}
	return NULL;
}


/*
 * isSimpleNode - check if given node is simple (doesn't need parenthesizing)
 *
 *	true   : simple in the context of parent node's type
 *	false  : not simple
 */
static bool
isSimpleNode(Node *node, Node *parentNode, int prettyFlags)
{
	if (!node)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_CoerceToDomainValue:
		case T_SetToDefault:
		case T_CurrentOfExpr:
			/* single words: always simple */
			return true;

		case T_SubscriptingRef:
		case T_ArrayExpr:
		case T_RowExpr:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
		case T_SQLValueFunction:
		case T_XmlExpr:
		case T_NextValueExpr:
		case T_NullIfExpr:
		case T_Aggref:
		case T_GroupingFunc:
		case T_WindowFunc:
		case T_MergeSupportFunc:
		case T_FuncExpr:
		case T_JsonConstructorExpr:
		case T_JsonExpr:
			/* function-like: name(..) or name[..] */
			return true;

			/* CASE keywords act as parentheses */
		case T_CaseExpr:
			return true;

		case T_FieldSelect:

			/*
			 * appears simple since . has top precedence, unless parent is
			 * T_FieldSelect itself!
			 */
			return !IsA(parentNode, FieldSelect);

		case T_FieldStore:

			/*
			 * treat like FieldSelect (probably doesn't matter)
			 */
			return !IsA(parentNode, FieldStore);

		case T_CoerceToDomain:
			/* maybe simple, check args */
			return isSimpleNode((Node *) ((CoerceToDomain *) node)->arg,
								node, prettyFlags);
		case T_RelabelType:
			return isSimpleNode((Node *) ((RelabelType *) node)->arg,
								node, prettyFlags);
		case T_CoerceViaIO:
			return isSimpleNode((Node *) ((CoerceViaIO *) node)->arg,
								node, prettyFlags);
		case T_ArrayCoerceExpr:
			return isSimpleNode((Node *) ((ArrayCoerceExpr *) node)->arg,
								node, prettyFlags);
		case T_ConvertRowtypeExpr:
			return isSimpleNode((Node *) ((ConvertRowtypeExpr *) node)->arg,
								node, prettyFlags);
		case T_ReturningExpr:
			return isSimpleNode((Node *) ((ReturningExpr *) node)->retexpr,
								node, prettyFlags);

		case T_OpExpr:
			{
				/* depends on parent node type; needs further checking */
				if (prettyFlags & PRETTYFLAG_PAREN && IsA(parentNode, OpExpr))
				{
					const char *op;
					const char *parentOp;
					bool		is_lopriop;
					bool		is_hipriop;
					bool		is_lopriparent;
					bool		is_hipriparent;

					op = get_simple_binary_op_name((OpExpr *) node);
					if (!op)
						return false;

					/* We know only the basic operators + - and * / % */
					is_lopriop = (strchr("+-", *op) != NULL);
					is_hipriop = (strchr("*/%", *op) != NULL);
					if (!(is_lopriop || is_hipriop))
						return false;

					parentOp = get_simple_binary_op_name((OpExpr *) parentNode);
					if (!parentOp)
						return false;

					is_lopriparent = (strchr("+-", *parentOp) != NULL);
					is_hipriparent = (strchr("*/%", *parentOp) != NULL);
					if (!(is_lopriparent || is_hipriparent))
						return false;

					if (is_hipriop && is_lopriparent)
						return true;	/* op binds tighter than parent */

					if (is_lopriop && is_hipriparent)
						return false;

					/*
					 * Operators are same priority --- can skip parens only if
					 * we have (a - b) - c, not a - (b - c).
					 */
					if (node == (Node *) linitial(((OpExpr *) parentNode)->args))
						return true;

					return false;
				}
				/* else do the same stuff as for T_SubLink et al. */
			}
			/* FALLTHROUGH */

		case T_SubLink:
		case T_NullTest:
		case T_BooleanTest:
		case T_DistinctExpr:
		case T_JsonIsPredicate:
			switch (nodeTag(parentNode))
			{
				case T_FuncExpr:
					{
						/* special handling for casts and COERCE_SQL_SYNTAX */
						CoercionForm type = ((FuncExpr *) parentNode)->funcformat;

						if (type == COERCE_EXPLICIT_CAST ||
							type == COERCE_IMPLICIT_CAST ||
							type == COERCE_SQL_SYNTAX)
							return false;
						return true;	/* own parentheses */
					}
				case T_BoolExpr:	/* lower precedence */
				case T_SubscriptingRef: /* other separators */
				case T_ArrayExpr:	/* other separators */
				case T_RowExpr: /* other separators */
				case T_CoalesceExpr:	/* own parentheses */
				case T_MinMaxExpr:	/* own parentheses */
				case T_XmlExpr: /* own parentheses */
				case T_NullIfExpr:	/* other separators */
				case T_Aggref:	/* own parentheses */
				case T_GroupingFunc:	/* own parentheses */
				case T_WindowFunc:	/* own parentheses */
				case T_CaseExpr:	/* other separators */
					return true;
				default:
					return false;
			}

		case T_BoolExpr:
			switch (nodeTag(parentNode))
			{
				case T_BoolExpr:
					if (prettyFlags & PRETTYFLAG_PAREN)
					{
						BoolExprType type;
						BoolExprType parentType;

						type = ((BoolExpr *) node)->boolop;
						parentType = ((BoolExpr *) parentNode)->boolop;
						switch (type)
						{
							case NOT_EXPR:
							case AND_EXPR:
								if (parentType == AND_EXPR || parentType == OR_EXPR)
									return true;
								break;
							case OR_EXPR:
								if (parentType == OR_EXPR)
									return true;
								break;
						}
					}
					return false;
				case T_FuncExpr:
					{
						/* special handling for casts and COERCE_SQL_SYNTAX */
						CoercionForm type = ((FuncExpr *) parentNode)->funcformat;

						if (type == COERCE_EXPLICIT_CAST ||
							type == COERCE_IMPLICIT_CAST ||
							type == COERCE_SQL_SYNTAX)
							return false;
						return true;	/* own parentheses */
					}
				case T_SubscriptingRef: /* other separators */
				case T_ArrayExpr:	/* other separators */
				case T_RowExpr: /* other separators */
				case T_CoalesceExpr:	/* own parentheses */
				case T_MinMaxExpr:	/* own parentheses */
				case T_XmlExpr: /* own parentheses */
				case T_NullIfExpr:	/* other separators */
				case T_Aggref:	/* own parentheses */
				case T_GroupingFunc:	/* own parentheses */
				case T_WindowFunc:	/* own parentheses */
				case T_CaseExpr:	/* other separators */
				case T_JsonExpr:	/* own parentheses */
					return true;
				default:
					return false;
			}

		case T_JsonValueExpr:
			/* maybe simple, check args */
			return isSimpleNode((Node *) ((JsonValueExpr *) node)->raw_expr,
								node, prettyFlags);

		default:
			break;
	}
	/* those we don't know: in dubio complexo */
	return false;
}


/*
 * appendContextKeyword - append a keyword to buffer
 *
 * If prettyPrint is enabled, perform a line break, and adjust indentation.
 * Otherwise, just append the keyword.
 */
static void
appendContextKeyword(deparse_context *context, const char *str,
					 int indentBefore, int indentAfter, int indentPlus)
{
	StringInfo	buf = context->buf;

	if (PRETTY_INDENT(context))
	{
		int			indentAmount;

		context->indentLevel += indentBefore;

		/* remove any trailing spaces currently in the buffer ... */
		removeStringInfoSpaces(buf);
		/* ... then add a newline and some spaces */
		appendStringInfoChar(buf, '\n');

		if (context->indentLevel < PRETTYINDENT_LIMIT)
			indentAmount = Max(context->indentLevel, 0) + indentPlus;
		else
		{
			/*
			 * If we're indented more than PRETTYINDENT_LIMIT characters, try
			 * to conserve horizontal space by reducing the per-level
			 * indentation.  For best results the scale factor here should
			 * divide all the indent amounts that get added to indentLevel
			 * (PRETTYINDENT_STD, etc).  It's important that the indentation
			 * not grow unboundedly, else deeply-nested trees use O(N^2)
			 * whitespace; so we also wrap modulo PRETTYINDENT_LIMIT.
			 */
			indentAmount = PRETTYINDENT_LIMIT +
				(context->indentLevel - PRETTYINDENT_LIMIT) /
				(PRETTYINDENT_STD / 2);
			indentAmount %= PRETTYINDENT_LIMIT;
			/* scale/wrap logic affects indentLevel, but not indentPlus */
			indentAmount += indentPlus;
		}
		appendStringInfoSpaces(buf, indentAmount);

		appendStringInfoString(buf, str);

		context->indentLevel += indentAfter;
		if (context->indentLevel < 0)
			context->indentLevel = 0;
	}
	else
		appendStringInfoString(buf, str);
}

/*
 * removeStringInfoSpaces - delete trailing spaces from a buffer.
 *
 * Possibly this should move to stringinfo.c at some point.
 */
static void
removeStringInfoSpaces(StringInfo str)
{
	while (str->len > 0 && str->data[str->len - 1] == ' ')
		str->data[--(str->len)] = '\0';
}


/*
 * get_rule_expr_paren	- deparse expr using get_rule_expr,
 * embracing the string with parentheses if necessary for prettyPrint.
 *
 * Never embrace if prettyFlags=0, because it's done in the calling node.
 *
 * Any node that does *not* embrace its argument node by sql syntax (with
 * parentheses, non-operator keywords like CASE/WHEN/ON, or comma etc) should
 * use get_rule_expr_paren instead of get_rule_expr so parentheses can be
 * added.
 */
static void
get_rule_expr_paren(Node *node, deparse_context *context,
					bool showimplicit, Node *parentNode)
{
	bool		need_paren;

	need_paren = PRETTY_PAREN(context) &&
		!isSimpleNode(node, parentNode, context->prettyFlags);

	if (need_paren)
		appendStringInfoChar(context->buf, '(');

	get_rule_expr(node, context, showimplicit);

	if (need_paren)
		appendStringInfoChar(context->buf, ')');
}

static void
get_json_behavior(JsonBehavior *behavior, deparse_context *context,
				  const char *on)
{
	/*
	 * The order of array elements must correspond to the order of
	 * JsonBehaviorType members.
	 */
	const char *behavior_names[] =
	{
		" NULL",
		" ERROR",
		" EMPTY",
		" TRUE",
		" FALSE",
		" UNKNOWN",
		" EMPTY ARRAY",
		" EMPTY OBJECT",
		" DEFAULT "
	};

	if ((int) behavior->btype < 0 || behavior->btype >= lengthof(behavior_names))
		elog(ERROR, "invalid json behavior type: %d", behavior->btype);

	appendStringInfoString(context->buf, behavior_names[behavior->btype]);

	if (behavior->btype == JSON_BEHAVIOR_DEFAULT)
		get_rule_expr(behavior->expr, context, false);

	appendStringInfo(context->buf, " ON %s", on);
}

/*
 * get_json_expr_options
 *
 * Parse back common options for JSON_QUERY, JSON_VALUE, JSON_EXISTS and
 * JSON_TABLE columns.
 */
static void
get_json_expr_options(JsonExpr *jsexpr, deparse_context *context,
					  JsonBehaviorType default_behavior)
{
	if (jsexpr->op == JSON_QUERY_OP)
	{
		if (jsexpr->wrapper == JSW_CONDITIONAL)
			appendStringInfoString(context->buf, " WITH CONDITIONAL WRAPPER");
		else if (jsexpr->wrapper == JSW_UNCONDITIONAL)
			appendStringInfoString(context->buf, " WITH UNCONDITIONAL WRAPPER");
		/* The default */
		else if (jsexpr->wrapper == JSW_NONE || jsexpr->wrapper == JSW_UNSPEC)
			appendStringInfoString(context->buf, " WITHOUT WRAPPER");

		if (jsexpr->omit_quotes)
			appendStringInfoString(context->buf, " OMIT QUOTES");
		/* The default */
		else
			appendStringInfoString(context->buf, " KEEP QUOTES");
	}

	if (jsexpr->on_empty && jsexpr->on_empty->btype != default_behavior)
		get_json_behavior(jsexpr->on_empty, context, "EMPTY");

	if (jsexpr->on_error && jsexpr->on_error->btype != default_behavior)
		get_json_behavior(jsexpr->on_error, context, "ERROR");
}

/* ----------
 * get_rule_expr			- Parse back an expression
 *
 * Note: showimplicit determines whether we display any implicit cast that
 * is present at the top of the expression tree.  It is a passed argument,
 * not a field of the context struct, because we change the value as we
 * recurse down into the expression.  In general we suppress implicit casts
 * when the result type is known with certainty (eg, the arguments of an
 * OR must be boolean).  We display implicit casts for arguments of functions
 * and operators, since this is needed to be certain that the same function
 * or operator will be chosen when the expression is re-parsed.
 * ----------
 */
static void
get_rule_expr(Node *node, deparse_context *context,
			  bool showimplicit)
{
	StringInfo	buf = context->buf;

	if (node == NULL)
		return;

	/* Guard against excessively long or deeply-nested queries */
	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	/*
	 * Each level of get_rule_expr must emit an indivisible term
	 * (parenthesized if necessary) to ensure result is reparsed into the same
	 * expression tree.  The only exception is that when the input is a List,
	 * we emit the component items comma-separated with no surrounding
	 * decoration; this is convenient for most callers.
	 */
	switch (nodeTag(node))
	{
		case T_Var:
			(void) get_variable((Var *) node, 0, false, context);
			break;

		case T_Const:
			get_const_expr((Const *) node, context, 0);
			break;

		case T_Param:
			get_parameter((Param *) node, context);
			break;

		case T_Aggref:
			get_agg_expr((Aggref *) node, context, (Aggref *) node);
			break;

		case T_GroupingFunc:
			{
				GroupingFunc *gexpr = (GroupingFunc *) node;

				appendStringInfoString(buf, "GROUPING(");
				get_rule_expr((Node *) gexpr->args, context, true);
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_WindowFunc:
			get_windowfunc_expr((WindowFunc *) node, context);
			break;

		case T_MergeSupportFunc:
			appendStringInfoString(buf, "MERGE_ACTION()");
			break;

		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;
				bool		need_parens;

				/*
				 * If the argument is a CaseTestExpr, we must be inside a
				 * FieldStore, ie, we are assigning to an element of an array
				 * within a composite column.  Since we already punted on
				 * displaying the FieldStore's target information, just punt
				 * here too, and display only the assignment source
				 * expression.
				 */
				if (IsA(sbsref->refexpr, CaseTestExpr))
				{
					Assert(sbsref->refassgnexpr);
					get_rule_expr((Node *) sbsref->refassgnexpr,
								  context, showimplicit);
					break;
				}

				/*
				 * Parenthesize the argument unless it's a simple Var or a
				 * FieldSelect.  (In particular, if it's another
				 * SubscriptingRef, we *must* parenthesize to avoid
				 * confusion.)
				 */
				need_parens = !IsA(sbsref->refexpr, Var) &&
					!IsA(sbsref->refexpr, FieldSelect);
				if (need_parens)
					appendStringInfoChar(buf, '(');
				get_rule_expr((Node *) sbsref->refexpr, context, showimplicit);
				if (need_parens)
					appendStringInfoChar(buf, ')');

				/*
				 * If there's a refassgnexpr, we want to print the node in the
				 * format "container[subscripts] := refassgnexpr".  This is
				 * not legal SQL, so decompilation of INSERT or UPDATE
				 * statements should always use processIndirection as part of
				 * the statement-level syntax.  We should only see this when
				 * EXPLAIN tries to print the targetlist of a plan resulting
				 * from such a statement.
				 */
				if (sbsref->refassgnexpr)
				{
					Node	   *refassgnexpr;

					/*
					 * Use processIndirection to print this node's subscripts
					 * as well as any additional field selections or
					 * subscripting in immediate descendants.  It returns the
					 * RHS expr that is actually being "assigned".
					 */
					refassgnexpr = processIndirection(node, context);
					appendStringInfoString(buf, " := ");
					get_rule_expr(refassgnexpr, context, showimplicit);
				}
				else
				{
					/* Just an ordinary container fetch, so print subscripts */
					printSubscripts(sbsref, context);
				}
			}
			break;

		case T_FuncExpr:
			get_func_expr((FuncExpr *) node, context, showimplicit);
			break;

		case T_NamedArgExpr:
			{
				NamedArgExpr *na = (NamedArgExpr *) node;

				appendStringInfo(buf, "%s => ", quote_identifier(na->name));
				get_rule_expr((Node *) na->arg, context, showimplicit);
			}
			break;

		case T_OpExpr:
			get_oper_expr((OpExpr *) node, context);
			break;

		case T_DistinctExpr:
			{
				DistinctExpr *expr = (DistinctExpr *) node;
				List	   *args = expr->args;
				Node	   *arg1 = (Node *) linitial(args);
				Node	   *arg2 = (Node *) lsecond(args);

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren(arg1, context, true, node);
				appendStringInfoString(buf, " IS DISTINCT FROM ");
				get_rule_expr_paren(arg2, context, true, node);
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_NullIfExpr:
			{
				NullIfExpr *nullifexpr = (NullIfExpr *) node;

				appendStringInfoString(buf, "NULLIF(");
				get_rule_expr((Node *) nullifexpr->args, context, true);
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;
				List	   *args = expr->args;
				Node	   *arg1 = (Node *) linitial(args);
				Node	   *arg2 = (Node *) lsecond(args);

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren(arg1, context, true, node);
				appendStringInfo(buf, " %s %s (",
								 generate_operator_name(expr->opno,
														exprType(arg1),
														get_base_element_type(exprType(arg2))),
								 expr->useOr ? "ANY" : "ALL");
				get_rule_expr_paren(arg2, context, true, node);

				/*
				 * There's inherent ambiguity in "x op ANY/ALL (y)" when y is
				 * a bare sub-SELECT.  Since we're here, the sub-SELECT must
				 * be meant as a scalar sub-SELECT yielding an array value to
				 * be used in ScalarArrayOpExpr; but the grammar will
				 * preferentially interpret such a construct as an ANY/ALL
				 * SubLink.  To prevent misparsing the output that way, insert
				 * a dummy coercion (which will be stripped by parse analysis,
				 * so no inefficiency is added in dump and reload).  This is
				 * indeed most likely what the user wrote to get the construct
				 * accepted in the first place.
				 */
				if (IsA(arg2, SubLink) &&
					((SubLink *) arg2)->subLinkType == EXPR_SUBLINK)
					appendStringInfo(buf, "::%s",
									 format_type_with_typemod(exprType(arg2),
															  exprTypmod(arg2)));
				appendStringInfoChar(buf, ')');
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;
				Node	   *first_arg = linitial(expr->args);
				ListCell   *arg;

				switch (expr->boolop)
				{
					case AND_EXPR:
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, '(');
						get_rule_expr_paren(first_arg, context,
											false, node);
						for_each_from(arg, expr->args, 1)
						{
							appendStringInfoString(buf, " AND ");
							get_rule_expr_paren((Node *) lfirst(arg), context,
												false, node);
						}
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, ')');
						break;

					case OR_EXPR:
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, '(');
						get_rule_expr_paren(first_arg, context,
											false, node);
						for_each_from(arg, expr->args, 1)
						{
							appendStringInfoString(buf, " OR ");
							get_rule_expr_paren((Node *) lfirst(arg), context,
												false, node);
						}
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, ')');
						break;

					case NOT_EXPR:
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, '(');
						appendStringInfoString(buf, "NOT ");
						get_rule_expr_paren(first_arg, context,
											false, node);
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, ')');
						break;

					default:
						elog(ERROR, "unrecognized boolop: %d",
							 (int) expr->boolop);
				}
			}
			break;

		case T_SubLink:
			get_sublink_expr((SubLink *) node, context);
			break;

		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;

				/*
				 * We cannot see an already-planned subplan in rule deparsing,
				 * only while EXPLAINing a query plan.  We don't try to
				 * reconstruct the original SQL, just reference the subplan
				 * that appears elsewhere in EXPLAIN's result.  It does seem
				 * useful to show the subLinkType and testexpr (if any), and
				 * we also note whether the subplan will be hashed.
				 */
				switch (subplan->subLinkType)
				{
					case EXISTS_SUBLINK:
						appendStringInfoString(buf, "EXISTS(");
						Assert(subplan->testexpr == NULL);
						break;
					case ALL_SUBLINK:
						appendStringInfoString(buf, "(ALL ");
						Assert(subplan->testexpr != NULL);
						break;
					case ANY_SUBLINK:
						appendStringInfoString(buf, "(ANY ");
						Assert(subplan->testexpr != NULL);
						break;
					case ROWCOMPARE_SUBLINK:
						/* Parenthesizing the testexpr seems sufficient */
						appendStringInfoChar(buf, '(');
						Assert(subplan->testexpr != NULL);
						break;
					case EXPR_SUBLINK:
						/* No need to decorate these subplan references */
						appendStringInfoChar(buf, '(');
						Assert(subplan->testexpr == NULL);
						break;
					case MULTIEXPR_SUBLINK:
						/* MULTIEXPR isn't executed in the normal way */
						appendStringInfoString(buf, "(rescan ");
						Assert(subplan->testexpr == NULL);
						break;
					case ARRAY_SUBLINK:
						appendStringInfoString(buf, "ARRAY(");
						Assert(subplan->testexpr == NULL);
						break;
					case CTE_SUBLINK:
						/* This case is unreachable within expressions */
						appendStringInfoString(buf, "CTE(");
						Assert(subplan->testexpr == NULL);
						break;
				}

				if (subplan->testexpr != NULL)
				{
					deparse_namespace *dpns;

					/*
					 * Push SubPlan into ancestors list while deparsing
					 * testexpr, so that we can handle PARAM_EXEC references
					 * to the SubPlan's paramIds.  (This makes it look like
					 * the SubPlan is an "ancestor" of the current plan node,
					 * which is a little weird, but it does no harm.)  In this
					 * path, we don't need to mention the SubPlan explicitly,
					 * because the referencing Params will show its existence.
					 */
					dpns = (deparse_namespace *) linitial(context->namespaces);
					dpns->ancestors = lcons(subplan, dpns->ancestors);

					get_rule_expr(subplan->testexpr, context, showimplicit);
					appendStringInfoChar(buf, ')');

					dpns->ancestors = list_delete_first(dpns->ancestors);
				}
				else
				{
					/* No referencing Params, so show the SubPlan's name */
					if (subplan->useHashTable)
						appendStringInfo(buf, "hashed %s)", subplan->plan_name);
					else
						appendStringInfo(buf, "%s)", subplan->plan_name);
				}
			}
			break;

		case T_AlternativeSubPlan:
			{
				AlternativeSubPlan *asplan = (AlternativeSubPlan *) node;
				ListCell   *lc;

				/*
				 * This case cannot be reached in normal usage, since no
				 * AlternativeSubPlan can appear either in parsetrees or
				 * finished plan trees.  We keep it just in case somebody
				 * wants to use this code to print planner data structures.
				 */
				appendStringInfoString(buf, "(alternatives: ");
				foreach(lc, asplan->subplans)
				{
					SubPlan    *splan = lfirst_node(SubPlan, lc);

					if (splan->useHashTable)
						appendStringInfo(buf, "hashed %s", splan->plan_name);
					else
						appendStringInfoString(buf, splan->plan_name);
					if (lnext(asplan->subplans, lc))
						appendStringInfoString(buf, " or ");
				}
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_FieldSelect:
			{
				FieldSelect *fselect = (FieldSelect *) node;
				Node	   *arg = (Node *) fselect->arg;
				int			fno = fselect->fieldnum;
				const char *fieldname;
				bool		need_parens;

				/*
				 * Parenthesize the argument unless it's an SubscriptingRef or
				 * another FieldSelect.  Note in particular that it would be
				 * WRONG to not parenthesize a Var argument; simplicity is not
				 * the issue here, having the right number of names is.
				 */
				need_parens = !IsA(arg, SubscriptingRef) &&
					!IsA(arg, FieldSelect);
				if (need_parens)
					appendStringInfoChar(buf, '(');
				get_rule_expr(arg, context, true);
				if (need_parens)
					appendStringInfoChar(buf, ')');

				/*
				 * Get and print the field name.
				 */
				fieldname = get_name_for_var_field((Var *) arg, fno,
												   0, context);
				appendStringInfo(buf, ".%s", quote_identifier(fieldname));
			}
			break;

		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;
				bool		need_parens;

				/*
				 * There is no good way to represent a FieldStore as real SQL,
				 * so decompilation of INSERT or UPDATE statements should
				 * always use processIndirection as part of the
				 * statement-level syntax.  We should only get here when
				 * EXPLAIN tries to print the targetlist of a plan resulting
				 * from such a statement.  The plan case is even harder than
				 * ordinary rules would be, because the planner tries to
				 * collapse multiple assignments to the same field or subfield
				 * into one FieldStore; so we can see a list of target fields
				 * not just one, and the arguments could be FieldStores
				 * themselves.  We don't bother to try to print the target
				 * field names; we just print the source arguments, with a
				 * ROW() around them if there's more than one.  This isn't
				 * terribly complete, but it's probably good enough for
				 * EXPLAIN's purposes; especially since anything more would be
				 * either hopelessly confusing or an even poorer
				 * representation of what the plan is actually doing.
				 */
				need_parens = (list_length(fstore->newvals) != 1);
				if (need_parens)
					appendStringInfoString(buf, "ROW(");
				get_rule_expr((Node *) fstore->newvals, context, showimplicit);
				if (need_parens)
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;
				Node	   *arg = (Node *) relabel->arg;

				if (relabel->relabelformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr_paren(arg, context, false, node);
				}
				else
				{
					get_coercion_expr(arg, context,
									  relabel->resulttype,
									  relabel->resulttypmod,
									  node);
				}
			}
			break;

		case T_CoerceViaIO:
			{
				CoerceViaIO *iocoerce = (CoerceViaIO *) node;
				Node	   *arg = (Node *) iocoerce->arg;

				if (iocoerce->coerceformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr_paren(arg, context, false, node);
				}
				else
				{
					get_coercion_expr(arg, context,
									  iocoerce->resulttype,
									  -1,
									  node);
				}
			}
			break;

		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;
				Node	   *arg = (Node *) acoerce->arg;

				if (acoerce->coerceformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr_paren(arg, context, false, node);
				}
				else
				{
					get_coercion_expr(arg, context,
									  acoerce->resulttype,
									  acoerce->resulttypmod,
									  node);
				}
			}
			break;

		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *convert = (ConvertRowtypeExpr *) node;
				Node	   *arg = (Node *) convert->arg;

				if (convert->convertformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr_paren(arg, context, false, node);
				}
				else
				{
					get_coercion_expr(arg, context,
									  convert->resulttype, -1,
									  node);
				}
			}
			break;

		case T_CollateExpr:
			{
				CollateExpr *collate = (CollateExpr *) node;
				Node	   *arg = (Node *) collate->arg;

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren(arg, context, showimplicit, node);
				appendStringInfo(buf, " COLLATE %s",
								 generate_collation_name(collate->collOid));
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;
				ListCell   *temp;

				appendContextKeyword(context, "CASE",
									 0, PRETTYINDENT_VAR, 0);
				if (caseexpr->arg)
				{
					appendStringInfoChar(buf, ' ');
					get_rule_expr((Node *) caseexpr->arg, context, true);
				}
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(temp);
					Node	   *w = (Node *) when->expr;

					if (caseexpr->arg)
					{
						/*
						 * The parser should have produced WHEN clauses of the
						 * form "CaseTestExpr = RHS", possibly with an
						 * implicit coercion inserted above the CaseTestExpr.
						 * For accurate decompilation of rules it's essential
						 * that we show just the RHS.  However in an
						 * expression that's been through the optimizer, the
						 * WHEN clause could be almost anything (since the
						 * equality operator could have been expanded into an
						 * inline function).  If we don't recognize the form
						 * of the WHEN clause, just punt and display it as-is.
						 */
						if (IsA(w, OpExpr))
						{
							List	   *args = ((OpExpr *) w)->args;

							if (list_length(args) == 2 &&
								IsA(strip_implicit_coercions(linitial(args)),
									CaseTestExpr))
								w = (Node *) lsecond(args);
						}
					}

					if (!PRETTY_INDENT(context))
						appendStringInfoChar(buf, ' ');
					appendContextKeyword(context, "WHEN ",
										 0, 0, 0);
					get_rule_expr(w, context, false);
					appendStringInfoString(buf, " THEN ");
					get_rule_expr((Node *) when->result, context, true);
				}
				if (!PRETTY_INDENT(context))
					appendStringInfoChar(buf, ' ');
				appendContextKeyword(context, "ELSE ",
									 0, 0, 0);
				get_rule_expr((Node *) caseexpr->defresult, context, true);
				if (!PRETTY_INDENT(context))
					appendStringInfoChar(buf, ' ');
				appendContextKeyword(context, "END",
									 -PRETTYINDENT_VAR, 0, 0);
			}
			break;

		case T_CaseTestExpr:
			{
				/*
				 * Normally we should never get here, since for expressions
				 * that can contain this node type we attempt to avoid
				 * recursing to it.  But in an optimized expression we might
				 * be unable to avoid that (see comments for CaseExpr).  If we
				 * do see one, print it as CASE_TEST_EXPR.
				 */
				appendStringInfoString(buf, "CASE_TEST_EXPR");
			}
			break;

		case T_ArrayExpr:
			{
				ArrayExpr  *arrayexpr = (ArrayExpr *) node;

				appendStringInfoString(buf, "ARRAY[");
				get_rule_expr((Node *) arrayexpr->elements, context, true);
				appendStringInfoChar(buf, ']');

				/*
				 * If the array isn't empty, we assume its elements are
				 * coerced to the desired type.  If it's empty, though, we
				 * need an explicit coercion to the array type.
				 */
				if (arrayexpr->elements == NIL)
					appendStringInfo(buf, "::%s",
									 format_type_with_typemod(arrayexpr->array_typeid, -1));
			}
			break;

		case T_RowExpr:
			{
				RowExpr    *rowexpr = (RowExpr *) node;
				TupleDesc	tupdesc = NULL;
				ListCell   *arg;
				int			i;
				char	   *sep;

				/*
				 * If it's a named type and not RECORD, we may have to skip
				 * dropped columns and/or claim there are NULLs for added
				 * columns.
				 */
				if (rowexpr->row_typeid != RECORDOID)
				{
					tupdesc = lookup_rowtype_tupdesc(rowexpr->row_typeid, -1);
					Assert(list_length(rowexpr->args) <= tupdesc->natts);
				}

				/*
				 * SQL99 allows "ROW" to be omitted when there is more than
				 * one column, but for simplicity we always print it.
				 */
				appendStringInfoString(buf, "ROW(");
				sep = "";
				i = 0;
				foreach(arg, rowexpr->args)
				{
					Node	   *e = (Node *) lfirst(arg);

					if (tupdesc == NULL ||
						!TupleDescAttr(tupdesc, i)->attisdropped)
					{
						appendStringInfoString(buf, sep);
						/* Whole-row Vars need special treatment here */
						get_rule_expr_toplevel(e, context, true);
						sep = ", ";
					}
					i++;
				}
				if (tupdesc != NULL)
				{
					while (i < tupdesc->natts)
					{
						if (!TupleDescAttr(tupdesc, i)->attisdropped)
						{
							appendStringInfoString(buf, sep);
							appendStringInfoString(buf, "NULL");
							sep = ", ";
						}
						i++;
					}

					ReleaseTupleDesc(tupdesc);
				}
				appendStringInfoChar(buf, ')');
				if (rowexpr->row_format == COERCE_EXPLICIT_CAST)
					appendStringInfo(buf, "::%s",
									 format_type_with_typemod(rowexpr->row_typeid, -1));
			}
			break;

		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;

				/*
				 * SQL99 allows "ROW" to be omitted when there is more than
				 * one column, but for simplicity we always print it.  Within
				 * a ROW expression, whole-row Vars need special treatment, so
				 * use get_rule_list_toplevel.
				 */
				appendStringInfoString(buf, "(ROW(");
				get_rule_list_toplevel(rcexpr->largs, context, true);

				/*
				 * We assume that the name of the first-column operator will
				 * do for all the rest too.  This is definitely open to
				 * failure, eg if some but not all operators were renamed
				 * since the construct was parsed, but there seems no way to
				 * be perfect.
				 */
				appendStringInfo(buf, ") %s ROW(",
								 generate_operator_name(linitial_oid(rcexpr->opnos),
														exprType(linitial(rcexpr->largs)),
														exprType(linitial(rcexpr->rargs))));
				get_rule_list_toplevel(rcexpr->rargs, context, true);
				appendStringInfoString(buf, "))");
			}
			break;

		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesceexpr = (CoalesceExpr *) node;

				appendStringInfoString(buf, "COALESCE(");
				get_rule_expr((Node *) coalesceexpr->args, context, true);
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_MinMaxExpr:
			{
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;

				switch (minmaxexpr->op)
				{
					case IS_GREATEST:
						appendStringInfoString(buf, "GREATEST(");
						break;
					case IS_LEAST:
						appendStringInfoString(buf, "LEAST(");
						break;
				}
				get_rule_expr((Node *) minmaxexpr->args, context, true);
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_SQLValueFunction:
			{
				SQLValueFunction *svf = (SQLValueFunction *) node;

				/*
				 * Note: this code knows that typmod for time, timestamp, and
				 * timestamptz just prints as integer.
				 */
				switch (svf->op)
				{
					case SVFOP_CURRENT_DATE:
						appendStringInfoString(buf, "CURRENT_DATE");
						break;
					case SVFOP_CURRENT_TIME:
						appendStringInfoString(buf, "CURRENT_TIME");
						break;
					case SVFOP_CURRENT_TIME_N:
						appendStringInfo(buf, "CURRENT_TIME(%d)", svf->typmod);
						break;
					case SVFOP_CURRENT_TIMESTAMP:
						appendStringInfoString(buf, "CURRENT_TIMESTAMP");
						break;
					case SVFOP_CURRENT_TIMESTAMP_N:
						appendStringInfo(buf, "CURRENT_TIMESTAMP(%d)",
										 svf->typmod);
						break;
					case SVFOP_LOCALTIME:
						appendStringInfoString(buf, "LOCALTIME");
						break;
					case SVFOP_LOCALTIME_N:
						appendStringInfo(buf, "LOCALTIME(%d)", svf->typmod);
						break;
					case SVFOP_LOCALTIMESTAMP:
						appendStringInfoString(buf, "LOCALTIMESTAMP");
						break;
					case SVFOP_LOCALTIMESTAMP_N:
						appendStringInfo(buf, "LOCALTIMESTAMP(%d)",
										 svf->typmod);
						break;
					case SVFOP_CURRENT_ROLE:
						appendStringInfoString(buf, "CURRENT_ROLE");
						break;
					case SVFOP_CURRENT_USER:
						appendStringInfoString(buf, "CURRENT_USER");
						break;
					case SVFOP_USER:
						appendStringInfoString(buf, "USER");
						break;
					case SVFOP_SESSION_USER:
						appendStringInfoString(buf, "SESSION_USER");
						break;
					case SVFOP_CURRENT_CATALOG:
						appendStringInfoString(buf, "CURRENT_CATALOG");
						break;
					case SVFOP_CURRENT_SCHEMA:
						appendStringInfoString(buf, "CURRENT_SCHEMA");
						break;
				}
			}
			break;

		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;
				bool		needcomma = false;
				ListCell   *arg;
				ListCell   *narg;
				Const	   *con;

				switch (xexpr->op)
				{
					case IS_XMLCONCAT:
						appendStringInfoString(buf, "XMLCONCAT(");
						break;
					case IS_XMLELEMENT:
						appendStringInfoString(buf, "XMLELEMENT(");
						break;
					case IS_XMLFOREST:
						appendStringInfoString(buf, "XMLFOREST(");
						break;
					case IS_XMLPARSE:
						appendStringInfoString(buf, "XMLPARSE(");
						break;
					case IS_XMLPI:
						appendStringInfoString(buf, "XMLPI(");
						break;
					case IS_XMLROOT:
						appendStringInfoString(buf, "XMLROOT(");
						break;
					case IS_XMLSERIALIZE:
						appendStringInfoString(buf, "XMLSERIALIZE(");
						break;
					case IS_DOCUMENT:
						break;
				}
				if (xexpr->op == IS_XMLPARSE || xexpr->op == IS_XMLSERIALIZE)
				{
					if (xexpr->xmloption == XMLOPTION_DOCUMENT)
						appendStringInfoString(buf, "DOCUMENT ");
					else
						appendStringInfoString(buf, "CONTENT ");
				}
				if (xexpr->name)
				{
					appendStringInfo(buf, "NAME %s",
									 quote_identifier(map_xml_name_to_sql_identifier(xexpr->name)));
					needcomma = true;
				}
				if (xexpr->named_args)
				{
					if (xexpr->op != IS_XMLFOREST)
					{
						if (needcomma)
							appendStringInfoString(buf, ", ");
						appendStringInfoString(buf, "XMLATTRIBUTES(");
						needcomma = false;
					}
					forboth(arg, xexpr->named_args, narg, xexpr->arg_names)
					{
						Node	   *e = (Node *) lfirst(arg);
						char	   *argname = strVal(lfirst(narg));

						if (needcomma)
							appendStringInfoString(buf, ", ");
						get_rule_expr((Node *) e, context, true);
						appendStringInfo(buf, " AS %s",
										 quote_identifier(map_xml_name_to_sql_identifier(argname)));
						needcomma = true;
					}
					if (xexpr->op != IS_XMLFOREST)
						appendStringInfoChar(buf, ')');
				}
				if (xexpr->args)
				{
					if (needcomma)
						appendStringInfoString(buf, ", ");
					switch (xexpr->op)
					{
						case IS_XMLCONCAT:
						case IS_XMLELEMENT:
						case IS_XMLFOREST:
						case IS_XMLPI:
						case IS_XMLSERIALIZE:
							/* no extra decoration needed */
							get_rule_expr((Node *) xexpr->args, context, true);
							break;
						case IS_XMLPARSE:
							Assert(list_length(xexpr->args) == 2);

							get_rule_expr((Node *) linitial(xexpr->args),
										  context, true);

							con = lsecond_node(Const, xexpr->args);
							Assert(!con->constisnull);
							if (DatumGetBool(con->constvalue))
								appendStringInfoString(buf,
													   " PRESERVE WHITESPACE");
							else
								appendStringInfoString(buf,
													   " STRIP WHITESPACE");
							break;
						case IS_XMLROOT:
							Assert(list_length(xexpr->args) == 3);

							get_rule_expr((Node *) linitial(xexpr->args),
										  context, true);

							appendStringInfoString(buf, ", VERSION ");
							con = (Const *) lsecond(xexpr->args);
							if (IsA(con, Const) &&
								con->constisnull)
								appendStringInfoString(buf, "NO VALUE");
							else
								get_rule_expr((Node *) con, context, false);

							con = lthird_node(Const, xexpr->args);
							if (con->constisnull)
								 /* suppress STANDALONE NO VALUE */ ;
							else
							{
								switch (DatumGetInt32(con->constvalue))
								{
									case XML_STANDALONE_YES:
										appendStringInfoString(buf,
															   ", STANDALONE YES");
										break;
									case XML_STANDALONE_NO:
										appendStringInfoString(buf,
															   ", STANDALONE NO");
										break;
									case XML_STANDALONE_NO_VALUE:
										appendStringInfoString(buf,
															   ", STANDALONE NO VALUE");
										break;
									default:
										break;
								}
							}
							break;
						case IS_DOCUMENT:
							get_rule_expr_paren((Node *) xexpr->args, context, false, node);
							break;
					}
				}
				if (xexpr->op == IS_XMLSERIALIZE)
					appendStringInfo(buf, " AS %s",
									 format_type_with_typemod(xexpr->type,
															  xexpr->typmod));
				if (xexpr->op == IS_DOCUMENT)
					appendStringInfoString(buf, " IS DOCUMENT");
				else
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren((Node *) ntest->arg, context, true, node);

				/*
				 * For scalar inputs, we prefer to print as IS [NOT] NULL,
				 * which is shorter and traditional.  If it's a rowtype input
				 * but we're applying a scalar test, must print IS [NOT]
				 * DISTINCT FROM NULL to be semantically correct.
				 */
				if (ntest->argisrow ||
					!type_is_rowtype(exprType((Node *) ntest->arg)))
				{
					switch (ntest->nulltesttype)
					{
						case IS_NULL:
							appendStringInfoString(buf, " IS NULL");
							break;
						case IS_NOT_NULL:
							appendStringInfoString(buf, " IS NOT NULL");
							break;
						default:
							elog(ERROR, "unrecognized nulltesttype: %d",
								 (int) ntest->nulltesttype);
					}
				}
				else
				{
					switch (ntest->nulltesttype)
					{
						case IS_NULL:
							appendStringInfoString(buf, " IS NOT DISTINCT FROM NULL");
							break;
						case IS_NOT_NULL:
							appendStringInfoString(buf, " IS DISTINCT FROM NULL");
							break;
						default:
							elog(ERROR, "unrecognized nulltesttype: %d",
								 (int) ntest->nulltesttype);
					}
				}
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_BooleanTest:
			{
				BooleanTest *btest = (BooleanTest *) node;

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren((Node *) btest->arg, context, false, node);
				switch (btest->booltesttype)
				{
					case IS_TRUE:
						appendStringInfoString(buf, " IS TRUE");
						break;
					case IS_NOT_TRUE:
						appendStringInfoString(buf, " IS NOT TRUE");
						break;
					case IS_FALSE:
						appendStringInfoString(buf, " IS FALSE");
						break;
					case IS_NOT_FALSE:
						appendStringInfoString(buf, " IS NOT FALSE");
						break;
					case IS_UNKNOWN:
						appendStringInfoString(buf, " IS UNKNOWN");
						break;
					case IS_NOT_UNKNOWN:
						appendStringInfoString(buf, " IS NOT UNKNOWN");
						break;
					default:
						elog(ERROR, "unrecognized booltesttype: %d",
							 (int) btest->booltesttype);
				}
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_CoerceToDomain:
			{
				CoerceToDomain *ctest = (CoerceToDomain *) node;
				Node	   *arg = (Node *) ctest->arg;

				if (ctest->coercionformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr(arg, context, false);
				}
				else
				{
					get_coercion_expr(arg, context,
									  ctest->resulttype,
									  ctest->resulttypmod,
									  node);
				}
			}
			break;

		case T_CoerceToDomainValue:
			appendStringInfoString(buf, "VALUE");
			break;

		case T_SetToDefault:
			appendStringInfoString(buf, "DEFAULT");
			break;

		case T_CurrentOfExpr:
			{
				CurrentOfExpr *cexpr = (CurrentOfExpr *) node;

				if (cexpr->cursor_name)
					appendStringInfo(buf, "CURRENT OF %s",
									 quote_identifier(cexpr->cursor_name));
				else
					appendStringInfo(buf, "CURRENT OF $%d",
									 cexpr->cursor_param);
			}
			break;

		case T_NextValueExpr:
			{
				NextValueExpr *nvexpr = (NextValueExpr *) node;

				/*
				 * This isn't exactly nextval(), but that seems close enough
				 * for EXPLAIN's purposes.
				 */
				appendStringInfoString(buf, "nextval(");
				simple_quote_literal(buf,
									 generate_relation_name(nvexpr->seqid,
															NIL));
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_InferenceElem:
			{
				InferenceElem *iexpr = (InferenceElem *) node;
				bool		save_varprefix;
				bool		need_parens;

				/*
				 * InferenceElem can only refer to target relation, so a
				 * prefix is not useful, and indeed would cause parse errors.
				 */
				save_varprefix = context->varprefix;
				context->varprefix = false;

				/*
				 * Parenthesize the element unless it's a simple Var or a bare
				 * function call.  Follows pg_get_indexdef_worker().
				 */
				need_parens = !IsA(iexpr->expr, Var);
				if (IsA(iexpr->expr, FuncExpr) &&
					((FuncExpr *) iexpr->expr)->funcformat ==
					COERCE_EXPLICIT_CALL)
					need_parens = false;

				if (need_parens)
					appendStringInfoChar(buf, '(');
				get_rule_expr((Node *) iexpr->expr,
							  context, false);
				if (need_parens)
					appendStringInfoChar(buf, ')');

				context->varprefix = save_varprefix;

				if (iexpr->infercollid)
					appendStringInfo(buf, " COLLATE %s",
									 generate_collation_name(iexpr->infercollid));

				/* Add the operator class name, if not default */
				if (iexpr->inferopclass)
				{
					Oid			inferopclass = iexpr->inferopclass;
					Oid			inferopcinputtype = get_opclass_input_type(iexpr->inferopclass);

					get_opclass_name(inferopclass, inferopcinputtype, buf);
				}
			}
			break;

		case T_ReturningExpr:
			{
				ReturningExpr *retExpr = (ReturningExpr *) node;

				/*
				 * We cannot see a ReturningExpr in rule deparsing, only while
				 * EXPLAINing a query plan (ReturningExpr nodes are only ever
				 * adding during query rewriting). Just display the expression
				 * returned (an expanded view column).
				 */
				get_rule_expr((Node *) retExpr->retexpr, context, showimplicit);
			}
			break;

		case T_PartitionBoundSpec:
			{
				PartitionBoundSpec *spec = (PartitionBoundSpec *) node;
				ListCell   *cell;
				char	   *sep;

				if (spec->is_default)
				{
					appendStringInfoString(buf, "DEFAULT");
					break;
				}

				switch (spec->strategy)
				{
					case PARTITION_STRATEGY_HASH:
						Assert(spec->modulus > 0 && spec->remainder >= 0);
						Assert(spec->modulus > spec->remainder);

						appendStringInfoString(buf, "FOR VALUES");
						appendStringInfo(buf, " WITH (modulus %d, remainder %d)",
										 spec->modulus, spec->remainder);
						break;

					case PARTITION_STRATEGY_LIST:
						Assert(spec->listdatums != NIL);

						appendStringInfoString(buf, "FOR VALUES IN (");
						sep = "";
						foreach(cell, spec->listdatums)
						{
							Const	   *val = lfirst_node(Const, cell);

							appendStringInfoString(buf, sep);
							get_const_expr(val, context, -1);
							sep = ", ";
						}

						appendStringInfoChar(buf, ')');
						break;

					case PARTITION_STRATEGY_RANGE:
						Assert(spec->lowerdatums != NIL &&
							   spec->upperdatums != NIL &&
							   list_length(spec->lowerdatums) ==
							   list_length(spec->upperdatums));

						appendStringInfo(buf, "FOR VALUES FROM %s TO %s",
										 get_range_partbound_string(spec->lowerdatums),
										 get_range_partbound_string(spec->upperdatums));
						break;

					default:
						elog(ERROR, "unrecognized partition strategy: %d",
							 (int) spec->strategy);
						break;
				}
			}
			break;

		case T_JsonValueExpr:
			{
				JsonValueExpr *jve = (JsonValueExpr *) node;

				get_rule_expr((Node *) jve->raw_expr, context, false);
				get_json_format(jve->format, context->buf);
			}
			break;

		case T_JsonConstructorExpr:
			get_json_constructor((JsonConstructorExpr *) node, context, false);
			break;

		case T_JsonIsPredicate:
			{
				JsonIsPredicate *pred = (JsonIsPredicate *) node;

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(context->buf, '(');

				get_rule_expr_paren(pred->expr, context, true, node);

				appendStringInfoString(context->buf, " IS JSON");

				/* TODO: handle FORMAT clause */

				switch (pred->item_type)
				{
					case JS_TYPE_SCALAR:
						appendStringInfoString(context->buf, " SCALAR");
						break;
					case JS_TYPE_ARRAY:
						appendStringInfoString(context->buf, " ARRAY");
						break;
					case JS_TYPE_OBJECT:
						appendStringInfoString(context->buf, " OBJECT");
						break;
					default:
						break;
				}

				if (pred->unique_keys)
					appendStringInfoString(context->buf, " WITH UNIQUE KEYS");

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(context->buf, ')');
			}
			break;

		case T_JsonExpr:
			{
				JsonExpr   *jexpr = (JsonExpr *) node;

				switch (jexpr->op)
				{
					case JSON_EXISTS_OP:
						appendStringInfoString(buf, "JSON_EXISTS(");
						break;
					case JSON_QUERY_OP:
						appendStringInfoString(buf, "JSON_QUERY(");
						break;
					case JSON_VALUE_OP:
						appendStringInfoString(buf, "JSON_VALUE(");
						break;
					default:
						elog(ERROR, "unrecognized JsonExpr op: %d",
							 (int) jexpr->op);
				}

				get_rule_expr(jexpr->formatted_expr, context, showimplicit);

				appendStringInfoString(buf, ", ");

				get_json_path_spec(jexpr->path_spec, context, showimplicit);

				if (jexpr->passing_values)
				{
					ListCell   *lc1,
							   *lc2;
					bool		needcomma = false;

					appendStringInfoString(buf, " PASSING ");

					forboth(lc1, jexpr->passing_names,
							lc2, jexpr->passing_values)
					{
						if (needcomma)
							appendStringInfoString(buf, ", ");
						needcomma = true;

						get_rule_expr((Node *) lfirst(lc2), context, showimplicit);
						appendStringInfo(buf, " AS %s",
										 quote_identifier(lfirst_node(String, lc1)->sval));
					}
				}

				if (jexpr->op != JSON_EXISTS_OP ||
					jexpr->returning->typid != BOOLOID)
					get_json_returning(jexpr->returning, context->buf,
									   jexpr->op == JSON_QUERY_OP);

				get_json_expr_options(jexpr, context,
									  jexpr->op != JSON_EXISTS_OP ?
									  JSON_BEHAVIOR_NULL :
									  JSON_BEHAVIOR_FALSE);

				appendStringInfoChar(buf, ')');
			}
			break;

		case T_List:
			{
				char	   *sep;
				ListCell   *l;

				sep = "";
				foreach(l, (List *) node)
				{
					appendStringInfoString(buf, sep);
					get_rule_expr((Node *) lfirst(l), context, showimplicit);
					sep = ", ";
				}
			}
			break;

		case T_TableFunc:
			get_tablefunc((TableFunc *) node, context, showimplicit);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}
}

/*
 * get_rule_expr_toplevel		- Parse back a toplevel expression
 *
 * Same as get_rule_expr(), except that if the expr is just a Var, we pass
 * istoplevel = true not false to get_variable().  This causes whole-row Vars
 * to get printed with decoration that will prevent expansion of "*".
 * We need to use this in contexts such as ROW() and VALUES(), where the
 * parser would expand "foo.*" appearing at top level.  (In principle we'd
 * use this in get_target_list() too, but that has additional worries about
 * whether to print AS, so it needs to invoke get_variable() directly anyway.)
 */
static void
get_rule_expr_toplevel(Node *node, deparse_context *context,
					   bool showimplicit)
{
	if (node && IsA(node, Var))
		(void) get_variable((Var *) node, 0, true, context);
	else
		get_rule_expr(node, context, showimplicit);
}

/*
 * get_rule_list_toplevel		- Parse back a list of toplevel expressions
 *
 * Apply get_rule_expr_toplevel() to each element of a List.
 *
 * This adds commas between the expressions, but caller is responsible
 * for printing surrounding decoration.
 */
static void
get_rule_list_toplevel(List *lst, deparse_context *context,
					   bool showimplicit)
{
	const char *sep;
	ListCell   *lc;

	sep = "";
	foreach(lc, lst)
	{
		Node	   *e = (Node *) lfirst(lc);

		appendStringInfoString(context->buf, sep);
		get_rule_expr_toplevel(e, context, showimplicit);
		sep = ", ";
	}
}

/*
 * get_rule_expr_funccall		- Parse back a function-call expression
 *
 * Same as get_rule_expr(), except that we guarantee that the output will
 * look like a function call, or like one of the things the grammar treats as
 * equivalent to a function call (see the func_expr_windowless production).
 * This is needed in places where the grammar uses func_expr_windowless and
 * you can't substitute a parenthesized a_expr.  If what we have isn't going
 * to look like a function call, wrap it in a dummy CAST() expression, which
 * will satisfy the grammar --- and, indeed, is likely what the user wrote to
 * produce such a thing.
 */
static void
get_rule_expr_funccall(Node *node, deparse_context *context,
					   bool showimplicit)
{
	if (looks_like_function(node))
		get_rule_expr(node, context, showimplicit);
	else
	{
		StringInfo	buf = context->buf;

		appendStringInfoString(buf, "CAST(");
		/* no point in showing any top-level implicit cast */
		get_rule_expr(node, context, false);
		appendStringInfo(buf, " AS %s)",
						 format_type_with_typemod(exprType(node),
												  exprTypmod(node)));
	}
}

/*
 * Helper function to identify node types that satisfy func_expr_windowless.
 * If in doubt, "false" is always a safe answer.
 */
static bool
looks_like_function(Node *node)
{
	if (node == NULL)
		return false;			/* probably shouldn't happen */
	switch (nodeTag(node))
	{
		case T_FuncExpr:
			/* OK, unless it's going to deparse as a cast */
			return (((FuncExpr *) node)->funcformat == COERCE_EXPLICIT_CALL ||
					((FuncExpr *) node)->funcformat == COERCE_SQL_SYNTAX);
		case T_NullIfExpr:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
		case T_SQLValueFunction:
		case T_XmlExpr:
		case T_JsonExpr:
			/* these are all accepted by func_expr_common_subexpr */
			return true;
		default:
			break;
	}
	return false;
}


/*
 * get_oper_expr			- Parse back an OpExpr node
 */
static void
get_oper_expr(OpExpr *expr, deparse_context *context)
{
	StringInfo	buf = context->buf;
	Oid			opno = expr->opno;
	List	   *args = expr->args;

	if (!PRETTY_PAREN(context))
		appendStringInfoChar(buf, '(');
	if (list_length(args) == 2)
	{
		/* binary operator */
		Node	   *arg1 = (Node *) linitial(args);
		Node	   *arg2 = (Node *) lsecond(args);

		get_rule_expr_paren(arg1, context, true, (Node *) expr);
		appendStringInfo(buf, " %s ",
						 generate_operator_name(opno,
												exprType(arg1),
												exprType(arg2)));
		get_rule_expr_paren(arg2, context, true, (Node *) expr);
	}
	else
	{
		/* prefix operator */
		Node	   *arg = (Node *) linitial(args);

		appendStringInfo(buf, "%s ",
						 generate_operator_name(opno,
												InvalidOid,
												exprType(arg)));
		get_rule_expr_paren(arg, context, true, (Node *) expr);
	}
	if (!PRETTY_PAREN(context))
		appendStringInfoChar(buf, ')');
}

/*
 * get_func_expr			- Parse back a FuncExpr node
 */
static void
get_func_expr(FuncExpr *expr, deparse_context *context,
			  bool showimplicit)
{
	StringInfo	buf = context->buf;
	Oid			funcoid = expr->funcid;
	Oid			argtypes[FUNC_MAX_ARGS];
	int			nargs;
	List	   *argnames;
	bool		use_variadic;
	ListCell   *l;

	/*
	 * If the function call came from an implicit coercion, then just show the
	 * first argument --- unless caller wants to see implicit coercions.
	 */
	if (expr->funcformat == COERCE_IMPLICIT_CAST && !showimplicit)
	{
		get_rule_expr_paren((Node *) linitial(expr->args), context,
							false, (Node *) expr);
		return;
	}

	/*
	 * If the function call came from a cast, then show the first argument
	 * plus an explicit cast operation.
	 */
	if (expr->funcformat == COERCE_EXPLICIT_CAST ||
		expr->funcformat == COERCE_IMPLICIT_CAST)
	{
		Node	   *arg = linitial(expr->args);
		Oid			rettype = expr->funcresulttype;
		int32		coercedTypmod;

		/* Get the typmod if this is a length-coercion function */
		(void) exprIsLengthCoercion((Node *) expr, &coercedTypmod);

		get_coercion_expr(arg, context,
						  rettype, coercedTypmod,
						  (Node *) expr);

		return;
	}

	/*
	 * If the function was called using one of the SQL spec's random special
	 * syntaxes, try to reproduce that.  If we don't recognize the function,
	 * fall through.
	 */
	if (expr->funcformat == COERCE_SQL_SYNTAX)
	{
		if (get_func_sql_syntax(expr, context))
			return;
	}

	/*
	 * Normal function: display as proname(args).  First we need to extract
	 * the argument datatypes.
	 */
	if (list_length(expr->args) > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("too many arguments")));
	nargs = 0;
	argnames = NIL;
	foreach(l, expr->args)
	{
		Node	   *arg = (Node *) lfirst(l);

		if (IsA(arg, NamedArgExpr))
			argnames = lappend(argnames, ((NamedArgExpr *) arg)->name);
		argtypes[nargs] = exprType(arg);
		nargs++;
	}

	appendStringInfo(buf, "%s(",
					 generate_function_name(funcoid, nargs,
											argnames, argtypes,
											expr->funcvariadic,
											&use_variadic,
											context->inGroupBy));
	nargs = 0;
	foreach(l, expr->args)
	{
		if (nargs++ > 0)
			appendStringInfoString(buf, ", ");
		if (use_variadic && lnext(expr->args, l) == NULL)
			appendStringInfoString(buf, "VARIADIC ");
		get_rule_expr((Node *) lfirst(l), context, true);
	}
	appendStringInfoChar(buf, ')');
}

/*
 * get_agg_expr			- Parse back an Aggref node
 */
static void
get_agg_expr(Aggref *aggref, deparse_context *context,
			 Aggref *original_aggref)
{
	get_agg_expr_helper(aggref, context, original_aggref, NULL, NULL,
						false);
}

/*
 * get_agg_expr_helper		- subroutine for get_agg_expr and
 *							get_json_agg_constructor
 */
static void
get_agg_expr_helper(Aggref *aggref, deparse_context *context,
					Aggref *original_aggref, const char *funcname,
					const char *options, bool is_json_objectagg)
{
	StringInfo	buf = context->buf;
	Oid			argtypes[FUNC_MAX_ARGS];
	int			nargs;
	bool		use_variadic = false;

	/*
	 * For a combining aggregate, we look up and deparse the corresponding
	 * partial aggregate instead.  This is necessary because our input
	 * argument list has been replaced; the new argument list always has just
	 * one element, which will point to a partial Aggref that supplies us with
	 * transition states to combine.
	 */
	if (DO_AGGSPLIT_COMBINE(aggref->aggsplit))
	{
		TargetEntry *tle;

		Assert(list_length(aggref->args) == 1);
		tle = linitial_node(TargetEntry, aggref->args);
		resolve_special_varno((Node *) tle->expr, context,
							  get_agg_combine_expr, original_aggref);
		return;
	}

	/*
	 * Mark as PARTIAL, if appropriate.  We look to the original aggref so as
	 * to avoid printing this when recursing from the code just above.
	 */
	if (DO_AGGSPLIT_SKIPFINAL(original_aggref->aggsplit))
		appendStringInfoString(buf, "PARTIAL ");

	/* Extract the argument types as seen by the parser */
	nargs = get_aggregate_argtypes(aggref, argtypes);

	if (!funcname)
		funcname = generate_function_name(aggref->aggfnoid, nargs, NIL,
										  argtypes, aggref->aggvariadic,
										  &use_variadic,
										  context->inGroupBy);

	/* Print the aggregate name, schema-qualified if needed */
	appendStringInfo(buf, "%s(%s", funcname,
					 (aggref->aggdistinct != NIL) ? "DISTINCT " : "");

	if (AGGKIND_IS_ORDERED_SET(aggref->aggkind))
	{
		/*
		 * Ordered-set aggregates do not use "*" syntax.  Also, we needn't
		 * worry about inserting VARIADIC.  So we can just dump the direct
		 * args as-is.
		 */
		Assert(!aggref->aggvariadic);
		get_rule_expr((Node *) aggref->aggdirectargs, context, true);
		Assert(aggref->aggorder != NIL);
		appendStringInfoString(buf, ") WITHIN GROUP (ORDER BY ");
		get_rule_orderby(aggref->aggorder, aggref->args, false, context);
	}
	else
	{
		/* aggstar can be set only in zero-argument aggregates */
		if (aggref->aggstar)
			appendStringInfoChar(buf, '*');
		else
		{
			ListCell   *l;
			int			i;

			i = 0;
			foreach(l, aggref->args)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(l);
				Node	   *arg = (Node *) tle->expr;

				Assert(!IsA(arg, NamedArgExpr));
				if (tle->resjunk)
					continue;
				if (i++ > 0)
				{
					if (is_json_objectagg)
					{
						/*
						 * the ABSENT ON NULL and WITH UNIQUE args are printed
						 * separately, so ignore them here
						 */
						if (i > 2)
							break;

						appendStringInfoString(buf, " : ");
					}
					else
						appendStringInfoString(buf, ", ");
				}
				if (use_variadic && i == nargs)
					appendStringInfoString(buf, "VARIADIC ");
				get_rule_expr(arg, context, true);
			}
		}

		if (aggref->aggorder != NIL)
		{
			appendStringInfoString(buf, " ORDER BY ");
			get_rule_orderby(aggref->aggorder, aggref->args, false, context);
		}
	}

	if (options)
		appendStringInfoString(buf, options);

	if (aggref->aggfilter != NULL)
	{
		appendStringInfoString(buf, ") FILTER (WHERE ");
		get_rule_expr((Node *) aggref->aggfilter, context, false);
	}

	appendStringInfoChar(buf, ')');
}

/*
 * This is a helper function for get_agg_expr().  It's used when we deparse
 * a combining Aggref; resolve_special_varno locates the corresponding partial
 * Aggref and then calls this.
 */
static void
get_agg_combine_expr(Node *node, deparse_context *context, void *callback_arg)
{
	Aggref	   *aggref;
	Aggref	   *original_aggref = callback_arg;

	if (!IsA(node, Aggref))
		elog(ERROR, "combining Aggref does not point to an Aggref");

	aggref = (Aggref *) node;
	get_agg_expr(aggref, context, original_aggref);
}

/*
 * get_windowfunc_expr	- Parse back a WindowFunc node
 */
static void
get_windowfunc_expr(WindowFunc *wfunc, deparse_context *context)
{
	get_windowfunc_expr_helper(wfunc, context, NULL, NULL, false);
}


/*
 * get_windowfunc_expr_helper	- subroutine for get_windowfunc_expr and
 *								get_json_agg_constructor
 */
static void
get_windowfunc_expr_helper(WindowFunc *wfunc, deparse_context *context,
						   const char *funcname, const char *options,
						   bool is_json_objectagg)
{
	StringInfo	buf = context->buf;
	Oid			argtypes[FUNC_MAX_ARGS];
	int			nargs;
	List	   *argnames;
	ListCell   *l;

	if (list_length(wfunc->args) > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("too many arguments")));
	nargs = 0;
	argnames = NIL;
	foreach(l, wfunc->args)
	{
		Node	   *arg = (Node *) lfirst(l);

		if (IsA(arg, NamedArgExpr))
			argnames = lappend(argnames, ((NamedArgExpr *) arg)->name);
		argtypes[nargs] = exprType(arg);
		nargs++;
	}

	if (!funcname)
		funcname = generate_function_name(wfunc->winfnoid, nargs, argnames,
										  argtypes, false, NULL,
										  context->inGroupBy);

	appendStringInfo(buf, "%s(", funcname);

	/* winstar can be set only in zero-argument aggregates */
	if (wfunc->winstar)
		appendStringInfoChar(buf, '*');
	else
	{
		if (is_json_objectagg)
		{
			get_rule_expr((Node *) linitial(wfunc->args), context, false);
			appendStringInfoString(buf, " : ");
			get_rule_expr((Node *) lsecond(wfunc->args), context, false);
		}
		else
			get_rule_expr((Node *) wfunc->args, context, true);
	}

	if (options)
		appendStringInfoString(buf, options);

	if (wfunc->aggfilter != NULL)
	{
		appendStringInfoString(buf, ") FILTER (WHERE ");
		get_rule_expr((Node *) wfunc->aggfilter, context, false);
	}

	appendStringInfoString(buf, ") OVER ");

	foreach(l, context->windowClause)
	{
		WindowClause *wc = (WindowClause *) lfirst(l);

		if (wc->winref == wfunc->winref)
		{
			if (wc->name)
				appendStringInfoString(buf, quote_identifier(wc->name));
			else
				get_rule_windowspec(wc, context->targetList, context);
			break;
		}
	}
	if (l == NULL)
	{
		if (context->windowClause)
			elog(ERROR, "could not find window clause for winref %u",
				 wfunc->winref);

		/*
		 * In EXPLAIN, we don't have window context information available, so
		 * we have to settle for this:
		 */
		appendStringInfoString(buf, "(?)");
	}
}

/*
 * get_func_sql_syntax		- Parse back a SQL-syntax function call
 *
 * Returns true if we successfully deparsed, false if we did not
 * recognize the function.
 */
static bool
get_func_sql_syntax(FuncExpr *expr, deparse_context *context)
{
	StringInfo	buf = context->buf;
	Oid			funcoid = expr->funcid;

	switch (funcoid)
	{
		case F_TIMEZONE_INTERVAL_TIMESTAMP:
		case F_TIMEZONE_INTERVAL_TIMESTAMPTZ:
		case F_TIMEZONE_INTERVAL_TIMETZ:
		case F_TIMEZONE_TEXT_TIMESTAMP:
		case F_TIMEZONE_TEXT_TIMESTAMPTZ:
		case F_TIMEZONE_TEXT_TIMETZ:
			/* AT TIME ZONE ... note reversed argument order */
			appendStringInfoChar(buf, '(');
			get_rule_expr_paren((Node *) lsecond(expr->args), context, false,
								(Node *) expr);
			appendStringInfoString(buf, " AT TIME ZONE ");
			get_rule_expr_paren((Node *) linitial(expr->args), context, false,
								(Node *) expr);
			appendStringInfoChar(buf, ')');
			return true;

		case F_TIMEZONE_TIMESTAMP:
		case F_TIMEZONE_TIMESTAMPTZ:
		case F_TIMEZONE_TIMETZ:
			/* AT LOCAL */
			appendStringInfoChar(buf, '(');
			get_rule_expr_paren((Node *) linitial(expr->args), context, false,
								(Node *) expr);
			appendStringInfoString(buf, " AT LOCAL)");
			return true;

		case F_OVERLAPS_TIMESTAMPTZ_INTERVAL_TIMESTAMPTZ_INTERVAL:
		case F_OVERLAPS_TIMESTAMPTZ_INTERVAL_TIMESTAMPTZ_TIMESTAMPTZ:
		case F_OVERLAPS_TIMESTAMPTZ_TIMESTAMPTZ_TIMESTAMPTZ_INTERVAL:
		case F_OVERLAPS_TIMESTAMPTZ_TIMESTAMPTZ_TIMESTAMPTZ_TIMESTAMPTZ:
		case F_OVERLAPS_TIMESTAMP_INTERVAL_TIMESTAMP_INTERVAL:
		case F_OVERLAPS_TIMESTAMP_INTERVAL_TIMESTAMP_TIMESTAMP:
		case F_OVERLAPS_TIMESTAMP_TIMESTAMP_TIMESTAMP_INTERVAL:
		case F_OVERLAPS_TIMESTAMP_TIMESTAMP_TIMESTAMP_TIMESTAMP:
		case F_OVERLAPS_TIMETZ_TIMETZ_TIMETZ_TIMETZ:
		case F_OVERLAPS_TIME_INTERVAL_TIME_INTERVAL:
		case F_OVERLAPS_TIME_INTERVAL_TIME_TIME:
		case F_OVERLAPS_TIME_TIME_TIME_INTERVAL:
		case F_OVERLAPS_TIME_TIME_TIME_TIME:
			/* (x1, x2) OVERLAPS (y1, y2) */
			appendStringInfoString(buf, "((");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoString(buf, ", ");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			appendStringInfoString(buf, ") OVERLAPS (");
			get_rule_expr((Node *) lthird(expr->args), context, false);
			appendStringInfoString(buf, ", ");
			get_rule_expr((Node *) lfourth(expr->args), context, false);
			appendStringInfoString(buf, "))");
			return true;

		case F_EXTRACT_TEXT_DATE:
		case F_EXTRACT_TEXT_TIME:
		case F_EXTRACT_TEXT_TIMETZ:
		case F_EXTRACT_TEXT_TIMESTAMP:
		case F_EXTRACT_TEXT_TIMESTAMPTZ:
		case F_EXTRACT_TEXT_INTERVAL:
			/* EXTRACT (x FROM y) */
			appendStringInfoString(buf, "EXTRACT(");
			{
				Const	   *con = (Const *) linitial(expr->args);

				Assert(IsA(con, Const) &&
					   con->consttype == TEXTOID &&
					   !con->constisnull);
				appendStringInfoString(buf, TextDatumGetCString(con->constvalue));
			}
			appendStringInfoString(buf, " FROM ");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			appendStringInfoChar(buf, ')');
			return true;

		case F_IS_NORMALIZED:
			/* IS xxx NORMALIZED */
			appendStringInfoChar(buf, '(');
			get_rule_expr_paren((Node *) linitial(expr->args), context, false,
								(Node *) expr);
			appendStringInfoString(buf, " IS");
			if (list_length(expr->args) == 2)
			{
				Const	   *con = (Const *) lsecond(expr->args);

				Assert(IsA(con, Const) &&
					   con->consttype == TEXTOID &&
					   !con->constisnull);
				appendStringInfo(buf, " %s",
								 TextDatumGetCString(con->constvalue));
			}
			appendStringInfoString(buf, " NORMALIZED)");
			return true;

		case F_PG_COLLATION_FOR:
			/* COLLATION FOR */
			appendStringInfoString(buf, "COLLATION FOR (");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoChar(buf, ')');
			return true;

		case F_NORMALIZE:
			/* NORMALIZE() */
			appendStringInfoString(buf, "NORMALIZE(");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			if (list_length(expr->args) == 2)
			{
				Const	   *con = (Const *) lsecond(expr->args);

				Assert(IsA(con, Const) &&
					   con->consttype == TEXTOID &&
					   !con->constisnull);
				appendStringInfo(buf, ", %s",
								 TextDatumGetCString(con->constvalue));
			}
			appendStringInfoChar(buf, ')');
			return true;

		case F_OVERLAY_BIT_BIT_INT4:
		case F_OVERLAY_BIT_BIT_INT4_INT4:
		case F_OVERLAY_BYTEA_BYTEA_INT4:
		case F_OVERLAY_BYTEA_BYTEA_INT4_INT4:
		case F_OVERLAY_TEXT_TEXT_INT4:
		case F_OVERLAY_TEXT_TEXT_INT4_INT4:
			/* OVERLAY() */
			appendStringInfoString(buf, "OVERLAY(");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoString(buf, " PLACING ");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			appendStringInfoString(buf, " FROM ");
			get_rule_expr((Node *) lthird(expr->args), context, false);
			if (list_length(expr->args) == 4)
			{
				appendStringInfoString(buf, " FOR ");
				get_rule_expr((Node *) lfourth(expr->args), context, false);
			}
			appendStringInfoChar(buf, ')');
			return true;

		case F_POSITION_BIT_BIT:
		case F_POSITION_BYTEA_BYTEA:
		case F_POSITION_TEXT_TEXT:
			/* POSITION() ... extra parens since args are b_expr not a_expr */
			appendStringInfoString(buf, "POSITION((");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			appendStringInfoString(buf, ") IN (");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoString(buf, "))");
			return true;

		case F_SUBSTRING_BIT_INT4:
		case F_SUBSTRING_BIT_INT4_INT4:
		case F_SUBSTRING_BYTEA_INT4:
		case F_SUBSTRING_BYTEA_INT4_INT4:
		case F_SUBSTRING_TEXT_INT4:
		case F_SUBSTRING_TEXT_INT4_INT4:
			/* SUBSTRING FROM/FOR (i.e., integer-position variants) */
			appendStringInfoString(buf, "SUBSTRING(");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoString(buf, " FROM ");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			if (list_length(expr->args) == 3)
			{
				appendStringInfoString(buf, " FOR ");
				get_rule_expr((Node *) lthird(expr->args), context, false);
			}
			appendStringInfoChar(buf, ')');
			return true;

		case F_SUBSTRING_TEXT_TEXT_TEXT:
			/* SUBSTRING SIMILAR/ESCAPE */
			appendStringInfoString(buf, "SUBSTRING(");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoString(buf, " SIMILAR ");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			appendStringInfoString(buf, " ESCAPE ");
			get_rule_expr((Node *) lthird(expr->args), context, false);
			appendStringInfoChar(buf, ')');
			return true;

		case F_BTRIM_BYTEA_BYTEA:
		case F_BTRIM_TEXT:
		case F_BTRIM_TEXT_TEXT:
			/* TRIM() */
			appendStringInfoString(buf, "TRIM(BOTH");
			if (list_length(expr->args) == 2)
			{
				appendStringInfoChar(buf, ' ');
				get_rule_expr((Node *) lsecond(expr->args), context, false);
			}
			appendStringInfoString(buf, " FROM ");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoChar(buf, ')');
			return true;

		case F_LTRIM_BYTEA_BYTEA:
		case F_LTRIM_TEXT:
		case F_LTRIM_TEXT_TEXT:
			/* TRIM() */
			appendStringInfoString(buf, "TRIM(LEADING");
			if (list_length(expr->args) == 2)
			{
				appendStringInfoChar(buf, ' ');
				get_rule_expr((Node *) lsecond(expr->args), context, false);
			}
			appendStringInfoString(buf, " FROM ");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoChar(buf, ')');
			return true;

		case F_RTRIM_BYTEA_BYTEA:
		case F_RTRIM_TEXT:
		case F_RTRIM_TEXT_TEXT:
			/* TRIM() */
			appendStringInfoString(buf, "TRIM(TRAILING");
			if (list_length(expr->args) == 2)
			{
				appendStringInfoChar(buf, ' ');
				get_rule_expr((Node *) lsecond(expr->args), context, false);
			}
			appendStringInfoString(buf, " FROM ");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoChar(buf, ')');
			return true;

		case F_SYSTEM_USER:
			appendStringInfoString(buf, "SYSTEM_USER");
			return true;

		case F_XMLEXISTS:
			/* XMLEXISTS ... extra parens because args are c_expr */
			appendStringInfoString(buf, "XMLEXISTS((");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoString(buf, ") PASSING (");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			appendStringInfoString(buf, "))");
			return true;
	}
	return false;
}

/* ----------
 * get_coercion_expr
 *
 *	Make a string representation of a value coerced to a specific type
 * ----------
 */
static void
get_coercion_expr(Node *arg, deparse_context *context,
				  Oid resulttype, int32 resulttypmod,
				  Node *parentNode)
{
	StringInfo	buf = context->buf;

	/*
	 * Since parse_coerce.c doesn't immediately collapse application of
	 * length-coercion functions to constants, what we'll typically see in
	 * such cases is a Const with typmod -1 and a length-coercion function
	 * right above it.  Avoid generating redundant output. However, beware of
	 * suppressing casts when the user actually wrote something like
	 * 'foo'::text::char(3).
	 *
	 * Note: it might seem that we are missing the possibility of needing to
	 * print a COLLATE clause for such a Const.  However, a Const could only
	 * have nondefault collation in a post-constant-folding tree, in which the
	 * length coercion would have been folded too.  See also the special
	 * handling of CollateExpr in coerce_to_target_type(): any collation
	 * marking will be above the coercion node, not below it.
	 */
	if (arg && IsA(arg, Const) &&
		((Const *) arg)->consttype == resulttype &&
		((Const *) arg)->consttypmod == -1)
	{
		/* Show the constant without normal ::typename decoration */
		get_const_expr((Const *) arg, context, -1);
	}
	else
	{
		if (!PRETTY_PAREN(context))
			appendStringInfoChar(buf, '(');
		get_rule_expr_paren(arg, context, false, parentNode);
		if (!PRETTY_PAREN(context))
			appendStringInfoChar(buf, ')');
	}

	/*
	 * Never emit resulttype(arg) functional notation. A pg_proc entry could
	 * take precedence, and a resulttype in pg_temp would require schema
	 * qualification that format_type_with_typemod() would usually omit. We've
	 * standardized on arg::resulttype, but CAST(arg AS resulttype) notation
	 * would work fine.
	 */
	appendStringInfo(buf, "::%s",
					 format_type_with_typemod(resulttype, resulttypmod));
}

/* ----------
 * get_const_expr
 *
 *	Make a string representation of a Const
 *
 * showtype can be -1 to never show "::typename" decoration, or +1 to always
 * show it, or 0 to show it only if the constant wouldn't be assumed to be
 * the right type by default.
 *
 * If the Const's collation isn't default for its type, show that too.
 * We mustn't do this when showtype is -1 (since that means the caller will
 * print "::typename", and we can't put a COLLATE clause in between).  It's
 * caller's responsibility that collation isn't missed in such cases.
 * ----------
 */
static void
get_const_expr(Const *constval, deparse_context *context, int showtype)
{
	StringInfo	buf = context->buf;
	Oid			typoutput;
	bool		typIsVarlena;
	char	   *extval;
	bool		needlabel = false;

	if (constval->constisnull)
	{
		/*
		 * Always label the type of a NULL constant to prevent misdecisions
		 * about type when reparsing.
		 */
		appendStringInfoString(buf, "NULL");
		if (showtype >= 0)
		{
			appendStringInfo(buf, "::%s",
							 format_type_with_typemod(constval->consttype,
													  constval->consttypmod));
			get_const_collation(constval, context);
		}
		return;
	}

	getTypeOutputInfo(constval->consttype,
					  &typoutput, &typIsVarlena);

	extval = OidOutputFunctionCall(typoutput, constval->constvalue);

	switch (constval->consttype)
	{
		case INT4OID:

			/*
			 * INT4 can be printed without any decoration, unless it is
			 * negative; in that case print it as '-nnn'::integer to ensure
			 * that the output will re-parse as a constant, not as a constant
			 * plus operator.  In most cases we could get away with printing
			 * (-nnn) instead, because of the way that gram.y handles negative
			 * literals; but that doesn't work for INT_MIN, and it doesn't
			 * seem that much prettier anyway.
			 */
			if (extval[0] != '-')
				appendStringInfoString(buf, extval);
			else
			{
				appendStringInfo(buf, "'%s'", extval);
				needlabel = true;	/* we must attach a cast */
			}
			break;

		case NUMERICOID:

			/*
			 * NUMERIC can be printed without quotes if it looks like a float
			 * constant (not an integer, and not Infinity or NaN) and doesn't
			 * have a leading sign (for the same reason as for INT4).
			 */
			if (isdigit((unsigned char) extval[0]) &&
				strcspn(extval, "eE.") != strlen(extval))
			{
				appendStringInfoString(buf, extval);
			}
			else
			{
				appendStringInfo(buf, "'%s'", extval);
				needlabel = true;	/* we must attach a cast */
			}
			break;

		case BOOLOID:
			if (strcmp(extval, "t") == 0)
				appendStringInfoString(buf, "true");
			else
				appendStringInfoString(buf, "false");
			break;

		default:
			simple_quote_literal(buf, extval);
			break;
	}

	pfree(extval);

	if (showtype < 0)
		return;

	/*
	 * For showtype == 0, append ::typename unless the constant will be
	 * implicitly typed as the right type when it is read in.
	 *
	 * XXX this code has to be kept in sync with the behavior of the parser,
	 * especially make_const.
	 */
	switch (constval->consttype)
	{
		case BOOLOID:
		case UNKNOWNOID:
			/* These types can be left unlabeled */
			needlabel = false;
			break;
		case INT4OID:
			/* We determined above whether a label is needed */
			break;
		case NUMERICOID:

			/*
			 * Float-looking constants will be typed as numeric, which we
			 * checked above; but if there's a nondefault typmod we need to
			 * show it.
			 */
			needlabel |= (constval->consttypmod >= 0);
			break;
		default:
			needlabel = true;
			break;
	}
	if (needlabel || showtype > 0)
		appendStringInfo(buf, "::%s",
						 format_type_with_typemod(constval->consttype,
												  constval->consttypmod));

	get_const_collation(constval, context);
}

/*
 * helper for get_const_expr: append COLLATE if needed
 */
static void
get_const_collation(Const *constval, deparse_context *context)
{
	StringInfo	buf = context->buf;

	if (OidIsValid(constval->constcollid))
	{
		Oid			typcollation = get_typcollation(constval->consttype);

		if (constval->constcollid != typcollation)
		{
			appendStringInfo(buf, " COLLATE %s",
							 generate_collation_name(constval->constcollid));
		}
	}
}

/*
 * get_json_path_spec		- Parse back a JSON path specification
 */
static void
get_json_path_spec(Node *path_spec, deparse_context *context, bool showimplicit)
{
	if (IsA(path_spec, Const))
		get_const_expr((Const *) path_spec, context, -1);
	else
		get_rule_expr(path_spec, context, showimplicit);
}

/*
 * get_json_format			- Parse back a JsonFormat node
 */
static void
get_json_format(JsonFormat *format, StringInfo buf)
{
	if (format->format_type == JS_FORMAT_DEFAULT)
		return;

	appendStringInfoString(buf,
						   format->format_type == JS_FORMAT_JSONB ?
						   " FORMAT JSONB" : " FORMAT JSON");

	if (format->encoding != JS_ENC_DEFAULT)
	{
		const char *encoding;

		encoding =
			format->encoding == JS_ENC_UTF16 ? "UTF16" :
			format->encoding == JS_ENC_UTF32 ? "UTF32" : "UTF8";

		appendStringInfo(buf, " ENCODING %s", encoding);
	}
}

/*
 * get_json_returning		- Parse back a JsonReturning structure
 */
static void
get_json_returning(JsonReturning *returning, StringInfo buf,
				   bool json_format_by_default)
{
	if (!OidIsValid(returning->typid))
		return;

	appendStringInfo(buf, " RETURNING %s",
					 format_type_with_typemod(returning->typid,
											  returning->typmod));

	if (!json_format_by_default ||
		returning->format->format_type !=
		(returning->typid == JSONBOID ? JS_FORMAT_JSONB : JS_FORMAT_JSON))
		get_json_format(returning->format, buf);
}

/*
 * get_json_constructor		- Parse back a JsonConstructorExpr node
 */
static void
get_json_constructor(JsonConstructorExpr *ctor, deparse_context *context,
					 bool showimplicit)
{
	StringInfo	buf = context->buf;
	const char *funcname;
	bool		is_json_object;
	int			curridx;
	ListCell   *lc;

	if (ctor->type == JSCTOR_JSON_OBJECTAGG)
	{
		get_json_agg_constructor(ctor, context, "JSON_OBJECTAGG", true);
		return;
	}
	else if (ctor->type == JSCTOR_JSON_ARRAYAGG)
	{
		get_json_agg_constructor(ctor, context, "JSON_ARRAYAGG", false);
		return;
	}

	switch (ctor->type)
	{
		case JSCTOR_JSON_OBJECT:
			funcname = "JSON_OBJECT";
			break;
		case JSCTOR_JSON_ARRAY:
			funcname = "JSON_ARRAY";
			break;
		case JSCTOR_JSON_PARSE:
			funcname = "JSON";
			break;
		case JSCTOR_JSON_SCALAR:
			funcname = "JSON_SCALAR";
			break;
		case JSCTOR_JSON_SERIALIZE:
			funcname = "JSON_SERIALIZE";
			break;
		default:
			elog(ERROR, "invalid JsonConstructorType %d", ctor->type);
	}

	appendStringInfo(buf, "%s(", funcname);

	is_json_object = ctor->type == JSCTOR_JSON_OBJECT;
	foreach(lc, ctor->args)
	{
		curridx = foreach_current_index(lc);
		if (curridx > 0)
		{
			const char *sep;

			sep = (is_json_object && (curridx % 2) != 0) ? " : " : ", ";
			appendStringInfoString(buf, sep);
		}

		get_rule_expr((Node *) lfirst(lc), context, true);
	}

	get_json_constructor_options(ctor, buf);
	appendStringInfoChar(buf, ')');
}

/*
 * Append options, if any, to the JSON constructor being deparsed
 */
static void
get_json_constructor_options(JsonConstructorExpr *ctor, StringInfo buf)
{
	if (ctor->absent_on_null)
	{
		if (ctor->type == JSCTOR_JSON_OBJECT ||
			ctor->type == JSCTOR_JSON_OBJECTAGG)
			appendStringInfoString(buf, " ABSENT ON NULL");
	}
	else
	{
		if (ctor->type == JSCTOR_JSON_ARRAY ||
			ctor->type == JSCTOR_JSON_ARRAYAGG)
			appendStringInfoString(buf, " NULL ON NULL");
	}

	if (ctor->unique)
		appendStringInfoString(buf, " WITH UNIQUE KEYS");

	/*
	 * Append RETURNING clause if needed; JSON() and JSON_SCALAR() don't
	 * support one.
	 */
	if (ctor->type != JSCTOR_JSON_PARSE && ctor->type != JSCTOR_JSON_SCALAR)
		get_json_returning(ctor->returning, buf, true);
}

/*
 * get_json_agg_constructor - Parse back an aggregate JsonConstructorExpr node
 */
static void
get_json_agg_constructor(JsonConstructorExpr *ctor, deparse_context *context,
						 const char *funcname, bool is_json_objectagg)
{
	StringInfoData options;

	initStringInfo(&options);
	get_json_constructor_options(ctor, &options);

	if (IsA(ctor->func, Aggref))
		get_agg_expr_helper((Aggref *) ctor->func, context,
							(Aggref *) ctor->func,
							funcname, options.data, is_json_objectagg);
	else if (IsA(ctor->func, WindowFunc))
		get_windowfunc_expr_helper((WindowFunc *) ctor->func, context,
								   funcname, options.data,
								   is_json_objectagg);
	else
		elog(ERROR, "invalid JsonConstructorExpr underlying node type: %d",
			 nodeTag(ctor->func));
}

/*
 * simple_quote_literal - Format a string as a SQL literal, append to buf
 */
static void
simple_quote_literal(StringInfo buf, const char *val)
{
	const char *valptr;

	/*
	 * We form the string literal according to the prevailing setting of
	 * standard_conforming_strings; we never use E''. User is responsible for
	 * making sure result is used correctly.
	 */
	appendStringInfoChar(buf, '\'');
	for (valptr = val; *valptr; valptr++)
	{
		char		ch = *valptr;

		if (SQL_STR_DOUBLE(ch, !standard_conforming_strings))
			appendStringInfoChar(buf, ch);
		appendStringInfoChar(buf, ch);
	}
	appendStringInfoChar(buf, '\'');
}


/* ----------
 * get_sublink_expr			- Parse back a sublink
 * ----------
 */
static void
get_sublink_expr(SubLink *sublink, deparse_context *context)
{
	StringInfo	buf = context->buf;
	Query	   *query = (Query *) (sublink->subselect);
	char	   *opname = NULL;
	bool		need_paren;

	if (sublink->subLinkType == ARRAY_SUBLINK)
		appendStringInfoString(buf, "ARRAY(");
	else
		appendStringInfoChar(buf, '(');

	/*
	 * Note that we print the name of only the first operator, when there are
	 * multiple combining operators.  This is an approximation that could go
	 * wrong in various scenarios (operators in different schemas, renamed
	 * operators, etc) but there is not a whole lot we can do about it, since
	 * the syntax allows only one operator to be shown.
	 */
	if (sublink->testexpr)
	{
		if (IsA(sublink->testexpr, OpExpr))
		{
			/* single combining operator */
			OpExpr	   *opexpr = (OpExpr *) sublink->testexpr;

			get_rule_expr(linitial(opexpr->args), context, true);
			opname = generate_operator_name(opexpr->opno,
											exprType(linitial(opexpr->args)),
											exprType(lsecond(opexpr->args)));
		}
		else if (IsA(sublink->testexpr, BoolExpr))
		{
			/* multiple combining operators, = or <> cases */
			char	   *sep;
			ListCell   *l;

			appendStringInfoChar(buf, '(');
			sep = "";
			foreach(l, ((BoolExpr *) sublink->testexpr)->args)
			{
				OpExpr	   *opexpr = lfirst_node(OpExpr, l);

				appendStringInfoString(buf, sep);
				get_rule_expr(linitial(opexpr->args), context, true);
				if (!opname)
					opname = generate_operator_name(opexpr->opno,
													exprType(linitial(opexpr->args)),
													exprType(lsecond(opexpr->args)));
				sep = ", ";
			}
			appendStringInfoChar(buf, ')');
		}
		else if (IsA(sublink->testexpr, RowCompareExpr))
		{
			/* multiple combining operators, < <= > >= cases */
			RowCompareExpr *rcexpr = (RowCompareExpr *) sublink->testexpr;

			appendStringInfoChar(buf, '(');
			get_rule_expr((Node *) rcexpr->largs, context, true);
			opname = generate_operator_name(linitial_oid(rcexpr->opnos),
											exprType(linitial(rcexpr->largs)),
											exprType(linitial(rcexpr->rargs)));
			appendStringInfoChar(buf, ')');
		}
		else
			elog(ERROR, "unrecognized testexpr type: %d",
				 (int) nodeTag(sublink->testexpr));
	}

	need_paren = true;

	switch (sublink->subLinkType)
	{
		case EXISTS_SUBLINK:
			appendStringInfoString(buf, "EXISTS ");
			break;

		case ANY_SUBLINK:
			if (strcmp(opname, "=") == 0)	/* Represent = ANY as IN */
				appendStringInfoString(buf, " IN ");
			else
				appendStringInfo(buf, " %s ANY ", opname);
			break;

		case ALL_SUBLINK:
			appendStringInfo(buf, " %s ALL ", opname);
			break;

		case ROWCOMPARE_SUBLINK:
			appendStringInfo(buf, " %s ", opname);
			break;

		case EXPR_SUBLINK:
		case MULTIEXPR_SUBLINK:
		case ARRAY_SUBLINK:
			need_paren = false;
			break;

		case CTE_SUBLINK:		/* shouldn't occur in a SubLink */
		default:
			elog(ERROR, "unrecognized sublink type: %d",
				 (int) sublink->subLinkType);
			break;
	}

	if (need_paren)
		appendStringInfoChar(buf, '(');

	get_query_def(query, buf, context->namespaces, NULL, false,
				  context->prettyFlags, context->wrapColumn,
				  context->indentLevel);

	if (need_paren)
		appendStringInfoString(buf, "))");
	else
		appendStringInfoChar(buf, ')');
}


/* ----------
 * get_xmltable			- Parse back a XMLTABLE function
 * ----------
 */
static void
get_xmltable(TableFunc *tf, deparse_context *context, bool showimplicit)
{
	StringInfo	buf = context->buf;

	appendStringInfoString(buf, "XMLTABLE(");

	if (tf->ns_uris != NIL)
	{
		ListCell   *lc1,
				   *lc2;
		bool		first = true;

		appendStringInfoString(buf, "XMLNAMESPACES (");
		forboth(lc1, tf->ns_uris, lc2, tf->ns_names)
		{
			Node	   *expr = (Node *) lfirst(lc1);
			String	   *ns_node = lfirst_node(String, lc2);

			if (!first)
				appendStringInfoString(buf, ", ");
			else
				first = false;

			if (ns_node != NULL)
			{
				get_rule_expr(expr, context, showimplicit);
				appendStringInfo(buf, " AS %s",
								 quote_identifier(strVal(ns_node)));
			}
			else
			{
				appendStringInfoString(buf, "DEFAULT ");
				get_rule_expr(expr, context, showimplicit);
			}
		}
		appendStringInfoString(buf, "), ");
	}

	appendStringInfoChar(buf, '(');
	get_rule_expr((Node *) tf->rowexpr, context, showimplicit);
	appendStringInfoString(buf, ") PASSING (");
	get_rule_expr((Node *) tf->docexpr, context, showimplicit);
	appendStringInfoChar(buf, ')');

	if (tf->colexprs != NIL)
	{
		ListCell   *l1;
		ListCell   *l2;
		ListCell   *l3;
		ListCell   *l4;
		ListCell   *l5;
		int			colnum = 0;

		appendStringInfoString(buf, " COLUMNS ");
		forfive(l1, tf->colnames, l2, tf->coltypes, l3, tf->coltypmods,
				l4, tf->colexprs, l5, tf->coldefexprs)
		{
			char	   *colname = strVal(lfirst(l1));
			Oid			typid = lfirst_oid(l2);
			int32		typmod = lfirst_int(l3);
			Node	   *colexpr = (Node *) lfirst(l4);
			Node	   *coldefexpr = (Node *) lfirst(l5);
			bool		ordinality = (tf->ordinalitycol == colnum);
			bool		notnull = bms_is_member(colnum, tf->notnulls);

			if (colnum > 0)
				appendStringInfoString(buf, ", ");
			colnum++;

			appendStringInfo(buf, "%s %s", quote_identifier(colname),
							 ordinality ? "FOR ORDINALITY" :
							 format_type_with_typemod(typid, typmod));
			if (ordinality)
				continue;

			if (coldefexpr != NULL)
			{
				appendStringInfoString(buf, " DEFAULT (");
				get_rule_expr((Node *) coldefexpr, context, showimplicit);
				appendStringInfoChar(buf, ')');
			}
			if (colexpr != NULL)
			{
				appendStringInfoString(buf, " PATH (");
				get_rule_expr((Node *) colexpr, context, showimplicit);
				appendStringInfoChar(buf, ')');
			}
			if (notnull)
				appendStringInfoString(buf, " NOT NULL");
		}
	}

	appendStringInfoChar(buf, ')');
}

/*
 * get_json_table_nested_columns - Parse back nested JSON_TABLE columns
 */
static void
get_json_table_nested_columns(TableFunc *tf, JsonTablePlan *plan,
							  deparse_context *context, bool showimplicit,
							  bool needcomma)
{
	if (IsA(plan, JsonTablePathScan))
	{
		JsonTablePathScan *scan = castNode(JsonTablePathScan, plan);

		if (needcomma)
			appendStringInfoChar(context->buf, ',');

		appendStringInfoChar(context->buf, ' ');
		appendContextKeyword(context, "NESTED PATH ", 0, 0, 0);
		get_const_expr(scan->path->value, context, -1);
		appendStringInfo(context->buf, " AS %s", quote_identifier(scan->path->name));
		get_json_table_columns(tf, scan, context, showimplicit);
	}
	else if (IsA(plan, JsonTableSiblingJoin))
	{
		JsonTableSiblingJoin *join = (JsonTableSiblingJoin *) plan;

		get_json_table_nested_columns(tf, join->lplan, context, showimplicit,
									  needcomma);
		get_json_table_nested_columns(tf, join->rplan, context, showimplicit,
									  true);
	}
}

/*
 * get_json_table_columns - Parse back JSON_TABLE columns
 */
static void
get_json_table_columns(TableFunc *tf, JsonTablePathScan *scan,
					   deparse_context *context,
					   bool showimplicit)
{
	StringInfo	buf = context->buf;
	ListCell   *lc_colname;
	ListCell   *lc_coltype;
	ListCell   *lc_coltypmod;
	ListCell   *lc_colvalexpr;
	int			colnum = 0;

	appendStringInfoChar(buf, ' ');
	appendContextKeyword(context, "COLUMNS (", 0, 0, 0);

	if (PRETTY_INDENT(context))
		context->indentLevel += PRETTYINDENT_VAR;

	forfour(lc_colname, tf->colnames,
			lc_coltype, tf->coltypes,
			lc_coltypmod, tf->coltypmods,
			lc_colvalexpr, tf->colvalexprs)
	{
		char	   *colname = strVal(lfirst(lc_colname));
		JsonExpr   *colexpr;
		Oid			typid;
		int32		typmod;
		bool		ordinality;
		JsonBehaviorType default_behavior;

		typid = lfirst_oid(lc_coltype);
		typmod = lfirst_int(lc_coltypmod);
		colexpr = castNode(JsonExpr, lfirst(lc_colvalexpr));

		/* Skip columns that don't belong to this scan. */
		if (scan->colMin < 0 || colnum < scan->colMin)
		{
			colnum++;
			continue;
		}
		if (colnum > scan->colMax)
			break;

		if (colnum > scan->colMin)
			appendStringInfoString(buf, ", ");

		colnum++;

		ordinality = !colexpr;

		appendContextKeyword(context, "", 0, 0, 0);

		appendStringInfo(buf, "%s %s", quote_identifier(colname),
						 ordinality ? "FOR ORDINALITY" :
						 format_type_with_typemod(typid, typmod));
		if (ordinality)
			continue;

		/*
		 * Set default_behavior to guide get_json_expr_options() on whether to
		 * to emit the ON ERROR / EMPTY clauses.
		 */
		if (colexpr->op == JSON_EXISTS_OP)
		{
			appendStringInfoString(buf, " EXISTS");
			default_behavior = JSON_BEHAVIOR_FALSE;
		}
		else
		{
			if (colexpr->op == JSON_QUERY_OP)
			{
				char		typcategory;
				bool		typispreferred;

				get_type_category_preferred(typid, &typcategory, &typispreferred);

				if (typcategory == TYPCATEGORY_STRING)
					appendStringInfoString(buf,
										   colexpr->format->format_type == JS_FORMAT_JSONB ?
										   " FORMAT JSONB" : " FORMAT JSON");
			}

			default_behavior = JSON_BEHAVIOR_NULL;
		}

		appendStringInfoString(buf, " PATH ");

		get_json_path_spec(colexpr->path_spec, context, showimplicit);

		get_json_expr_options(colexpr, context, default_behavior);
	}

	if (scan->child)
		get_json_table_nested_columns(tf, scan->child, context, showimplicit,
									  scan->colMin >= 0);

	if (PRETTY_INDENT(context))
		context->indentLevel -= PRETTYINDENT_VAR;

	appendContextKeyword(context, ")", 0, 0, 0);
}

/* ----------
 * get_json_table			- Parse back a JSON_TABLE function
 * ----------
 */
static void
get_json_table(TableFunc *tf, deparse_context *context, bool showimplicit)
{
	StringInfo	buf = context->buf;
	JsonExpr   *jexpr = castNode(JsonExpr, tf->docexpr);
	JsonTablePathScan *root = castNode(JsonTablePathScan, tf->plan);

	appendStringInfoString(buf, "JSON_TABLE(");

	if (PRETTY_INDENT(context))
		context->indentLevel += PRETTYINDENT_VAR;

	appendContextKeyword(context, "", 0, 0, 0);

	get_rule_expr(jexpr->formatted_expr, context, showimplicit);

	appendStringInfoString(buf, ", ");

	get_const_expr(root->path->value, context, -1);

	appendStringInfo(buf, " AS %s", quote_identifier(root->path->name));

	if (jexpr->passing_values)
	{
		ListCell   *lc1,
				   *lc2;
		bool		needcomma = false;

		appendStringInfoChar(buf, ' ');
		appendContextKeyword(context, "PASSING ", 0, 0, 0);

		if (PRETTY_INDENT(context))
			context->indentLevel += PRETTYINDENT_VAR;

		forboth(lc1, jexpr->passing_names,
				lc2, jexpr->passing_values)
		{
			if (needcomma)
				appendStringInfoString(buf, ", ");
			needcomma = true;

			appendContextKeyword(context, "", 0, 0, 0);

			get_rule_expr((Node *) lfirst(lc2), context, false);
			appendStringInfo(buf, " AS %s",
							 quote_identifier((lfirst_node(String, lc1))->sval)
				);
		}

		if (PRETTY_INDENT(context))
			context->indentLevel -= PRETTYINDENT_VAR;
	}

	get_json_table_columns(tf, castNode(JsonTablePathScan, tf->plan), context,
						   showimplicit);

	if (jexpr->on_error->btype != JSON_BEHAVIOR_EMPTY_ARRAY)
		get_json_behavior(jexpr->on_error, context, "ERROR");

	if (PRETTY_INDENT(context))
		context->indentLevel -= PRETTYINDENT_VAR;

	appendContextKeyword(context, ")", 0, 0, 0);
}

/* ----------
 * get_tablefunc			- Parse back a table function
 * ----------
 */
static void
get_tablefunc(TableFunc *tf, deparse_context *context, bool showimplicit)
{
	/* XMLTABLE and JSON_TABLE are the only existing implementations.  */

	if (tf->functype == TFT_XMLTABLE)
		get_xmltable(tf, context, showimplicit);
	else if (tf->functype == TFT_JSON_TABLE)
		get_json_table(tf, context, showimplicit);
}

/* ----------
 * get_from_clause			- Parse back a FROM clause
 *
 * "prefix" is the keyword that denotes the start of the list of FROM
 * elements. It is FROM when used to parse back SELECT and UPDATE, but
 * is USING when parsing back DELETE.
 * ----------
 */
static void
get_from_clause(Query *query, const char *prefix, deparse_context *context)
{
	StringInfo	buf = context->buf;
	bool		first = true;
	ListCell   *l;

	/*
	 * We use the query's jointree as a guide to what to print.  However, we
	 * must ignore auto-added RTEs that are marked not inFromCl. (These can
	 * only appear at the top level of the jointree, so it's sufficient to
	 * check here.)  This check also ensures we ignore the rule pseudo-RTEs
	 * for NEW and OLD.
	 */
	foreach(l, query->jointree->fromlist)
	{
		Node	   *jtnode = (Node *) lfirst(l);

		if (IsA(jtnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jtnode)->rtindex;
			RangeTblEntry *rte = rt_fetch(varno, query->rtable);

			if (!rte->inFromCl)
				continue;
		}

		if (first)
		{
			appendContextKeyword(context, prefix,
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 2);
			first = false;

			get_from_clause_item(jtnode, query, context);
		}
		else
		{
			StringInfoData itembuf;

			appendStringInfoString(buf, ", ");

			/*
			 * Put the new FROM item's text into itembuf so we can decide
			 * after we've got it whether or not it needs to go on a new line.
			 */
			initStringInfo(&itembuf);
			context->buf = &itembuf;

			get_from_clause_item(jtnode, query, context);

			/* Restore context's output buffer */
			context->buf = buf;

			/* Consider line-wrapping if enabled */
			if (PRETTY_INDENT(context) && context->wrapColumn >= 0)
			{
				/* Does the new item start with a new line? */
				if (itembuf.len > 0 && itembuf.data[0] == '\n')
				{
					/* If so, we shouldn't add anything */
					/* instead, remove any trailing spaces currently in buf */
					removeStringInfoSpaces(buf);
				}
				else
				{
					char	   *trailing_nl;

					/* Locate the start of the current line in the buffer */
					trailing_nl = strrchr(buf->data, '\n');
					if (trailing_nl == NULL)
						trailing_nl = buf->data;
					else
						trailing_nl++;

					/*
					 * Add a newline, plus some indentation, if the new item
					 * would cause an overflow.
					 */
					if (strlen(trailing_nl) + itembuf.len > context->wrapColumn)
						appendContextKeyword(context, "", -PRETTYINDENT_STD,
											 PRETTYINDENT_STD,
											 PRETTYINDENT_VAR);
				}
			}

			/* Add the new item */
			appendBinaryStringInfo(buf, itembuf.data, itembuf.len);

			/* clean up */
			pfree(itembuf.data);
		}
	}
}

static void
get_from_clause_item(Node *jtnode, Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	deparse_namespace *dpns = (deparse_namespace *) linitial(context->namespaces);

	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, query->rtable);
		deparse_columns *colinfo = deparse_columns_fetch(varno, dpns);
		RangeTblFunction *rtfunc1 = NULL;

		if (rte->lateral)
			appendStringInfoString(buf, "LATERAL ");

		/* Print the FROM item proper */
		switch (rte->rtekind)
		{
			case RTE_RELATION:
				/* Normal relation RTE */
				appendStringInfo(buf, "%s%s",
								 only_marker(rte),
								 generate_relation_name(rte->relid,
														context->namespaces));
				break;
			case RTE_SUBQUERY:
				/* Subquery RTE */
				appendStringInfoChar(buf, '(');
				get_query_def(rte->subquery, buf, context->namespaces, NULL,
							  true,
							  context->prettyFlags, context->wrapColumn,
							  context->indentLevel);
				appendStringInfoChar(buf, ')');
				break;
			case RTE_FUNCTION:
				/* Function RTE */
				rtfunc1 = (RangeTblFunction *) linitial(rte->functions);

				/*
				 * Omit ROWS FROM() syntax for just one function, unless it
				 * has both a coldeflist and WITH ORDINALITY. If it has both,
				 * we must use ROWS FROM() syntax to avoid ambiguity about
				 * whether the coldeflist includes the ordinality column.
				 */
				if (list_length(rte->functions) == 1 &&
					(rtfunc1->funccolnames == NIL || !rte->funcordinality))
				{
					get_rule_expr_funccall(rtfunc1->funcexpr, context, true);
					/* we'll print the coldeflist below, if it has one */
				}
				else
				{
					bool		all_unnest;
					ListCell   *lc;

					/*
					 * If all the function calls in the list are to unnest,
					 * and none need a coldeflist, then collapse the list back
					 * down to UNNEST(args).  (If we had more than one
					 * built-in unnest function, this would get more
					 * difficult.)
					 *
					 * XXX This is pretty ugly, since it makes not-terribly-
					 * future-proof assumptions about what the parser would do
					 * with the output; but the alternative is to emit our
					 * nonstandard ROWS FROM() notation for what might have
					 * been a perfectly spec-compliant multi-argument
					 * UNNEST().
					 */
					all_unnest = true;
					foreach(lc, rte->functions)
					{
						RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

						if (!IsA(rtfunc->funcexpr, FuncExpr) ||
							((FuncExpr *) rtfunc->funcexpr)->funcid != F_UNNEST_ANYARRAY ||
							rtfunc->funccolnames != NIL)
						{
							all_unnest = false;
							break;
						}
					}

					if (all_unnest)
					{
						List	   *allargs = NIL;

						foreach(lc, rte->functions)
						{
							RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);
							List	   *args = ((FuncExpr *) rtfunc->funcexpr)->args;

							allargs = list_concat(allargs, args);
						}

						appendStringInfoString(buf, "UNNEST(");
						get_rule_expr((Node *) allargs, context, true);
						appendStringInfoChar(buf, ')');
					}
					else
					{
						int			funcno = 0;

						appendStringInfoString(buf, "ROWS FROM(");
						foreach(lc, rte->functions)
						{
							RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

							if (funcno > 0)
								appendStringInfoString(buf, ", ");
							get_rule_expr_funccall(rtfunc->funcexpr, context, true);
							if (rtfunc->funccolnames != NIL)
							{
								/* Reconstruct the column definition list */
								appendStringInfoString(buf, " AS ");
								get_from_clause_coldeflist(rtfunc,
														   NULL,
														   context);
							}
							funcno++;
						}
						appendStringInfoChar(buf, ')');
					}
					/* prevent printing duplicate coldeflist below */
					rtfunc1 = NULL;
				}
				if (rte->funcordinality)
					appendStringInfoString(buf, " WITH ORDINALITY");
				break;
			case RTE_TABLEFUNC:
				get_tablefunc(rte->tablefunc, context, true);
				break;
			case RTE_VALUES:
				/* Values list RTE */
				appendStringInfoChar(buf, '(');
				get_values_def(rte->values_lists, context);
				appendStringInfoChar(buf, ')');
				break;
			case RTE_CTE:
				appendStringInfoString(buf, quote_identifier(rte->ctename));
				break;
			default:
				elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
				break;
		}

		/* Print the relation alias, if needed */
		get_rte_alias(rte, varno, false, context);

		/* Print the column definitions or aliases, if needed */
		if (rtfunc1 && rtfunc1->funccolnames != NIL)
		{
			/* Reconstruct the columndef list, which is also the aliases */
			get_from_clause_coldeflist(rtfunc1, colinfo, context);
		}
		else
		{
			/* Else print column aliases as needed */
			get_column_alias_list(colinfo, context);
		}

		/* Tablesample clause must go after any alias */
		if (rte->rtekind == RTE_RELATION && rte->tablesample)
			get_tablesample_def(rte->tablesample, context);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		deparse_columns *colinfo = deparse_columns_fetch(j->rtindex, dpns);
		bool		need_paren_on_right;

		need_paren_on_right = PRETTY_PAREN(context) &&
			!IsA(j->rarg, RangeTblRef) &&
			!(IsA(j->rarg, JoinExpr) && ((JoinExpr *) j->rarg)->alias != NULL);

		if (!PRETTY_PAREN(context) || j->alias != NULL)
			appendStringInfoChar(buf, '(');

		get_from_clause_item(j->larg, query, context);

		switch (j->jointype)
		{
			case JOIN_INNER:
				if (j->quals)
					appendContextKeyword(context, " JOIN ",
										 -PRETTYINDENT_STD,
										 PRETTYINDENT_STD,
										 PRETTYINDENT_JOIN);
				else
					appendContextKeyword(context, " CROSS JOIN ",
										 -PRETTYINDENT_STD,
										 PRETTYINDENT_STD,
										 PRETTYINDENT_JOIN);
				break;
			case JOIN_LEFT:
				appendContextKeyword(context, " LEFT JOIN ",
									 -PRETTYINDENT_STD,
									 PRETTYINDENT_STD,
									 PRETTYINDENT_JOIN);
				break;
			case JOIN_FULL:
				appendContextKeyword(context, " FULL JOIN ",
									 -PRETTYINDENT_STD,
									 PRETTYINDENT_STD,
									 PRETTYINDENT_JOIN);
				break;
			case JOIN_RIGHT:
				appendContextKeyword(context, " RIGHT JOIN ",
									 -PRETTYINDENT_STD,
									 PRETTYINDENT_STD,
									 PRETTYINDENT_JOIN);
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
		}

		if (need_paren_on_right)
			appendStringInfoChar(buf, '(');
		get_from_clause_item(j->rarg, query, context);
		if (need_paren_on_right)
			appendStringInfoChar(buf, ')');

		if (j->usingClause)
		{
			ListCell   *lc;
			bool		first = true;

			appendStringInfoString(buf, " USING (");
			/* Use the assigned names, not what's in usingClause */
			foreach(lc, colinfo->usingNames)
			{
				char	   *colname = (char *) lfirst(lc);

				if (first)
					first = false;
				else
					appendStringInfoString(buf, ", ");
				appendStringInfoString(buf, quote_identifier(colname));
			}
			appendStringInfoChar(buf, ')');

			if (j->join_using_alias)
				appendStringInfo(buf, " AS %s",
								 quote_identifier(j->join_using_alias->aliasname));
		}
		else if (j->quals)
		{
			appendStringInfoString(buf, " ON ");
			if (!PRETTY_PAREN(context))
				appendStringInfoChar(buf, '(');
			get_rule_expr(j->quals, context, false);
			if (!PRETTY_PAREN(context))
				appendStringInfoChar(buf, ')');
		}
		else if (j->jointype != JOIN_INNER)
		{
			/* If we didn't say CROSS JOIN above, we must provide an ON */
			appendStringInfoString(buf, " ON TRUE");
		}

		if (!PRETTY_PAREN(context) || j->alias != NULL)
			appendStringInfoChar(buf, ')');

		/* Yes, it's correct to put alias after the right paren ... */
		if (j->alias != NULL)
		{
			/*
			 * Note that it's correct to emit an alias clause if and only if
			 * there was one originally.  Otherwise we'd be converting a named
			 * join to unnamed or vice versa, which creates semantic
			 * subtleties we don't want.  However, we might print a different
			 * alias name than was there originally.
			 */
			appendStringInfo(buf, " %s",
							 quote_identifier(get_rtable_name(j->rtindex,
															  context)));
			get_column_alias_list(colinfo, context);
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * get_rte_alias - print the relation's alias, if needed
 *
 * If printed, the alias is preceded by a space, or by " AS " if use_as is true.
 */
static void
get_rte_alias(RangeTblEntry *rte, int varno, bool use_as,
			  deparse_context *context)
{
	deparse_namespace *dpns = (deparse_namespace *) linitial(context->namespaces);
	char	   *refname = get_rtable_name(varno, context);
	deparse_columns *colinfo = deparse_columns_fetch(varno, dpns);
	bool		printalias = false;

	if (rte->alias != NULL)
	{
		/* Always print alias if user provided one */
		printalias = true;
	}
	else if (colinfo->printaliases)
	{
		/* Always print alias if we need to print column aliases */
		printalias = true;
	}
	else if (rte->rtekind == RTE_RELATION)
	{
		/*
		 * No need to print alias if it's same as relation name (this would
		 * normally be the case, but not if set_rtable_names had to resolve a
		 * conflict).
		 */
		if (strcmp(refname, get_relation_name(rte->relid)) != 0)
			printalias = true;
	}
	else if (rte->rtekind == RTE_FUNCTION)
	{
		/*
		 * For a function RTE, always print alias.  This covers possible
		 * renaming of the function and/or instability of the FigureColname
		 * rules for things that aren't simple functions.  Note we'd need to
		 * force it anyway for the columndef list case.
		 */
		printalias = true;
	}
	else if (rte->rtekind == RTE_SUBQUERY ||
			 rte->rtekind == RTE_VALUES)
	{
		/*
		 * For a subquery, always print alias.  This makes the output
		 * SQL-spec-compliant, even though we allow such aliases to be omitted
		 * on input.
		 */
		printalias = true;
	}
	else if (rte->rtekind == RTE_CTE)
	{
		/*
		 * No need to print alias if it's same as CTE name (this would
		 * normally be the case, but not if set_rtable_names had to resolve a
		 * conflict).
		 */
		if (strcmp(refname, rte->ctename) != 0)
			printalias = true;
	}

	if (printalias)
		appendStringInfo(context->buf, "%s%s",
						 use_as ? " AS " : " ",
						 quote_identifier(refname));
}

/*
 * get_column_alias_list - print column alias list for an RTE
 *
 * Caller must already have printed the relation's alias name.
 */
static void
get_column_alias_list(deparse_columns *colinfo, deparse_context *context)
{
	StringInfo	buf = context->buf;
	int			i;
	bool		first = true;

	/* Don't print aliases if not needed */
	if (!colinfo->printaliases)
		return;

	for (i = 0; i < colinfo->num_new_cols; i++)
	{
		char	   *colname = colinfo->new_colnames[i];

		if (first)
		{
			appendStringInfoChar(buf, '(');
			first = false;
		}
		else
			appendStringInfoString(buf, ", ");
		appendStringInfoString(buf, quote_identifier(colname));
	}
	if (!first)
		appendStringInfoChar(buf, ')');
}

/*
 * get_from_clause_coldeflist - reproduce FROM clause coldeflist
 *
 * When printing a top-level coldeflist (which is syntactically also the
 * relation's column alias list), use column names from colinfo.  But when
 * printing a coldeflist embedded inside ROWS FROM(), we prefer to use the
 * original coldeflist's names, which are available in rtfunc->funccolnames.
 * Pass NULL for colinfo to select the latter behavior.
 *
 * The coldeflist is appended immediately (no space) to buf.  Caller is
 * responsible for ensuring that an alias or AS is present before it.
 */
static void
get_from_clause_coldeflist(RangeTblFunction *rtfunc,
						   deparse_columns *colinfo,
						   deparse_context *context)
{
	StringInfo	buf = context->buf;
	ListCell   *l1;
	ListCell   *l2;
	ListCell   *l3;
	ListCell   *l4;
	int			i;

	appendStringInfoChar(buf, '(');

	i = 0;
	forfour(l1, rtfunc->funccoltypes,
			l2, rtfunc->funccoltypmods,
			l3, rtfunc->funccolcollations,
			l4, rtfunc->funccolnames)
	{
		Oid			atttypid = lfirst_oid(l1);
		int32		atttypmod = lfirst_int(l2);
		Oid			attcollation = lfirst_oid(l3);
		char	   *attname;

		if (colinfo)
			attname = colinfo->colnames[i];
		else
			attname = strVal(lfirst(l4));

		Assert(attname);		/* shouldn't be any dropped columns here */

		if (i > 0)
			appendStringInfoString(buf, ", ");
		appendStringInfo(buf, "%s %s",
						 quote_identifier(attname),
						 format_type_with_typemod(atttypid, atttypmod));
		if (OidIsValid(attcollation) &&
			attcollation != get_typcollation(atttypid))
			appendStringInfo(buf, " COLLATE %s",
							 generate_collation_name(attcollation));

		i++;
	}

	appendStringInfoChar(buf, ')');
}

/*
 * get_tablesample_def			- print a TableSampleClause
 */
static void
get_tablesample_def(TableSampleClause *tablesample, deparse_context *context)
{
	StringInfo	buf = context->buf;
	Oid			argtypes[1];
	int			nargs;
	ListCell   *l;

	/*
	 * We should qualify the handler's function name if it wouldn't be
	 * resolved by lookup in the current search path.
	 */
	argtypes[0] = INTERNALOID;
	appendStringInfo(buf, " TABLESAMPLE %s (",
					 generate_function_name(tablesample->tsmhandler, 1,
											NIL, argtypes,
											false, NULL, false));

	nargs = 0;
	foreach(l, tablesample->args)
	{
		if (nargs++ > 0)
			appendStringInfoString(buf, ", ");
		get_rule_expr((Node *) lfirst(l), context, false);
	}
	appendStringInfoChar(buf, ')');

	if (tablesample->repeatable != NULL)
	{
		appendStringInfoString(buf, " REPEATABLE (");
		get_rule_expr((Node *) tablesample->repeatable, context, false);
		appendStringInfoChar(buf, ')');
	}
}

/*
 * get_opclass_name			- fetch name of an index operator class
 *
 * The opclass name is appended (after a space) to buf.
 *
 * Output is suppressed if the opclass is the default for the given
 * actual_datatype.  (If you don't want this behavior, just pass
 * InvalidOid for actual_datatype.)
 */
static void
get_opclass_name(Oid opclass, Oid actual_datatype,
				 StringInfo buf)
{
	HeapTuple	ht_opc;
	Form_pg_opclass opcrec;
	char	   *opcname;
	char	   *nspname;

	ht_opc = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
	if (!HeapTupleIsValid(ht_opc))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	opcrec = (Form_pg_opclass) GETSTRUCT(ht_opc);

	if (!OidIsValid(actual_datatype) ||
		GetDefaultOpClass(actual_datatype, opcrec->opcmethod) != opclass)
	{
		/* Okay, we need the opclass name.  Do we need to qualify it? */
		opcname = NameStr(opcrec->opcname);
		if (OpclassIsVisible(opclass))
			appendStringInfo(buf, " %s", quote_identifier(opcname));
		else
		{
			nspname = get_namespace_name_or_temp(opcrec->opcnamespace);
			appendStringInfo(buf, " %s.%s",
							 quote_identifier(nspname),
							 quote_identifier(opcname));
		}
	}
	ReleaseSysCache(ht_opc);
}

/*
 * generate_opclass_name
 *		Compute the name to display for an opclass specified by OID
 *
 * The result includes all necessary quoting and schema-prefixing.
 */
char *
generate_opclass_name(Oid opclass)
{
	StringInfoData buf;

	initStringInfo(&buf);
	get_opclass_name(opclass, InvalidOid, &buf);

	return &buf.data[1];		/* get_opclass_name() prepends space */
}

/*
 * processIndirection - take care of array and subfield assignment
 *
 * We strip any top-level FieldStore or assignment SubscriptingRef nodes that
 * appear in the input, printing them as decoration for the base column
 * name (which we assume the caller just printed).  We might also need to
 * strip CoerceToDomain nodes, but only ones that appear above assignment
 * nodes.
 *
 * Returns the subexpression that's to be assigned.
 */
static Node *
processIndirection(Node *node, deparse_context *context)
{
	StringInfo	buf = context->buf;
	CoerceToDomain *cdomain = NULL;

	for (;;)
	{
		if (node == NULL)
			break;
		if (IsA(node, FieldStore))
		{
			FieldStore *fstore = (FieldStore *) node;
			Oid			typrelid;
			char	   *fieldname;

			/* lookup tuple type */
			typrelid = get_typ_typrelid(fstore->resulttype);
			if (!OidIsValid(typrelid))
				elog(ERROR, "argument type %s of FieldStore is not a tuple type",
					 format_type_be(fstore->resulttype));

			/*
			 * Print the field name.  There should only be one target field in
			 * stored rules.  There could be more than that in executable
			 * target lists, but this function cannot be used for that case.
			 */
			Assert(list_length(fstore->fieldnums) == 1);
			fieldname = get_attname(typrelid,
									linitial_int(fstore->fieldnums), false);
			appendStringInfo(buf, ".%s", quote_identifier(fieldname));

			/*
			 * We ignore arg since it should be an uninteresting reference to
			 * the target column or subcolumn.
			 */
			node = (Node *) linitial(fstore->newvals);
		}
		else if (IsA(node, SubscriptingRef))
		{
			SubscriptingRef *sbsref = (SubscriptingRef *) node;

			if (sbsref->refassgnexpr == NULL)
				break;

			printSubscripts(sbsref, context);

			/*
			 * We ignore refexpr since it should be an uninteresting reference
			 * to the target column or subcolumn.
			 */
			node = (Node *) sbsref->refassgnexpr;
		}
		else if (IsA(node, CoerceToDomain))
		{
			cdomain = (CoerceToDomain *) node;
			/* If it's an explicit domain coercion, we're done */
			if (cdomain->coercionformat != COERCE_IMPLICIT_CAST)
				break;
			/* Tentatively descend past the CoerceToDomain */
			node = (Node *) cdomain->arg;
		}
		else
			break;
	}

	/*
	 * If we descended past a CoerceToDomain whose argument turned out not to
	 * be a FieldStore or array assignment, back up to the CoerceToDomain.
	 * (This is not enough to be fully correct if there are nested implicit
	 * CoerceToDomains, but such cases shouldn't ever occur.)
	 */
	if (cdomain && node == (Node *) cdomain->arg)
		node = (Node *) cdomain;

	return node;
}

static void
printSubscripts(SubscriptingRef *sbsref, deparse_context *context)
{
	StringInfo	buf = context->buf;
	ListCell   *lowlist_item;
	ListCell   *uplist_item;

	lowlist_item = list_head(sbsref->reflowerindexpr);	/* could be NULL */
	foreach(uplist_item, sbsref->refupperindexpr)
	{
		appendStringInfoChar(buf, '[');
		if (lowlist_item)
		{
			/* If subexpression is NULL, get_rule_expr prints nothing */
			get_rule_expr((Node *) lfirst(lowlist_item), context, false);
			appendStringInfoChar(buf, ':');
			lowlist_item = lnext(sbsref->reflowerindexpr, lowlist_item);
		}
		/* If subexpression is NULL, get_rule_expr prints nothing */
		get_rule_expr((Node *) lfirst(uplist_item), context, false);
		appendStringInfoChar(buf, ']');
	}
}

/*
 * quote_identifier			- Quote an identifier only if needed
 *
 * When quotes are needed, we palloc the required space; slightly
 * space-wasteful but well worth it for notational simplicity.
 */
const char *
quote_identifier(const char *ident)
{
	/*
	 * Can avoid quoting if ident starts with a lowercase letter or underscore
	 * and contains only lowercase letters, digits, and underscores, *and* is
	 * not any SQL keyword.  Otherwise, supply quotes.
	 */
	int			nquotes = 0;
	bool		safe;
	const char *ptr;
	char	   *result;
	char	   *optr;

	/*
	 * would like to use <ctype.h> macros here, but they might yield unwanted
	 * locale-specific results...
	 */
	safe = ((ident[0] >= 'a' && ident[0] <= 'z') || ident[0] == '_');

	for (ptr = ident; *ptr; ptr++)
	{
		char		ch = *ptr;

		if ((ch >= 'a' && ch <= 'z') ||
			(ch >= '0' && ch <= '9') ||
			(ch == '_'))
		{
			/* okay */
		}
		else
		{
			safe = false;
			if (ch == '"')
				nquotes++;
		}
	}

	if (quote_all_identifiers)
		safe = false;

	if (safe)
	{
		/*
		 * Check for keyword.  We quote keywords except for unreserved ones.
		 * (In some cases we could avoid quoting a col_name or type_func_name
		 * keyword, but it seems much harder than it's worth to tell that.)
		 *
		 * Note: ScanKeywordLookup() does case-insensitive comparison, but
		 * that's fine, since we already know we have all-lower-case.
		 */
		int			kwnum = ScanKeywordLookup(ident, &ScanKeywords);

		if (kwnum >= 0 && ScanKeywordCategories[kwnum] != UNRESERVED_KEYWORD)
			safe = false;
	}

	if (safe)
		return ident;			/* no change needed */

	result = (char *) palloc(strlen(ident) + nquotes + 2 + 1);

	optr = result;
	*optr++ = '"';
	for (ptr = ident; *ptr; ptr++)
	{
		char		ch = *ptr;

		if (ch == '"')
			*optr++ = '"';
		*optr++ = ch;
	}
	*optr++ = '"';
	*optr = '\0';

	return result;
}

/*
 * quote_qualified_identifier	- Quote a possibly-qualified identifier
 *
 * Return a name of the form qualifier.ident, or just ident if qualifier
 * is NULL, quoting each component if necessary.  The result is palloc'd.
 */
char *
quote_qualified_identifier(const char *qualifier,
						   const char *ident)
{
	StringInfoData buf;

	initStringInfo(&buf);
	if (qualifier)
		appendStringInfo(&buf, "%s.", quote_identifier(qualifier));
	appendStringInfoString(&buf, quote_identifier(ident));
	return buf.data;
}

/*
 * get_relation_name
 *		Get the unqualified name of a relation specified by OID
 *
 * This differs from the underlying get_rel_name() function in that it will
 * throw error instead of silently returning NULL if the OID is bad.
 */
static char *
get_relation_name(Oid relid)
{
	char	   *relname = get_rel_name(relid);

	if (!relname)
		elog(ERROR, "cache lookup failed for relation %u", relid);
	return relname;
}

/*
 * generate_relation_name
 *		Compute the name to display for a relation specified by OID
 *
 * The result includes all necessary quoting and schema-prefixing.
 *
 * If namespaces isn't NIL, it must be a list of deparse_namespace nodes.
 * We will forcibly qualify the relation name if it equals any CTE name
 * visible in the namespace list.
 */
static char *
generate_relation_name(Oid relid, List *namespaces)
{
	HeapTuple	tp;
	Form_pg_class reltup;
	bool		need_qual;
	ListCell   *nslist;
	char	   *relname;
	char	   *nspname;
	char	   *result;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	reltup = (Form_pg_class) GETSTRUCT(tp);
	relname = NameStr(reltup->relname);

	/* Check for conflicting CTE name */
	need_qual = false;
	foreach(nslist, namespaces)
	{
		deparse_namespace *dpns = (deparse_namespace *) lfirst(nslist);
		ListCell   *ctlist;

		foreach(ctlist, dpns->ctes)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(ctlist);

			if (strcmp(cte->ctename, relname) == 0)
			{
				need_qual = true;
				break;
			}
		}
		if (need_qual)
			break;
	}

	/* Otherwise, qualify the name if not visible in search path */
	if (!need_qual)
		need_qual = !RelationIsVisible(relid);

	if (need_qual)
		nspname = get_namespace_name_or_temp(reltup->relnamespace);
	else
		nspname = NULL;

	result = quote_qualified_identifier(nspname, relname);

	ReleaseSysCache(tp);

	return result;
}

/*
 * generate_qualified_relation_name
 *		Compute the name to display for a relation specified by OID
 *
 * As above, but unconditionally schema-qualify the name.
 */
static char *
generate_qualified_relation_name(Oid relid)
{
	HeapTuple	tp;
	Form_pg_class reltup;
	char	   *relname;
	char	   *nspname;
	char	   *result;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	reltup = (Form_pg_class) GETSTRUCT(tp);
	relname = NameStr(reltup->relname);

	nspname = get_namespace_name_or_temp(reltup->relnamespace);
	if (!nspname)
		elog(ERROR, "cache lookup failed for namespace %u",
			 reltup->relnamespace);

	result = quote_qualified_identifier(nspname, relname);

	ReleaseSysCache(tp);

	return result;
}

/*
 * generate_function_name
 *		Compute the name to display for a function specified by OID,
 *		given that it is being called with the specified actual arg names and
 *		types.  (Those matter because of ambiguous-function resolution rules.)
 *
 * If we're dealing with a potentially variadic function (in practice, this
 * means a FuncExpr or Aggref, not some other way of calling a function), then
 * has_variadic must specify whether variadic arguments have been merged,
 * and *use_variadic_p will be set to indicate whether to print VARIADIC in
 * the output.  For non-FuncExpr cases, has_variadic should be false and
 * use_variadic_p can be NULL.
 *
 * inGroupBy must be true if we're deparsing a GROUP BY clause.
 *
 * The result includes all necessary quoting and schema-prefixing.
 */
static char *
generate_function_name(Oid funcid, int nargs, List *argnames, Oid *argtypes,
					   bool has_variadic, bool *use_variadic_p,
					   bool inGroupBy)
{
	char	   *result;
	HeapTuple	proctup;
	Form_pg_proc procform;
	char	   *proname;
	bool		use_variadic;
	char	   *nspname;
	FuncDetailCode p_result;
	Oid			p_funcid;
	Oid			p_rettype;
	bool		p_retset;
	int			p_nvargs;
	Oid			p_vatype;
	Oid		   *p_true_typeids;
	bool		force_qualify = false;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);
	proname = NameStr(procform->proname);

	/*
	 * Due to parser hacks to avoid needing to reserve CUBE, we need to force
	 * qualification of some function names within GROUP BY.
	 */
	if (inGroupBy)
	{
		if (strcmp(proname, "cube") == 0 || strcmp(proname, "rollup") == 0)
			force_qualify = true;
	}

	/*
	 * Determine whether VARIADIC should be printed.  We must do this first
	 * since it affects the lookup rules in func_get_detail().
	 *
	 * We always print VARIADIC if the function has a merged variadic-array
	 * argument.  Note that this is always the case for functions taking a
	 * VARIADIC argument type other than VARIADIC ANY.  If we omitted VARIADIC
	 * and printed the array elements as separate arguments, the call could
	 * match a newer non-VARIADIC function.
	 */
	if (use_variadic_p)
	{
		/* Parser should not have set funcvariadic unless fn is variadic */
		Assert(!has_variadic || OidIsValid(procform->provariadic));
		use_variadic = has_variadic;
		*use_variadic_p = use_variadic;
	}
	else
	{
		Assert(!has_variadic);
		use_variadic = false;
	}

	/*
	 * The idea here is to schema-qualify only if the parser would fail to
	 * resolve the correct function given the unqualified func name with the
	 * specified argtypes and VARIADIC flag.  But if we already decided to
	 * force qualification, then we can skip the lookup and pretend we didn't
	 * find it.
	 */
	if (!force_qualify)
		p_result = func_get_detail(list_make1(makeString(proname)),
								   NIL, argnames, nargs, argtypes,
								   !use_variadic, true, false,
								   &p_funcid, &p_rettype,
								   &p_retset, &p_nvargs, &p_vatype,
								   &p_true_typeids, NULL);
	else
	{
		p_result = FUNCDETAIL_NOTFOUND;
		p_funcid = InvalidOid;
	}

	if ((p_result == FUNCDETAIL_NORMAL ||
		 p_result == FUNCDETAIL_AGGREGATE ||
		 p_result == FUNCDETAIL_WINDOWFUNC) &&
		p_funcid == funcid)
		nspname = NULL;
	else
		nspname = get_namespace_name_or_temp(procform->pronamespace);

	result = quote_qualified_identifier(nspname, proname);

	ReleaseSysCache(proctup);

	return result;
}

/*
 * generate_operator_name
 *		Compute the name to display for an operator specified by OID,
 *		given that it is being called with the specified actual arg types.
 *		(Arg types matter because of ambiguous-operator resolution rules.
 *		Pass InvalidOid for unused arg of a unary operator.)
 *
 * The result includes all necessary quoting and schema-prefixing,
 * plus the OPERATOR() decoration needed to use a qualified operator name
 * in an expression.
 */
static char *
generate_operator_name(Oid operid, Oid arg1, Oid arg2)
{
	StringInfoData buf;
	HeapTuple	opertup;
	Form_pg_operator operform;
	char	   *oprname;
	char	   *nspname;
	Operator	p_result;

	initStringInfo(&buf);

	opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operid));
	if (!HeapTupleIsValid(opertup))
		elog(ERROR, "cache lookup failed for operator %u", operid);
	operform = (Form_pg_operator) GETSTRUCT(opertup);
	oprname = NameStr(operform->oprname);

	/*
	 * The idea here is to schema-qualify only if the parser would fail to
	 * resolve the correct operator given the unqualified op name with the
	 * specified argtypes.
	 */
	switch (operform->oprkind)
	{
		case 'b':
			p_result = oper(NULL, list_make1(makeString(oprname)), arg1, arg2,
							true, -1);
			break;
		case 'l':
			p_result = left_oper(NULL, list_make1(makeString(oprname)), arg2,
								 true, -1);
			break;
		default:
			elog(ERROR, "unrecognized oprkind: %d", operform->oprkind);
			p_result = NULL;	/* keep compiler quiet */
			break;
	}

	if (p_result != NULL && oprid(p_result) == operid)
		nspname = NULL;
	else
	{
		nspname = get_namespace_name_or_temp(operform->oprnamespace);
		appendStringInfo(&buf, "OPERATOR(%s.", quote_identifier(nspname));
	}

	appendStringInfoString(&buf, oprname);

	if (nspname)
		appendStringInfoChar(&buf, ')');

	if (p_result != NULL)
		ReleaseSysCache(p_result);

	ReleaseSysCache(opertup);

	return buf.data;
}

/*
 * generate_operator_clause --- generate a binary-operator WHERE clause
 *
 * This is used for internally-generated-and-executed SQL queries, where
 * precision is essential and readability is secondary.  The basic
 * requirement is to append "leftop op rightop" to buf, where leftop and
 * rightop are given as strings and are assumed to yield types leftoptype
 * and rightoptype; the operator is identified by OID.  The complexity
 * comes from needing to be sure that the parser will select the desired
 * operator when the query is parsed.  We always name the operator using
 * OPERATOR(schema.op) syntax, so as to avoid search-path uncertainties.
 * We have to emit casts too, if either input isn't already the input type
 * of the operator; else we are at the mercy of the parser's heuristics for
 * ambiguous-operator resolution.  The caller must ensure that leftop and
 * rightop are suitable arguments for a cast operation; it's best to insert
 * parentheses if they aren't just variables or parameters.
 */
void
generate_operator_clause(StringInfo buf,
						 const char *leftop, Oid leftoptype,
						 Oid opoid,
						 const char *rightop, Oid rightoptype)
{
	HeapTuple	opertup;
	Form_pg_operator operform;
	char	   *oprname;
	char	   *nspname;

	opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(opoid));
	if (!HeapTupleIsValid(opertup))
		elog(ERROR, "cache lookup failed for operator %u", opoid);
	operform = (Form_pg_operator) GETSTRUCT(opertup);
	Assert(operform->oprkind == 'b');
	oprname = NameStr(operform->oprname);

	nspname = get_namespace_name(operform->oprnamespace);

	appendStringInfoString(buf, leftop);
	if (leftoptype != operform->oprleft)
		add_cast_to(buf, operform->oprleft);
	appendStringInfo(buf, " OPERATOR(%s.", quote_identifier(nspname));
	appendStringInfoString(buf, oprname);
	appendStringInfo(buf, ") %s", rightop);
	if (rightoptype != operform->oprright)
		add_cast_to(buf, operform->oprright);

	ReleaseSysCache(opertup);
}

/*
 * Add a cast specification to buf.  We spell out the type name the hard way,
 * intentionally not using format_type_be().  This is to avoid corner cases
 * for CHARACTER, BIT, and perhaps other types, where specifying the type
 * using SQL-standard syntax results in undesirable data truncation.  By
 * doing it this way we can be certain that the cast will have default (-1)
 * target typmod.
 */
static void
add_cast_to(StringInfo buf, Oid typid)
{
	HeapTuple	typetup;
	Form_pg_type typform;
	char	   *typname;
	char	   *nspname;

	typetup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typform = (Form_pg_type) GETSTRUCT(typetup);

	typname = NameStr(typform->typname);
	nspname = get_namespace_name_or_temp(typform->typnamespace);

	appendStringInfo(buf, "::%s.%s",
					 quote_identifier(nspname), quote_identifier(typname));

	ReleaseSysCache(typetup);
}

/*
 * generate_qualified_type_name
 *		Compute the name to display for a type specified by OID
 *
 * This is different from format_type_be() in that we unconditionally
 * schema-qualify the name.  That also means no special syntax for
 * SQL-standard type names ... although in current usage, this should
 * only get used for domains, so such cases wouldn't occur anyway.
 */
static char *
generate_qualified_type_name(Oid typid)
{
	HeapTuple	tp;
	Form_pg_type typtup;
	char	   *typname;
	char	   *nspname;
	char	   *result;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	typname = NameStr(typtup->typname);

	nspname = get_namespace_name_or_temp(typtup->typnamespace);
	if (!nspname)
		elog(ERROR, "cache lookup failed for namespace %u",
			 typtup->typnamespace);

	result = quote_qualified_identifier(nspname, typname);

	ReleaseSysCache(tp);

	return result;
}

/*
 * generate_collation_name
 *		Compute the name to display for a collation specified by OID
 *
 * The result includes all necessary quoting and schema-prefixing.
 */
char *
generate_collation_name(Oid collid)
{
	HeapTuple	tp;
	Form_pg_collation colltup;
	char	   *collname;
	char	   *nspname;
	char	   *result;

	tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for collation %u", collid);
	colltup = (Form_pg_collation) GETSTRUCT(tp);
	collname = NameStr(colltup->collname);

	if (!CollationIsVisible(collid))
		nspname = get_namespace_name_or_temp(colltup->collnamespace);
	else
		nspname = NULL;

	result = quote_qualified_identifier(nspname, collname);

	ReleaseSysCache(tp);

	return result;
}

/*
 * Given a C string, produce a TEXT datum.
 *
 * We assume that the input was palloc'd and may be freed.
 */
static text *
string_to_text(char *str)
{
	text	   *result;

	result = cstring_to_text(str);
	pfree(str);
	return result;
}

/*
 * Generate a C string representing a relation options from text[] datum.
 */
static void
get_reloptions(StringInfo buf, Datum reloptions)
{
	Datum	   *options;
	int			noptions;
	int			i;

	deconstruct_array_builtin(DatumGetArrayTypeP(reloptions), TEXTOID,
							  &options, NULL, &noptions);

	for (i = 0; i < noptions; i++)
	{
		char	   *option = TextDatumGetCString(options[i]);
		char	   *name;
		char	   *separator;
		char	   *value;

		/*
		 * Each array element should have the form name=value.  If the "=" is
		 * missing for some reason, treat it like an empty value.
		 */
		name = option;
		separator = strchr(option, '=');
		if (separator)
		{
			*separator = '\0';
			value = separator + 1;
		}
		else
			value = "";

		if (i > 0)
			appendStringInfoString(buf, ", ");
		appendStringInfo(buf, "%s=", quote_identifier(name));

		/*
		 * In general we need to quote the value; but to avoid unnecessary
		 * clutter, do not quote if it is an identifier that would not need
		 * quoting.  (We could also allow numbers, but that is a bit trickier
		 * than it looks --- for example, are leading zeroes significant?  We
		 * don't want to assume very much here about what custom reloptions
		 * might mean.)
		 */
		if (quote_identifier(value) == value)
			appendStringInfoString(buf, value);
		else
			simple_quote_literal(buf, value);

		pfree(option);
	}
}

/*
 * Generate a C string representing a relation's reloptions, or NULL if none.
 */
static char *
flatten_reloptions(Oid relid)
{
	char	   *result = NULL;
	HeapTuple	tuple;
	Datum		reloptions;
	bool		isnull;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	reloptions = SysCacheGetAttr(RELOID, tuple,
								 Anum_pg_class_reloptions, &isnull);
	if (!isnull)
	{
		StringInfoData buf;

		initStringInfo(&buf);
		get_reloptions(&buf, reloptions);

		result = buf.data;
	}

	ReleaseSysCache(tuple);

	return result;
}

/*
 * get_range_partbound_string
 *		A C string representation of one range partition bound
 */
char *
get_range_partbound_string(List *bound_datums)
{
	deparse_context context;
	StringInfo	buf = makeStringInfo();
	ListCell   *cell;
	char	   *sep;

	memset(&context, 0, sizeof(deparse_context));
	context.buf = buf;

	appendStringInfoChar(buf, '(');
	sep = "";
	foreach(cell, bound_datums)
	{
		PartitionRangeDatum *datum =
			lfirst_node(PartitionRangeDatum, cell);

		appendStringInfoString(buf, sep);
		if (datum->kind == PARTITION_RANGE_DATUM_MINVALUE)
			appendStringInfoString(buf, "MINVALUE");
		else if (datum->kind == PARTITION_RANGE_DATUM_MAXVALUE)
			appendStringInfoString(buf, "MAXVALUE");
		else
		{
			Const	   *val = castNode(Const, datum->value);

			get_const_expr(val, &context, -1);
		}
		sep = ", ";
	}
	appendStringInfoChar(buf, ')');

	return buf->data;
}
