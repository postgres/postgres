/*-------------------------------------------------------------------------
 *
 * indexing.h
 *	  This file provides some definitions to support indexing
 *	  on system catalogs
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: indexing.h,v 1.79 2003/08/04 02:40:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEXING_H
#define INDEXING_H

#include "access/htup.h"

/*
 * Names of indexes on system catalogs
 *
 * References to specific system indexes in the C code should use these
 * macros rather than hardwiring the actual index name.
 */
#define AccessMethodOperatorIndex	"pg_amop_opr_opc_index"
#define AccessMethodStrategyIndex	"pg_amop_opc_strategy_index"
#define AccessMethodProcedureIndex	"pg_amproc_opc_procnum_index"
#define AggregateFnoidIndex			"pg_aggregate_fnoid_index"
#define AmNameIndex					"pg_am_name_index"
#define AmOidIndex					"pg_am_oid_index"
#define AttrDefaultIndex			"pg_attrdef_adrelid_adnum_index"
#define AttrDefaultOidIndex			"pg_attrdef_oid_index"
#define AttributeRelidNameIndex		"pg_attribute_relid_attnam_index"
#define AttributeRelidNumIndex		"pg_attribute_relid_attnum_index"
#define CastOidIndex				"pg_cast_oid_index"
#define CastSourceTargetIndex		"pg_cast_source_target_index"
#define ClassNameNspIndex			"pg_class_relname_nsp_index"
#define ClassOidIndex				"pg_class_oid_index"
#define ConstraintNameNspIndex		"pg_constraint_conname_nsp_index"
#define ConstraintOidIndex			"pg_constraint_oid_index"
#define ConstraintRelidIndex		"pg_constraint_conrelid_index"
#define ConstraintTypidIndex		"pg_constraint_contypid_index"
#define ConversionDefaultIndex		"pg_conversion_default_index"
#define ConversionNameNspIndex		"pg_conversion_name_nsp_index"
#define ConversionOidIndex			"pg_conversion_oid_index"
#define DatabaseNameIndex			"pg_database_datname_index"
#define DatabaseOidIndex			"pg_database_oid_index"
#define DependDependerIndex			"pg_depend_depender_index"
#define DependReferenceIndex		"pg_depend_reference_index"
#define DescriptionObjIndex			"pg_description_o_c_o_index"
#define GroupNameIndex				"pg_group_name_index"
#define GroupSysidIndex				"pg_group_sysid_index"
#define IndexIndrelidIndex			"pg_index_indrelid_index"
#define IndexRelidIndex				"pg_index_indexrelid_index"
#define InheritsRelidSeqnoIndex		"pg_inherits_relid_seqno_index"
#define LanguageNameIndex			"pg_language_name_index"
#define LanguageOidIndex			"pg_language_oid_index"
#define LargeObjectLOidPNIndex		"pg_largeobject_loid_pn_index"
#define NamespaceNameIndex			"pg_namespace_nspname_index"
#define NamespaceOidIndex			"pg_namespace_oid_index"
#define OpclassAmNameNspIndex		"pg_opclass_am_name_nsp_index"
#define OpclassOidIndex				"pg_opclass_oid_index"
#define OperatorNameNspIndex		"pg_operator_oprname_l_r_n_index"
#define OperatorOidIndex			"pg_operator_oid_index"
#define ProcedureNameNspIndex		"pg_proc_proname_args_nsp_index"
#define ProcedureOidIndex			"pg_proc_oid_index"
#define RewriteOidIndex				"pg_rewrite_oid_index"
#define RewriteRelRulenameIndex		"pg_rewrite_rel_rulename_index"
#define ShadowNameIndex				"pg_shadow_usename_index"
#define ShadowSysidIndex			"pg_shadow_usesysid_index"
#define StatisticRelidAttnumIndex	"pg_statistic_relid_att_index"
#define TriggerConstrNameIndex		"pg_trigger_tgconstrname_index"
#define TriggerConstrRelidIndex		"pg_trigger_tgconstrrelid_index"
#define TriggerRelidNameIndex		"pg_trigger_tgrelid_tgname_index"
#define TriggerOidIndex				"pg_trigger_oid_index"
#define TypeNameNspIndex			"pg_type_typname_nsp_index"
#define TypeOidIndex				"pg_type_oid_index"


