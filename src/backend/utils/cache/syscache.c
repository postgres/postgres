/*-------------------------------------------------------------------------
 *
 * syscache.c
 *	  System cache management routines
 *
 *	  In pgplanner mode, all catalog lookups are intercepted here and
 *	  routed to user-provided callbacks. No real catcache is initialized.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/syscache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_description.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_parameter_acl.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_range.h"
#include "catalog/pg_replication_origin.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_shdescription.h"
#include "catalog/pg_shseclabel.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_config_map.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_user_mapping.h"
#include "lib/qunique.h"
#include "pgplanner/pgplanner.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*---------------------------------------------------------------------------
 *		struct cachedesc: information defining a single syscache
 *
 *		We keep this table only for nkeys lookup in the SearchSysCache
 *		dispatcher.  In pgplanner mode, InitCatalogCache is never called.
 *---------------------------------------------------------------------------
 */
struct cachedesc
{
	Oid			reloid;
	Oid			indoid;
	int			nkeys;
	int			key[4];
	int			nbuckets;
};

#define KEY(...) VA_ARGS_NARGS(__VA_ARGS__), { __VA_ARGS__ }

static const struct cachedesc cacheinfo[] = {
	[AGGFNOID] = {
		AggregateRelationId,
		AggregateFnoidIndexId,
		KEY(Anum_pg_aggregate_aggfnoid),
		16
	},
	[AMNAME] = {
		AccessMethodRelationId,
		AmNameIndexId,
		KEY(Anum_pg_am_amname),
		4
	},
	[AMOID] = {
		AccessMethodRelationId,
		AmOidIndexId,
		KEY(Anum_pg_am_oid),
		4
	},
	[AMOPOPID] = {
		AccessMethodOperatorRelationId,
		AccessMethodOperatorIndexId,
		KEY(Anum_pg_amop_amopopr,
			Anum_pg_amop_amoppurpose,
			Anum_pg_amop_amopfamily),
		64
	},
	[AMOPSTRATEGY] = {
		AccessMethodOperatorRelationId,
		AccessMethodStrategyIndexId,
		KEY(Anum_pg_amop_amopfamily,
			Anum_pg_amop_amoplefttype,
			Anum_pg_amop_amoprighttype,
			Anum_pg_amop_amopstrategy),
		64
	},
	[AMPROCNUM] = {
		AccessMethodProcedureRelationId,
		AccessMethodProcedureIndexId,
		KEY(Anum_pg_amproc_amprocfamily,
			Anum_pg_amproc_amproclefttype,
			Anum_pg_amproc_amprocrighttype,
			Anum_pg_amproc_amprocnum),
		16
	},
	[ATTNAME] = {
		AttributeRelationId,
		AttributeRelidNameIndexId,
		KEY(Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attname),
		32
	},
	[ATTNUM] = {
		AttributeRelationId,
		AttributeRelidNumIndexId,
		KEY(Anum_pg_attribute_attrelid,
			Anum_pg_attribute_attnum),
		128
	},
	[AUTHMEMMEMROLE] = {
		AuthMemRelationId,
		AuthMemMemRoleIndexId,
		KEY(Anum_pg_auth_members_member,
			Anum_pg_auth_members_roleid,
			Anum_pg_auth_members_grantor),
		8
	},
	[AUTHMEMROLEMEM] = {
		AuthMemRelationId,
		AuthMemRoleMemIndexId,
		KEY(Anum_pg_auth_members_roleid,
			Anum_pg_auth_members_member,
			Anum_pg_auth_members_grantor),
		8
	},
	[AUTHNAME] = {
		AuthIdRelationId,
		AuthIdRolnameIndexId,
		KEY(Anum_pg_authid_rolname),
		8
	},
	[AUTHOID] = {
		AuthIdRelationId,
		AuthIdOidIndexId,
		KEY(Anum_pg_authid_oid),
		8
	},
	[CASTSOURCETARGET] = {
		CastRelationId,
		CastSourceTargetIndexId,
		KEY(Anum_pg_cast_castsource,
			Anum_pg_cast_casttarget),
		256
	},
	[CLAAMNAMENSP] = {
		OperatorClassRelationId,
		OpclassAmNameNspIndexId,
		KEY(Anum_pg_opclass_opcmethod,
			Anum_pg_opclass_opcname,
			Anum_pg_opclass_opcnamespace),
		8
	},
	[CLAOID] = {
		OperatorClassRelationId,
		OpclassOidIndexId,
		KEY(Anum_pg_opclass_oid),
		8
	},
	[COLLNAMEENCNSP] = {
		CollationRelationId,
		CollationNameEncNspIndexId,
		KEY(Anum_pg_collation_collname,
			Anum_pg_collation_collencoding,
			Anum_pg_collation_collnamespace),
		8
	},
	[COLLOID] = {
		CollationRelationId,
		CollationOidIndexId,
		KEY(Anum_pg_collation_oid),
		8
	},
	[CONDEFAULT] = {
		ConversionRelationId,
		ConversionDefaultIndexId,
		KEY(Anum_pg_conversion_connamespace,
			Anum_pg_conversion_conforencoding,
			Anum_pg_conversion_contoencoding,
			Anum_pg_conversion_oid),
		8
	},
	[CONNAMENSP] = {
		ConversionRelationId,
		ConversionNameNspIndexId,
		KEY(Anum_pg_conversion_conname,
			Anum_pg_conversion_connamespace),
		8
	},
	[CONSTROID] = {
		ConstraintRelationId,
		ConstraintOidIndexId,
		KEY(Anum_pg_constraint_oid),
		16
	},
	[CONVOID] = {
		ConversionRelationId,
		ConversionOidIndexId,
		KEY(Anum_pg_conversion_oid),
		8
	},
	[DATABASEOID] = {
		DatabaseRelationId,
		DatabaseOidIndexId,
		KEY(Anum_pg_database_oid),
		4
	},
	[DEFACLROLENSPOBJ] = {
		DefaultAclRelationId,
		DefaultAclRoleNspObjIndexId,
		KEY(Anum_pg_default_acl_defaclrole,
			Anum_pg_default_acl_defaclnamespace,
			Anum_pg_default_acl_defaclobjtype),
		8
	},
	[ENUMOID] = {
		EnumRelationId,
		EnumOidIndexId,
		KEY(Anum_pg_enum_oid),
		8
	},
	[ENUMTYPOIDNAME] = {
		EnumRelationId,
		EnumTypIdLabelIndexId,
		KEY(Anum_pg_enum_enumtypid,
			Anum_pg_enum_enumlabel),
		8
	},
	[EVENTTRIGGERNAME] = {
		EventTriggerRelationId,
		EventTriggerNameIndexId,
		KEY(Anum_pg_event_trigger_evtname),
		8
	},
	[EVENTTRIGGEROID] = {
		EventTriggerRelationId,
		EventTriggerOidIndexId,
		KEY(Anum_pg_event_trigger_oid),
		8
	},
	[FOREIGNDATAWRAPPERNAME] = {
		ForeignDataWrapperRelationId,
		ForeignDataWrapperNameIndexId,
		KEY(Anum_pg_foreign_data_wrapper_fdwname),
		2
	},
	[FOREIGNDATAWRAPPEROID] = {
		ForeignDataWrapperRelationId,
		ForeignDataWrapperOidIndexId,
		KEY(Anum_pg_foreign_data_wrapper_oid),
		2
	},
	[FOREIGNSERVERNAME] = {
		ForeignServerRelationId,
		ForeignServerNameIndexId,
		KEY(Anum_pg_foreign_server_srvname),
		2
	},
	[FOREIGNSERVEROID] = {
		ForeignServerRelationId,
		ForeignServerOidIndexId,
		KEY(Anum_pg_foreign_server_oid),
		2
	},
	[FOREIGNTABLEREL] = {
		ForeignTableRelationId,
		ForeignTableRelidIndexId,
		KEY(Anum_pg_foreign_table_ftrelid),
		4
	},
	[INDEXRELID] = {
		IndexRelationId,
		IndexRelidIndexId,
		KEY(Anum_pg_index_indexrelid),
		64
	},
	[LANGNAME] = {
		LanguageRelationId,
		LanguageNameIndexId,
		KEY(Anum_pg_language_lanname),
		4
	},
	[LANGOID] = {
		LanguageRelationId,
		LanguageOidIndexId,
		KEY(Anum_pg_language_oid),
		4
	},
	[NAMESPACENAME] = {
		NamespaceRelationId,
		NamespaceNameIndexId,
		KEY(Anum_pg_namespace_nspname),
		4
	},
	[NAMESPACEOID] = {
		NamespaceRelationId,
		NamespaceOidIndexId,
		KEY(Anum_pg_namespace_oid),
		16
	},
	[OPERNAMENSP] = {
		OperatorRelationId,
		OperatorNameNspIndexId,
		KEY(Anum_pg_operator_oprname,
			Anum_pg_operator_oprleft,
			Anum_pg_operator_oprright,
			Anum_pg_operator_oprnamespace),
		256
	},
	[OPEROID] = {
		OperatorRelationId,
		OperatorOidIndexId,
		KEY(Anum_pg_operator_oid),
		32
	},
	[OPFAMILYAMNAMENSP] = {
		OperatorFamilyRelationId,
		OpfamilyAmNameNspIndexId,
		KEY(Anum_pg_opfamily_opfmethod,
			Anum_pg_opfamily_opfname,
			Anum_pg_opfamily_opfnamespace),
		8
	},
	[OPFAMILYOID] = {
		OperatorFamilyRelationId,
		OpfamilyOidIndexId,
		KEY(Anum_pg_opfamily_oid),
		8
	},
	[PARAMETERACLNAME] = {
		ParameterAclRelationId,
		ParameterAclParnameIndexId,
		KEY(Anum_pg_parameter_acl_parname),
		4
	},
	[PARAMETERACLOID] = {
		ParameterAclRelationId,
		ParameterAclOidIndexId,
		KEY(Anum_pg_parameter_acl_oid),
		4
	},
	[PARTRELID] = {
		PartitionedRelationId,
		PartitionedRelidIndexId,
		KEY(Anum_pg_partitioned_table_partrelid),
		32
	},
	[PROCNAMEARGSNSP] = {
		ProcedureRelationId,
		ProcedureNameArgsNspIndexId,
		KEY(Anum_pg_proc_proname,
			Anum_pg_proc_proargtypes,
			Anum_pg_proc_pronamespace),
		128
	},
	[PROCOID] = {
		ProcedureRelationId,
		ProcedureOidIndexId,
		KEY(Anum_pg_proc_oid),
		128
	},
	[PUBLICATIONNAME] = {
		PublicationRelationId,
		PublicationNameIndexId,
		KEY(Anum_pg_publication_pubname),
		8
	},
	[PUBLICATIONNAMESPACE] = {
		PublicationNamespaceRelationId,
		PublicationNamespaceObjectIndexId,
		KEY(Anum_pg_publication_namespace_oid),
		64
	},
	[PUBLICATIONNAMESPACEMAP] = {
		PublicationNamespaceRelationId,
		PublicationNamespacePnnspidPnpubidIndexId,
		KEY(Anum_pg_publication_namespace_pnnspid,
			Anum_pg_publication_namespace_pnpubid),
		64
	},
	[PUBLICATIONOID] = {
		PublicationRelationId,
		PublicationObjectIndexId,
		KEY(Anum_pg_publication_oid),
		8
	},
	[PUBLICATIONREL] = {
		PublicationRelRelationId,
		PublicationRelObjectIndexId,
		KEY(Anum_pg_publication_rel_oid),
		64
	},
	[PUBLICATIONRELMAP] = {
		PublicationRelRelationId,
		PublicationRelPrrelidPrpubidIndexId,
		KEY(Anum_pg_publication_rel_prrelid,
			Anum_pg_publication_rel_prpubid),
		64
	},
	[RANGEMULTIRANGE] = {
		RangeRelationId,
		RangeMultirangeTypidIndexId,
		KEY(Anum_pg_range_rngmultitypid),
		4
	},
	[RANGETYPE] = {
		RangeRelationId,
		RangeTypidIndexId,
		KEY(Anum_pg_range_rngtypid),
		4
	},
	[RELNAMENSP] = {
		RelationRelationId,
		ClassNameNspIndexId,
		KEY(Anum_pg_class_relname,
			Anum_pg_class_relnamespace),
		128
	},
	[RELOID] = {
		RelationRelationId,
		ClassOidIndexId,
		KEY(Anum_pg_class_oid),
		128
	},
	[REPLORIGIDENT] = {
		ReplicationOriginRelationId,
		ReplicationOriginIdentIndex,
		KEY(Anum_pg_replication_origin_roident),
		16
	},
	[REPLORIGNAME] = {
		ReplicationOriginRelationId,
		ReplicationOriginNameIndex,
		KEY(Anum_pg_replication_origin_roname),
		16
	},
	[RULERELNAME] = {
		RewriteRelationId,
		RewriteRelRulenameIndexId,
		KEY(Anum_pg_rewrite_ev_class,
			Anum_pg_rewrite_rulename),
		8
	},
	[SEQRELID] = {
		SequenceRelationId,
		SequenceRelidIndexId,
		KEY(Anum_pg_sequence_seqrelid),
		32
	},
	[STATEXTDATASTXOID] = {
		StatisticExtDataRelationId,
		StatisticExtDataStxoidInhIndexId,
		KEY(Anum_pg_statistic_ext_data_stxoid,
			Anum_pg_statistic_ext_data_stxdinherit),
		4
	},
	[STATEXTNAMENSP] = {
		StatisticExtRelationId,
		StatisticExtNameIndexId,
		KEY(Anum_pg_statistic_ext_stxname,
			Anum_pg_statistic_ext_stxnamespace),
		4
	},
	[STATEXTOID] = {
		StatisticExtRelationId,
		StatisticExtOidIndexId,
		KEY(Anum_pg_statistic_ext_oid),
		4
	},
	[STATRELATTINH] = {
		StatisticRelationId,
		StatisticRelidAttnumInhIndexId,
		KEY(Anum_pg_statistic_starelid,
			Anum_pg_statistic_staattnum,
			Anum_pg_statistic_stainherit),
		128
	},
	[SUBSCRIPTIONNAME] = {
		SubscriptionRelationId,
		SubscriptionNameIndexId,
		KEY(Anum_pg_subscription_subdbid,
			Anum_pg_subscription_subname),
		4
	},
	[SUBSCRIPTIONOID] = {
		SubscriptionRelationId,
		SubscriptionObjectIndexId,
		KEY(Anum_pg_subscription_oid),
		4
	},
	[SUBSCRIPTIONRELMAP] = {
		SubscriptionRelRelationId,
		SubscriptionRelSrrelidSrsubidIndexId,
		KEY(Anum_pg_subscription_rel_srrelid,
			Anum_pg_subscription_rel_srsubid),
		64
	},
	[TABLESPACEOID] = {
		TableSpaceRelationId,
		TablespaceOidIndexId,
		KEY(Anum_pg_tablespace_oid),
		4
	},
	[TRFOID] = {
		TransformRelationId,
		TransformOidIndexId,
		KEY(Anum_pg_transform_oid),
		16
	},
	[TRFTYPELANG] = {
		TransformRelationId,
		TransformTypeLangIndexId,
		KEY(Anum_pg_transform_trftype,
			Anum_pg_transform_trflang),
		16
	},
	[TSCONFIGMAP] = {
		TSConfigMapRelationId,
		TSConfigMapIndexId,
		KEY(Anum_pg_ts_config_map_mapcfg,
			Anum_pg_ts_config_map_maptokentype,
			Anum_pg_ts_config_map_mapseqno),
		2
	},
	[TSCONFIGNAMENSP] = {
		TSConfigRelationId,
		TSConfigNameNspIndexId,
		KEY(Anum_pg_ts_config_cfgname,
			Anum_pg_ts_config_cfgnamespace),
		2
	},
	[TSCONFIGOID] = {
		TSConfigRelationId,
		TSConfigOidIndexId,
		KEY(Anum_pg_ts_config_oid),
		2
	},
	[TSDICTNAMENSP] = {
		TSDictionaryRelationId,
		TSDictionaryNameNspIndexId,
		KEY(Anum_pg_ts_dict_dictname,
			Anum_pg_ts_dict_dictnamespace),
		2
	},
	[TSDICTOID] = {
		TSDictionaryRelationId,
		TSDictionaryOidIndexId,
		KEY(Anum_pg_ts_dict_oid),
		2
	},
	[TSPARSERNAMENSP] = {
		TSParserRelationId,
		TSParserNameNspIndexId,
		KEY(Anum_pg_ts_parser_prsname,
			Anum_pg_ts_parser_prsnamespace),
		2
	},
	[TSPARSEROID] = {
		TSParserRelationId,
		TSParserOidIndexId,
		KEY(Anum_pg_ts_parser_oid),
		2
	},
	[TSTEMPLATENAMENSP] = {
		TSTemplateRelationId,
		TSTemplateNameNspIndexId,
		KEY(Anum_pg_ts_template_tmplname,
			Anum_pg_ts_template_tmplnamespace),
		2
	},
	[TSTEMPLATEOID] = {
		TSTemplateRelationId,
		TSTemplateOidIndexId,
		KEY(Anum_pg_ts_template_oid),
		2
	},
	[TYPENAMENSP] = {
		TypeRelationId,
		TypeNameNspIndexId,
		KEY(Anum_pg_type_typname,
			Anum_pg_type_typnamespace),
		64
	},
	[TYPEOID] = {
		TypeRelationId,
		TypeOidIndexId,
		KEY(Anum_pg_type_oid),
		64
	},
	[USERMAPPINGOID] = {
		UserMappingRelationId,
		UserMappingOidIndexId,
		KEY(Anum_pg_user_mapping_oid),
		2
	},
	[USERMAPPINGUSERSERVER] = {
		UserMappingRelationId,
		UserMappingUserServerIndexId,
		KEY(Anum_pg_user_mapping_umuser,
			Anum_pg_user_mapping_umserver),
		2
	}
};

StaticAssertDecl(lengthof(cacheinfo) == SysCacheSize,
				 "SysCacheSize does not match syscache.c's array");

static CatCache *SysCache[SysCacheSize];

static bool CacheInitialized = false;

static Oid	SysCacheRelationOid[SysCacheSize];
static int	SysCacheRelationOidSize;

static Oid	SysCacheSupportingRelOid[SysCacheSize * 2];
static int	SysCacheSupportingRelOidSize;

static int	oid_compare(const void *a, const void *b);


/*
 * InitCatalogCache - initialize the caches
 *
 * In pgplanner mode this is never called, but we keep it for link
 * compatibility with code that references it.
 */
void
InitCatalogCache(void)
{
	int			cacheId;

	Assert(!CacheInitialized);

	SysCacheRelationOidSize = SysCacheSupportingRelOidSize = 0;

	for (cacheId = 0; cacheId < SysCacheSize; cacheId++)
	{
		Assert(cacheinfo[cacheId].reloid != 0);

		SysCache[cacheId] = InitCatCache(cacheId,
										 cacheinfo[cacheId].reloid,
										 cacheinfo[cacheId].indoid,
										 cacheinfo[cacheId].nkeys,
										 cacheinfo[cacheId].key,
										 cacheinfo[cacheId].nbuckets);
		if (!PointerIsValid(SysCache[cacheId]))
			elog(ERROR, "could not initialize cache %u (%d)",
				 cacheinfo[cacheId].reloid, cacheId);
		SysCacheRelationOid[SysCacheRelationOidSize++] =
			cacheinfo[cacheId].reloid;
		SysCacheSupportingRelOid[SysCacheSupportingRelOidSize++] =
			cacheinfo[cacheId].reloid;
		SysCacheSupportingRelOid[SysCacheSupportingRelOidSize++] =
			cacheinfo[cacheId].indoid;
		Assert(!RelationInvalidatesSnapshotsOnly(cacheinfo[cacheId].reloid));
	}

	Assert(SysCacheRelationOidSize <= lengthof(SysCacheRelationOid));
	Assert(SysCacheSupportingRelOidSize <= lengthof(SysCacheSupportingRelOid));

	pg_qsort(SysCacheRelationOid, SysCacheRelationOidSize,
			 sizeof(Oid), oid_compare);
	SysCacheRelationOidSize =
		qunique(SysCacheRelationOid, SysCacheRelationOidSize, sizeof(Oid),
				oid_compare);

	pg_qsort(SysCacheSupportingRelOid, SysCacheSupportingRelOidSize,
			 sizeof(Oid), oid_compare);
	SysCacheSupportingRelOidSize =
		qunique(SysCacheSupportingRelOid, SysCacheSupportingRelOidSize,
				sizeof(Oid), oid_compare);

	CacheInitialized = true;
}