/*
 * The state object used by CatalogOpenIndexes and friends is actually the
 * same as the executor's ResultRelInfo, but we give it another type name
 * to decouple callers from that fact.
 */
typedef struct ResultRelInfo *CatalogIndexState;

/*
 * indexing.c prototypes
 */
extern CatalogIndexState CatalogOpenIndexes(Relation heapRel);
extern void CatalogCloseIndexes(CatalogIndexState indstate);
extern void CatalogIndexInsert(CatalogIndexState indstate,
				   HeapTuple heapTuple);
extern void CatalogUpdateIndexes(Relation heapRel, HeapTuple heapTuple);


/*
 * These macros are just to keep the C compiler from spitting up on the
 * upcoming commands for genbki.sh.
 */
#define DECLARE_INDEX(x) extern int no_such_variable
#define DECLARE_UNIQUE_INDEX(x) extern int no_such_variable
#define BUILD_INDICES


/*
 * What follows are lines processed by genbki.sh to create the statements
 * the bootstrap parser will turn into DefineIndex commands.
 *
 * The keyword is DECLARE_INDEX or DECLARE_UNIQUE_INDEX.  Everything after
 * that is just like in a normal 'create index' SQL command.
 */

DECLARE_UNIQUE_INDEX(pg_aggregate_fnoid_index on pg_aggregate using btree(aggfnoid oid_ops));
DECLARE_UNIQUE_INDEX(pg_am_name_index on pg_am using btree(amname name_ops));
DECLARE_UNIQUE_INDEX(pg_am_oid_index on pg_am using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_amop_opr_opc_index on pg_amop using btree(amopopr oid_ops, amopclaid oid_ops));
DECLARE_UNIQUE_INDEX(pg_amop_opc_strategy_index on pg_amop using btree(amopclaid oid_ops, amopstrategy int2_ops));
DECLARE_UNIQUE_INDEX(pg_amproc_opc_procnum_index on pg_amproc using btree(amopclaid oid_ops, amprocnum int2_ops));
DECLARE_UNIQUE_INDEX(pg_attrdef_adrelid_adnum_index on pg_attrdef using btree(adrelid oid_ops, adnum int2_ops));
DECLARE_UNIQUE_INDEX(pg_attrdef_oid_index on pg_attrdef using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_attribute_relid_attnam_index on pg_attribute using btree(attrelid oid_ops, attname name_ops));
DECLARE_UNIQUE_INDEX(pg_attribute_relid_attnum_index on pg_attribute using btree(attrelid oid_ops, attnum int2_ops));
DECLARE_UNIQUE_INDEX(pg_cast_oid_index on pg_cast using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_cast_source_target_index on pg_cast using btree(castsource oid_ops, casttarget oid_ops));
DECLARE_UNIQUE_INDEX(pg_class_oid_index on pg_class using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_class_relname_nsp_index on pg_class using btree(relname name_ops, relnamespace oid_ops));
/* This following index is not used for a cache and is not unique */
DECLARE_INDEX(pg_constraint_conname_nsp_index on pg_constraint using btree(conname name_ops, connamespace oid_ops));
/* This following index is not used for a cache and is not unique */
DECLARE_INDEX(pg_constraint_conrelid_index on pg_constraint using btree(conrelid oid_ops));
/* This following index is not used for a cache and is not unique */
DECLARE_INDEX(pg_constraint_contypid_index on pg_constraint using btree(contypid oid_ops));
DECLARE_UNIQUE_INDEX(pg_constraint_oid_index on pg_constraint using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_conversion_default_index on pg_conversion using btree(connamespace oid_ops, conforencoding int4_ops, contoencoding int4_ops, oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_conversion_name_nsp_index on pg_conversion using btree(conname name_ops, connamespace oid_ops));
DECLARE_UNIQUE_INDEX(pg_conversion_oid_index on pg_conversion using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_database_datname_index on pg_database using btree(datname name_ops));
DECLARE_UNIQUE_INDEX(pg_database_oid_index on pg_database using btree(oid oid_ops));
/* This following index is not used for a cache and is not unique */
DECLARE_INDEX(pg_depend_depender_index on pg_depend using btree(classid oid_ops, objid oid_ops, objsubid int4_ops));
/* This following index is not used for a cache and is not unique */
DECLARE_INDEX(pg_depend_reference_index on pg_depend using btree(refclassid oid_ops, refobjid oid_ops, refobjsubid int4_ops));
DECLARE_UNIQUE_INDEX(pg_description_o_c_o_index on pg_description using btree(objoid oid_ops, classoid oid_ops, objsubid int4_ops));
DECLARE_UNIQUE_INDEX(pg_group_name_index on pg_group using btree(groname name_ops));
DECLARE_UNIQUE_INDEX(pg_group_sysid_index on pg_group using btree(grosysid int4_ops));
/* This following index is not used for a cache and is not unique */
DECLARE_INDEX(pg_index_indrelid_index on pg_index using btree(indrelid oid_ops));
DECLARE_UNIQUE_INDEX(pg_index_indexrelid_index on pg_index using btree(indexrelid oid_ops));
DECLARE_UNIQUE_INDEX(pg_inherits_relid_seqno_index on pg_inherits using btree(inhrelid oid_ops, inhseqno int4_ops));
DECLARE_UNIQUE_INDEX(pg_language_name_index on pg_language using btree(lanname name_ops));
DECLARE_UNIQUE_INDEX(pg_language_oid_index on pg_language using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_largeobject_loid_pn_index on pg_largeobject using btree(loid oid_ops, pageno int4_ops));
DECLARE_UNIQUE_INDEX(pg_namespace_nspname_index on pg_namespace using btree(nspname name_ops));
DECLARE_UNIQUE_INDEX(pg_namespace_oid_index on pg_namespace using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_opclass_am_name_nsp_index on pg_opclass using btree(opcamid oid_ops, opcname name_ops, opcnamespace oid_ops));
DECLARE_UNIQUE_INDEX(pg_opclass_oid_index on pg_opclass using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_operator_oid_index on pg_operator using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_operator_oprname_l_r_n_index on pg_operator using btree(oprname name_ops, oprleft oid_ops, oprright oid_ops, oprnamespace oid_ops));
DECLARE_UNIQUE_INDEX(pg_proc_oid_index on pg_proc using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_proc_proname_args_nsp_index on pg_proc using btree(proname name_ops, pronargs int2_ops, proargtypes oidvector_ops, pronamespace oid_ops));
/* This following index is not used for a cache and is not unique */
DECLARE_UNIQUE_INDEX(pg_rewrite_oid_index on pg_rewrite using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_rewrite_rel_rulename_index on pg_rewrite using btree(ev_class oid_ops, rulename name_ops));
DECLARE_UNIQUE_INDEX(pg_shadow_usename_index on pg_shadow using btree(usename name_ops));
DECLARE_UNIQUE_INDEX(pg_shadow_usesysid_index on pg_shadow using btree(usesysid int4_ops));
DECLARE_UNIQUE_INDEX(pg_statistic_relid_att_index on pg_statistic using btree(starelid oid_ops, staattnum int2_ops));
/* This following index is not used for a cache and is not unique */
DECLARE_INDEX(pg_trigger_tgconstrname_index on pg_trigger using btree(tgconstrname name_ops));
/* This following index is not used for a cache and is not unique */
DECLARE_INDEX(pg_trigger_tgconstrrelid_index on pg_trigger using btree(tgconstrrelid oid_ops));
DECLARE_UNIQUE_INDEX(pg_trigger_tgrelid_tgname_index on pg_trigger using btree(tgrelid oid_ops, tgname name_ops));
DECLARE_UNIQUE_INDEX(pg_trigger_oid_index on pg_trigger using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_type_oid_index on pg_type using btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_type_typname_nsp_index on pg_type using btree(typname name_ops, typnamespace oid_ops));

/* last step of initialization script: build the indexes declared above */
BUILD_INDICES

#endif   /* INDEXING_H */