void
InitCatalogCachePhase2(void)
{
	int			cacheId;

	Assert(CacheInitialized);

	for (cacheId = 0; cacheId < SysCacheSize; cacheId++)
		InitCatCachePhase2(SysCache[cacheId], true);
}


/* ----------------------------------------------------------------
 *		pgplanner fake tuple builders
 * ----------------------------------------------------------------
 */

/*
 * pgplanner_build_type_tuple
 *		Build a fake HeapTuple containing FormData_pg_type from callback data.
 */
static HeapTuple
pgplanner_build_type_tuple(Oid typid, const PgPlannerTypeInfo *tinfo)
{
	HeapTuple	result;
	Size		hdr_len = MAXALIGN(SizeofHeapTupleHeader);
	Size		data_len = sizeof(FormData_pg_type);
	Size		total_len = hdr_len + data_len;
	FormData_pg_type *typeForm;

	result = (HeapTuple) palloc0(sizeof(HeapTupleData));
	result->t_data = (HeapTupleHeader) palloc0(total_len);
	result->t_len = total_len;
	ItemPointerSetInvalid(&result->t_data->t_ctid);
	result->t_data->t_hoff = hdr_len;
	result->t_data->t_infomask = 0;
	HeapTupleHeaderSetNatts(result->t_data, Natts_pg_type);

	typeForm = (FormData_pg_type *) GETSTRUCT(result);
	typeForm->oid = typid;
	namestrcpy(&typeForm->typname, tinfo->typname ? tinfo->typname : "unknown");
	typeForm->typnamespace = tinfo->typnamespace;
	typeForm->typowner = tinfo->typowner;
	typeForm->typlen = tinfo->typlen;
	typeForm->typbyval = tinfo->typbyval;
	typeForm->typtype = tinfo->typtype;
	typeForm->typcategory = tinfo->typcategory;
	typeForm->typispreferred = tinfo->typispreferred;
	typeForm->typisdefined = tinfo->typisdefined;
	typeForm->typdelim = tinfo->typdelim;
	typeForm->typrelid = tinfo->typrelid;
	typeForm->typsubscript = tinfo->typsubscript;
	typeForm->typelem = tinfo->typelem;
	typeForm->typarray = tinfo->typarray;
	typeForm->typinput = tinfo->typinput;
	typeForm->typoutput = tinfo->typoutput;
	typeForm->typreceive = tinfo->typreceive;
	typeForm->typsend = tinfo->typsend;
	typeForm->typmodin = tinfo->typmodin;
	typeForm->typmodout = tinfo->typmodout;
	typeForm->typanalyze = tinfo->typanalyze;
	typeForm->typalign = tinfo->typalign;
	typeForm->typstorage = tinfo->typstorage;
	typeForm->typnotnull = tinfo->typnotnull;
	typeForm->typbasetype = tinfo->typbasetype;
	typeForm->typtypmod = tinfo->typtypmod;
	typeForm->typndims = tinfo->typndims;
	typeForm->typcollation = tinfo->typcollation;

	return result;
}

/*
 * pgplanner_build_proc_tuple
 *		Build a fake HeapTuple containing FormData_pg_proc from callback data.
 */
static HeapTuple
pgplanner_build_proc_tuple(Oid funcid, const PgPlannerFunctionInfo *finfo)
{
	HeapTuple	result;
	Size		hdr_len = MAXALIGN(SizeofHeapTupleHeader);
	int			nargs = finfo->pronargs;
	Size		data_len = offsetof(FormData_pg_proc, proargtypes) +
						   offsetof(oidvector, values) +
						   nargs * sizeof(Oid);
	Size		total_len = hdr_len + MAXALIGN(data_len);
	FormData_pg_proc *procForm;

	result = (HeapTuple) palloc0(sizeof(HeapTupleData));
	result->t_data = (HeapTupleHeader) palloc0(total_len);
	result->t_len = total_len;
	ItemPointerSetInvalid(&result->t_data->t_ctid);
	result->t_data->t_hoff = hdr_len;
	result->t_data->t_infomask = 0;
	HeapTupleHeaderSetNatts(result->t_data, Natts_pg_proc);

	procForm = (FormData_pg_proc *) GETSTRUCT(result);
	procForm->oid = funcid;
	namestrcpy(&procForm->proname, finfo->proname ? finfo->proname : "unknown");
	procForm->pronamespace = finfo->pronamespace ? finfo->pronamespace : 11;
	procForm->proowner = 10;		/* BOOTSTRAP_SUPERUSERID */
	procForm->prolang = 12;			/* INTERNALlanguageId */
	procForm->procost = finfo->procost > 0 ? finfo->procost : 1;
	procForm->prorows = finfo->prorows;
	procForm->provariadic = finfo->provariadic;
	procForm->prosupport = finfo->prosupport;
	procForm->prokind = finfo->prokind;
	procForm->prosecdef = false;
	procForm->proleakproof = finfo->proleakproof;
	procForm->proisstrict = finfo->proisstrict;
	procForm->proretset = finfo->retset;
	procForm->provolatile = finfo->provolatile ? finfo->provolatile : PROVOLATILE_IMMUTABLE;
	procForm->proparallel = finfo->proparallel ? finfo->proparallel : PROPARALLEL_SAFE;
	procForm->pronargs = finfo->pronargs;
	procForm->pronargdefaults = finfo->pronargdefaults;
	procForm->prorettype = finfo->rettype;

	/* Fill in proargtypes oidvector */
	procForm->proargtypes.ndim = 1;
	procForm->proargtypes.dataoffset = 0;
	procForm->proargtypes.elemtype = OIDOID;
	procForm->proargtypes.dim1 = nargs;
	procForm->proargtypes.lbound1 = 0;
	SET_VARSIZE(&procForm->proargtypes,
				offsetof(oidvector, values) + nargs * sizeof(Oid));
	if (nargs > 0 && finfo->proargtypes)
		memcpy(procForm->proargtypes.values, finfo->proargtypes, nargs * sizeof(Oid));

	return result;
}

/*
 * pgplanner_build_agg_tuple
 *		Build a fake HeapTuple containing FormData_pg_aggregate from callback data.
 */
static HeapTuple
pgplanner_build_agg_tuple(Oid aggfnoid, const PgPlannerAggregateInfo *ainfo)
{
	HeapTuple	result;
	Size		hdr_len = MAXALIGN(SizeofHeapTupleHeader);
	Size		data_len = sizeof(FormData_pg_aggregate);
	Size		total_len = hdr_len + data_len;
	FormData_pg_aggregate *aggForm;

	result = (HeapTuple) palloc0(sizeof(HeapTupleData));
	result->t_data = (HeapTupleHeader) palloc0(total_len);
	result->t_len = total_len;
	ItemPointerSetInvalid(&result->t_data->t_ctid);
	result->t_data->t_hoff = hdr_len;
	result->t_data->t_infomask = 0;
	HeapTupleHeaderSetNatts(result->t_data, Natts_pg_aggregate);

	aggForm = (FormData_pg_aggregate *) GETSTRUCT(result);
	aggForm->aggfnoid = aggfnoid;
	aggForm->aggkind = ainfo->aggkind;
	aggForm->aggnumdirectargs = ainfo->aggnumdirectargs;
	aggForm->aggtransfn = ainfo->aggtransfn;
	aggForm->aggfinalfn = ainfo->aggfinalfn;
	aggForm->aggcombinefn = ainfo->aggcombinefn;
	aggForm->aggserialfn = ainfo->aggserialfn;
	aggForm->aggdeserialfn = ainfo->aggdeserialfn;
	aggForm->aggmtransfn = 0;
	aggForm->aggminvtransfn = 0;
	aggForm->aggmfinalfn = 0;
	aggForm->aggfinalextra = false;
	aggForm->aggmfinalextra = false;
	aggForm->aggfinalmodify = ainfo->aggfinalmodify;
	aggForm->aggmfinalmodify = 'r';
	aggForm->aggsortop = ainfo->aggsortop;
	aggForm->aggtranstype = ainfo->aggtranstype;
	aggForm->aggtransspace = ainfo->aggtransspace;
	aggForm->aggmtranstype = 0;
	aggForm->aggmtransspace = 0;

	return result;
}

/*
 * pgplanner_build_operator_tuple
 *		Build a fake HeapTuple containing FormData_pg_operator from callback data.
 */
static HeapTuple
pgplanner_build_operator_tuple(Oid oproid, const PgPlannerOperatorInfo *oinfo)
{
	HeapTuple	result;
	Size		hdr_len = MAXALIGN(SizeofHeapTupleHeader);
	Size		data_len = sizeof(FormData_pg_operator);
	Size		total_len = hdr_len + data_len;
	FormData_pg_operator *oprForm;

	result = (HeapTuple) palloc0(sizeof(HeapTupleData));
	result->t_data = (HeapTupleHeader) palloc0(total_len);
	result->t_len = total_len;
	ItemPointerSetInvalid(&result->t_data->t_ctid);
	result->t_data->t_hoff = hdr_len;
	result->t_data->t_infomask = 0;
	HeapTupleHeaderSetNatts(result->t_data, Natts_pg_operator);

	oprForm = (FormData_pg_operator *) GETSTRUCT(result);
	oprForm->oid = oproid;
	namestrcpy(&oprForm->oprname, oinfo->oprname ? oinfo->oprname : "?");
	oprForm->oprnamespace = oinfo->oprnamespace ? oinfo->oprnamespace : 11;
	oprForm->oprowner = oinfo->oprowner ? oinfo->oprowner : 10;
	oprForm->oprkind = oinfo->oprkind ? oinfo->oprkind : 'b';
	oprForm->oprcanmerge = oinfo->oprcanmerge;
	oprForm->oprcanhash = oinfo->oprcanhash;
	oprForm->oprleft = oinfo->oprleft;
	oprForm->oprright = oinfo->oprright;
	oprForm->oprresult = oinfo->oprresult;
	oprForm->oprcom = oinfo->oprcom;
	oprForm->oprnegate = oinfo->oprnegate;
	oprForm->oprcode = oinfo->oprcode;
	oprForm->oprrest = oinfo->oprrest;
	oprForm->oprjoin = oinfo->oprjoin;

	return result;
}


/* ----------------------------------------------------------------
 *		SearchSysCache and variants
 * ----------------------------------------------------------------
 */

/*
 * SearchSysCache
 *		Dispatch to SearchSysCache1/2/3/4 based on the number of keys
 *		defined for this cache in cacheinfo[].
 */
HeapTuple
SearchSysCache(int cacheId,
			   Datum key1,
			   Datum key2,
			   Datum key3,
			   Datum key4)
{
	Assert(cacheId >= 0 && cacheId < SysCacheSize);

	switch (cacheinfo[cacheId].nkeys)
	{
		case 1:
			return SearchSysCache1(cacheId, key1);
		case 2:
			return SearchSysCache2(cacheId, key1, key2);
		case 3:
			return SearchSysCache3(cacheId, key1, key2, key3);
		case 4:
			return SearchSysCache4(cacheId, key1, key2, key3, key4);
		default:
			elog(ERROR, "SearchSysCache: unexpected nkeys %d for cacheId %d",
				 cacheinfo[cacheId].nkeys, cacheId);
			return NULL;
	}
}


HeapTuple
SearchSysCache1(int cacheId,
				Datum key1)
{
	const PgPlannerCallbacks *cb = pgplanner_get_callbacks();

	switch (cacheId)
	{
		case TYPEOID:
		{
			if (cb && cb->get_type)
			{
				PgPlannerTypeInfo *tinfo = cb->get_type((Oid) key1);

				if (tinfo == NULL)
					return NULL;
				return pgplanner_build_type_tuple((Oid) key1, tinfo);
			}
			break;
		}
		case PROCOID:
		{
			if (cb && cb->get_function)
			{
				PgPlannerFunctionInfo *finfo = cb->get_function((Oid) key1);

				if (finfo == NULL)
					return NULL;
				return pgplanner_build_proc_tuple((Oid) key1, finfo);
			}
			break;
		}
		case AGGFNOID:
		{
			if (cb && cb->get_aggregate)
			{
				PgPlannerAggregateInfo *ainfo = cb->get_aggregate((Oid) key1);

				if (ainfo == NULL)
					return NULL;
				return pgplanner_build_agg_tuple((Oid) key1, ainfo);
			}
			break;
		}
		case OPEROID:
		{
			if (cb && cb->get_operator_by_oid)
			{
				PgPlannerOperatorInfo *oinfo = cb->get_operator_by_oid((Oid) key1);

				if (oinfo == NULL)
					return NULL;
				return pgplanner_build_operator_tuple((Oid) key1, oinfo);
			}
			break;
		}
		default:
			break;
	}

	elog(ERROR, "Unsupported cache lookup1: cacheId=%d, key=%u", cacheId, (unsigned int) key1);
	return NULL;
}

HeapTuple
SearchSysCache2(int cacheId,
				Datum key1, Datum key2)
{
	elog(ERROR, "Unsupported cache lookup2: cacheId=%d", cacheId);
	return NULL;
}

HeapTuple
SearchSysCache3(int cacheId,
				Datum key1, Datum key2, Datum key3)
{
	elog(ERROR, "Unsupported cache lookup3: cacheId=%d", cacheId);
	return NULL;
}

/*
 * SearchSysCache4
 *		Handles OPERNAMENSP: operator lookup by (name, left, right, namespace).
 */
HeapTuple
SearchSysCache4(int cacheId,
				Datum key1, Datum key2, Datum key3, Datum key4)
{
	const PgPlannerCallbacks *cb = pgplanner_get_callbacks();

	switch (cacheId)
	{
		case OPERNAMENSP:
		{
			if (cb && cb->get_operator)
			{
				const char *opname = NameStr(*DatumGetName(key1));
				Oid			left_type = DatumGetObjectId(key2);
				Oid			right_type = DatumGetObjectId(key3);
				/* key4 is namespace — we ignore it, our callback doesn't filter by namespace */
				PgPlannerOperatorInfo *oinfo = cb->get_operator(opname, left_type, right_type);

				if (oinfo == NULL)
					return NULL;
				return pgplanner_build_operator_tuple(oinfo->oprid, oinfo);
			}
			break;
		}
		default:
			break;
	}

	elog(ERROR, "Unsupported cache lookup4: cacheId=%d", cacheId);
	return NULL;
}


/*
 * ReleaseSysCache
 *		In pgplanner mode, tuples are palloc'd fakes, so this is a no-op.
 */
void
ReleaseSysCache(HeapTuple tuple)
{
	/* no-op: our fake tuples are palloc'd, not catcache entries */
}

/*
 * SearchSysCacheCopy
 */
HeapTuple
SearchSysCacheCopy(int cacheId,
				   Datum key1,
				   Datum key2,
				   Datum key3,
				   Datum key4)
{
	HeapTuple	tuple,
				newtuple;

	tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tuple))
		return tuple;
	newtuple = heap_copytuple(tuple);
	ReleaseSysCache(tuple);
	return newtuple;
}

/*
 * SearchSysCacheExists
 */
bool
SearchSysCacheExists(int cacheId,
					 Datum key1,
					 Datum key2,
					 Datum key3,
					 Datum key4)
{
	HeapTuple	tuple;

	tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tuple))
		return false;
	ReleaseSysCache(tuple);
	return true;
}

/*
 * GetSysCacheOid
 *
 * In pgplanner mode, all our fake tuples have the OID as the first field
 * at offset 0 in GETSTRUCT.  We extract it directly instead of using
 * heap_getattr which would require a TupleDesc.
 */
Oid
GetSysCacheOid(int cacheId,
			   AttrNumber oidcol,
			   Datum key1,
			   Datum key2,
			   Datum key3,
			   Datum key4)
{
	HeapTuple	tuple;
	Oid			result;

	tuple = SearchSysCache(cacheId, key1, key2, key3, key4);
	if (!HeapTupleIsValid(tuple))
		return InvalidOid;

	/* For our fake tuples, OID is always the first field at GETSTRUCT offset */
	result = *(Oid *) GETSTRUCT(tuple);
	ReleaseSysCache(tuple);
	return result;
}


/*
 * SearchSysCacheAttName
 */
HeapTuple
SearchSysCacheAttName(Oid relid, const char *attname)
{
	HeapTuple	tuple;

	tuple = SearchSysCache2(ATTNAME,
							ObjectIdGetDatum(relid),
							CStringGetDatum(attname));
	if (!HeapTupleIsValid(tuple))
		return NULL;
	if (((Form_pg_attribute) GETSTRUCT(tuple))->attisdropped)
	{
		ReleaseSysCache(tuple);
		return NULL;
	}
	return tuple;
}

HeapTuple
SearchSysCacheCopyAttName(Oid relid, const char *attname)
{
	HeapTuple	tuple,
				newtuple;

	tuple = SearchSysCacheAttName(relid, attname);
	if (!HeapTupleIsValid(tuple))
		return tuple;
	newtuple = heap_copytuple(tuple);
	ReleaseSysCache(tuple);
	return newtuple;
}

bool
SearchSysCacheExistsAttName(Oid relid, const char *attname)
{
	HeapTuple	tuple;

	tuple = SearchSysCacheAttName(relid, attname);
	if (!HeapTupleIsValid(tuple))
		return false;
	ReleaseSysCache(tuple);
	return true;
}


HeapTuple
SearchSysCacheAttNum(Oid relid, int16 attnum)
{
	HeapTuple	tuple;

	tuple = SearchSysCache2(ATTNUM,
							ObjectIdGetDatum(relid),
							Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tuple))
		return NULL;
	if (((Form_pg_attribute) GETSTRUCT(tuple))->attisdropped)
	{
		ReleaseSysCache(tuple);
		return NULL;
	}
	return tuple;
}

HeapTuple
SearchSysCacheCopyAttNum(Oid relid, int16 attnum)
{
	HeapTuple	tuple,
				newtuple;

	tuple = SearchSysCacheAttNum(relid, attnum);
	if (!HeapTupleIsValid(tuple))
		return NULL;
	newtuple = heap_copytuple(tuple);
	ReleaseSysCache(tuple);
	return newtuple;
}


/*
 * SysCacheGetAttr
 *
 * In pgplanner mode, we don't have TupleDescs for the fake tuples, so we
 * handle known attribute requests directly.  Variable-length/nullable
 * attributes that the planner queries are handled as special cases.
 */
Datum
SysCacheGetAttr(int cacheId, HeapTuple tup,
				AttrNumber attributeNumber,
				bool *isNull)
{
	/*
	 * Handle known variable-length / nullable attribute lookups that the
	 * planner code actually performs on our fake tuples.
	 */
	switch (cacheId)
	{
		case AGGFNOID:
		{
			/* Anum_pg_aggregate_agginitval = 21 */
			if (attributeNumber == Anum_pg_aggregate_agginitval)
			{
				FormData_pg_aggregate *aggForm = (FormData_pg_aggregate *) GETSTRUCT(tup);
				const PgPlannerCallbacks *cb = pgplanner_get_callbacks();

				if (cb && cb->get_aggregate)
				{
					PgPlannerAggregateInfo *ainfo = cb->get_aggregate(aggForm->aggfnoid);

					if (ainfo && ainfo->agginitval)
					{
						*isNull = false;
						return CStringGetTextDatum(ainfo->agginitval);
					}
				}
				*isNull = true;
				return (Datum) 0;
			}
			break;
		}

		case PROCOID:
		case PROCNAMEARGSNSP:
		{
			/*
			 * The planner may request these nullable pg_proc attributes:
			 *   Anum_pg_proc_proallargtypes (21)
			 *   Anum_pg_proc_proargmodes    (22)
			 *   Anum_pg_proc_proargnames    (23)
			 *   Anum_pg_proc_proargdefaults (24)
			 * All are NULL for our simple function entries.
			 */
			if (attributeNumber >= Anum_pg_proc_proallargtypes &&
				attributeNumber <= Anum_pg_proc_proargdefaults)
			{
				*isNull = true;
				return (Datum) 0;
			}
			break;
		}

		default:
			break;
	}

	elog(ERROR, "SysCacheGetAttr: unsupported cacheId=%d, attr=%d", cacheId, attributeNumber);
	*isNull = true;
	return (Datum) 0;
}

/*
 * GetSysCacheHashValue
 *		Not supported in pgplanner mode.
 */
uint32
GetSysCacheHashValue(int cacheId,
					 Datum key1,
					 Datum key2,
					 Datum key3,
					 Datum key4)
{
	if (!CacheInitialized)
		return 0;

	if (cacheId < 0 || cacheId >= SysCacheSize ||
		!PointerIsValid(SysCache[cacheId]))
		elog(ERROR, "invalid cache ID: %d", cacheId);

	return GetCatCacheHashValue(SysCache[cacheId], key1, key2, key3, key4);
}

/*
 * SearchSysCacheList
 *
 * Handles PROCNAMEARGSNSP: lookup all pg_proc entries matching a function name.
 * Builds a fake CatCList with fake CatCTup members wrapping fake pg_proc HeapTuples.
 */
struct catclist *
SearchSysCacheList(int cacheId, int nkeys,
				   Datum key1, Datum key2, Datum key3)
{
	const PgPlannerCallbacks *cb = pgplanner_get_callbacks();

	switch (cacheId)
	{
		case PROCNAMEARGSNSP:
		{
			if (cb && cb->get_func_candidates && cb->get_function)
			{
				const char *funcname = NameStr(*DatumGetName(key1));
				PgPlannerFuncCandidate *candidates = NULL;
				int			ncandidates;
				CatCList   *clist;
				Size		clist_size;
				int			i;

				ncandidates = cb->get_func_candidates(funcname, &candidates);

				/*
				 * Allocate a CatCList with room for ncandidates member pointers.
				 * Use offsetof + array size for the flexible array member.
				 */
				clist_size = offsetof(CatCList, members) + ncandidates * sizeof(CatCTup *);
				clist = (CatCList *) palloc0(clist_size);
				clist->cl_magic = CL_MAGIC;
				clist->refcount = 2;	/* prevent ReleaseCatCacheList from freeing */
				clist->dead = false;
				clist->ordered = false;
				clist->nkeys = nkeys;
				clist->n_members = ncandidates;
				clist->my_cache = NULL;	/* marks as our fake list */

				for (i = 0; i < ncandidates; i++)
				{
					PgPlannerFuncCandidate *cand = &candidates[i];
					PgPlannerFunctionInfo *finfo = cb->get_function(cand->oid);
					HeapTuple	fakeTuple;
					CatCTup	   *ct;
					Size		ct_size;

					/*
					 * If the callback can't provide full function info, build a
					 * minimal one from the candidate.
					 */
					PgPlannerFunctionInfo tmpinfo;
					if (finfo == NULL)
					{
						memset(&tmpinfo, 0, sizeof(tmpinfo));
						tmpinfo.pronargs = cand->nargs;
						tmpinfo.proargtypes = cand->argtypes;
						tmpinfo.provariadic = cand->variadic_type;
						tmpinfo.pronargdefaults = cand->ndargs;
						tmpinfo.proname = funcname;
						tmpinfo.prokind = 'f';
						finfo = &tmpinfo;
					}

					fakeTuple = pgplanner_build_proc_tuple(cand->oid, finfo);

					/*
					 * Build a CatCTup that wraps this tuple.  We allocate it
					 * with enough trailing space for the tuple data, then copy
					 * the tuple data right after the struct.
					 */
					ct_size = sizeof(CatCTup) + MAXALIGN(fakeTuple->t_len);
					ct = (CatCTup *) palloc0(ct_size);
					ct->ct_magic = CT_MAGIC;
					ct->refcount = 2;
					ct->dead = false;
					ct->negative = false;
					ct->c_list = clist;
					ct->my_cache = NULL;

					/* Set up the inline HeapTupleData to point to the data right after the struct */
					ct->tuple.t_len = fakeTuple->t_len;
					ct->tuple.t_self = fakeTuple->t_data->t_ctid;
					ct->tuple.t_tableOid = InvalidOid;
					ct->tuple.t_data = (HeapTupleHeader) ((char *) ct + sizeof(CatCTup));
					memcpy(ct->tuple.t_data, fakeTuple->t_data, fakeTuple->t_len);

					/* Free the intermediate fake tuple */
					pfree(fakeTuple->t_data);
					pfree(fakeTuple);

					clist->members[i] = ct;
				}

				return clist;
			}
			break;
		}
		default:
			break;
	}

	elog(ERROR, "SearchSysCacheList: unsupported cacheId=%d", cacheId);
	return NULL;
}

/*
 * ReleaseSysCacheList
 *
 * For our fake CatCLists (my_cache == NULL), just decrement refcount.
 * For real catcache lists, delegate to ReleaseCatCacheList.
 */
void
ReleaseSysCacheList(struct catclist *list)
{
	if (list->my_cache == NULL)
	{
		/* Our fake list — just decrement refcount */
		Assert(list->cl_magic == CL_MAGIC);
		list->refcount--;
		return;
	}

	ReleaseCatCacheList(list);
}

/*
 * SysCacheInvalidate
 */
void
SysCacheInvalidate(int cacheId, uint32 hashValue)
{
	if (!CacheInitialized)
		return;

	if (cacheId < 0 || cacheId >= SysCacheSize)
		elog(ERROR, "invalid cache ID: %d", cacheId);

	if (!PointerIsValid(SysCache[cacheId]))
		return;

	CatCacheInvalidate(SysCache[cacheId], hashValue);
}

bool
RelationInvalidatesSnapshotsOnly(Oid relid)
{
	switch (relid)
	{
		case DbRoleSettingRelationId:
		case DependRelationId:
		case SharedDependRelationId:
		case DescriptionRelationId:
		case SharedDescriptionRelationId:
		case SecLabelRelationId:
		case SharedSecLabelRelationId:
			return true;
		default:
			break;
	}

	return false;
}

bool
RelationHasSysCache(Oid relid)
{
	if (!CacheInitialized)
		return false;

	{
		int			low = 0,
					high = SysCacheRelationOidSize - 1;

		while (low <= high)
		{
			int			middle = low + (high - low) / 2;

			if (SysCacheRelationOid[middle] == relid)
				return true;
			if (SysCacheRelationOid[middle] < relid)
				low = middle + 1;
			else
				high = middle - 1;
		}
	}

	return false;
}

bool
RelationSupportsSysCache(Oid relid)
{
	if (!CacheInitialized)
		return false;

	{
		int			low = 0,
					high = SysCacheSupportingRelOidSize - 1;

		while (low <= high)
		{
			int			middle = low + (high - low) / 2;

			if (SysCacheSupportingRelOid[middle] == relid)
				return true;
			if (SysCacheSupportingRelOid[middle] < relid)
				low = middle + 1;
			else
				high = middle - 1;
		}
	}

	return false;
}


static int
oid_compare(const void *a, const void *b)
{
	Oid			oa = *((const Oid *) a);
	Oid			ob = *((const Oid *) b);

	if (oa == ob)
		return 0;
	return (oa > ob) ? 1 : -1;
}
