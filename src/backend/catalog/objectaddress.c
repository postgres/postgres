/*-------------------------------------------------------------------------
 *
 * objectaddress.c
 *	  functions for working with ObjectAddresses
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/objectaddress.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/policy.h"
#include "commands/proclang.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteSupport.h"
#include "storage/large_object.h"
#include "storage/lmgr.h"
#include "storage/sinval.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/syscache.h"

/*
 * ObjectProperty
 *
 * This array provides a common part of system object structure; to help
 * consolidate routines to handle various kind of object classes.
 */
typedef struct
{
	Oid			class_oid;		/* oid of catalog */
	Oid			oid_index_oid;	/* oid of index on system oid column */
	int			oid_catcache_id;	/* id of catcache on system oid column	*/
	int			name_catcache_id;	/* id of catcache on (name,namespace), or
									 * (name) if the object does not live in a
									 * namespace */
	AttrNumber	attnum_oid;		/* attribute number of oid column */
	AttrNumber	attnum_name;	/* attnum of name field */
	AttrNumber	attnum_namespace;	/* attnum of namespace field */
	AttrNumber	attnum_owner;	/* attnum of owner field */
	AttrNumber	attnum_acl;		/* attnum of acl field */
	ObjectType	objtype;		/* OBJECT_* of this object type */
	bool		is_nsp_name_unique; /* can the nsp/name combination (or name
									 * alone, if there's no namespace) be
									 * considered a unique identifier for an
									 * object of this class? */
} ObjectPropertyType;

static const ObjectPropertyType ObjectProperty[] =
{
	{
		AccessMethodRelationId,
		AmOidIndexId,
		AMOID,
		AMNAME,
		Anum_pg_am_oid,
		Anum_pg_am_amname,
		InvalidAttrNumber,
		InvalidAttrNumber,
		InvalidAttrNumber,
		-1,
		true
	},
	{
		CastRelationId,
		CastOidIndexId,
		-1,
		-1,
		Anum_pg_cast_oid,
		InvalidAttrNumber,
		InvalidAttrNumber,
		InvalidAttrNumber,
		InvalidAttrNumber,
		-1,
		false
	},
	{
		CollationRelationId,
		CollationOidIndexId,
		COLLOID,
		-1,						/* COLLNAMEENCNSP also takes encoding */
		Anum_pg_collation_oid,
		Anum_pg_collation_collname,
		Anum_pg_collation_collnamespace,
		Anum_pg_collation_collowner,
		InvalidAttrNumber,
		OBJECT_COLLATION,
		true
	},
	{
		ConstraintRelationId,
		ConstraintOidIndexId,
		CONSTROID,
		-1,
		Anum_pg_constraint_oid,
		Anum_pg_constraint_conname,
		Anum_pg_constraint_connamespace,
		InvalidAttrNumber,
		InvalidAttrNumber,
		-1,
		false
	},
	{
		ConversionRelationId,
		ConversionOidIndexId,
		CONVOID,
		CONNAMENSP,
		Anum_pg_conversion_oid,
		Anum_pg_conversion_conname,
		Anum_pg_conversion_connamespace,
		Anum_pg_conversion_conowner,
		InvalidAttrNumber,
		OBJECT_CONVERSION,
		true
	},
	{
		DatabaseRelationId,
		DatabaseOidIndexId,
		DATABASEOID,
		-1,
		Anum_pg_database_oid,
		Anum_pg_database_datname,
		InvalidAttrNumber,
		Anum_pg_database_datdba,
		Anum_pg_database_datacl,
		OBJECT_DATABASE,
		true
	},
	{
		ExtensionRelationId,
		ExtensionOidIndexId,
		-1,
		-1,
		Anum_pg_extension_oid,
		Anum_pg_extension_extname,
		InvalidAttrNumber,		/* extension doesn't belong to extnamespace */
		Anum_pg_extension_extowner,
		InvalidAttrNumber,
		OBJECT_EXTENSION,
		true
	},
	{
		ForeignDataWrapperRelationId,
		ForeignDataWrapperOidIndexId,
		FOREIGNDATAWRAPPEROID,
		FOREIGNDATAWRAPPERNAME,
		Anum_pg_foreign_data_wrapper_oid,
		Anum_pg_foreign_data_wrapper_fdwname,
		InvalidAttrNumber,
		Anum_pg_foreign_data_wrapper_fdwowner,
		Anum_pg_foreign_data_wrapper_fdwacl,
		OBJECT_FDW,
		true
	},
	{
		ForeignServerRelationId,
		ForeignServerOidIndexId,
		FOREIGNSERVEROID,
		FOREIGNSERVERNAME,
		Anum_pg_foreign_server_oid,
		Anum_pg_foreign_server_srvname,
		InvalidAttrNumber,
		Anum_pg_foreign_server_srvowner,
		Anum_pg_foreign_server_srvacl,
		OBJECT_FOREIGN_SERVER,
		true
	},
	{
		ProcedureRelationId,
		ProcedureOidIndexId,
		PROCOID,
		-1,						/* PROCNAMEARGSNSP also takes argument types */
		Anum_pg_proc_oid,
		Anum_pg_proc_proname,
		Anum_pg_proc_pronamespace,
		Anum_pg_proc_proowner,
		Anum_pg_proc_proacl,
		OBJECT_FUNCTION,
		false
	},
	{
		LanguageRelationId,
		LanguageOidIndexId,
		LANGOID,
		LANGNAME,
		Anum_pg_language_oid,
		Anum_pg_language_lanname,
		InvalidAttrNumber,
		Anum_pg_language_lanowner,
		Anum_pg_language_lanacl,
		OBJECT_LANGUAGE,
		true
	},
	{
		LargeObjectMetadataRelationId,
		LargeObjectMetadataOidIndexId,
		-1,
		-1,
		Anum_pg_largeobject_metadata_oid,
		InvalidAttrNumber,
		InvalidAttrNumber,
		Anum_pg_largeobject_metadata_lomowner,
		Anum_pg_largeobject_metadata_lomacl,
		OBJECT_LARGEOBJECT,
		false
	},
	{
		OperatorClassRelationId,
		OpclassOidIndexId,
		CLAOID,
		-1,						/* CLAAMNAMENSP also takes opcmethod */
		Anum_pg_opclass_oid,
		Anum_pg_opclass_opcname,
		Anum_pg_opclass_opcnamespace,
		Anum_pg_opclass_opcowner,
		InvalidAttrNumber,
		OBJECT_OPCLASS,
		true
	},
	{
		OperatorRelationId,
		OperatorOidIndexId,
		OPEROID,
		-1,						/* OPERNAMENSP also takes left and right type */
		Anum_pg_operator_oid,
		Anum_pg_operator_oprname,
		Anum_pg_operator_oprnamespace,
		Anum_pg_operator_oprowner,
		InvalidAttrNumber,
		OBJECT_OPERATOR,
		false
	},
	{
		OperatorFamilyRelationId,
		OpfamilyOidIndexId,
		OPFAMILYOID,
		-1,						/* OPFAMILYAMNAMENSP also takes opfmethod */
		Anum_pg_opfamily_oid,
		Anum_pg_opfamily_opfname,
		Anum_pg_opfamily_opfnamespace,
		Anum_pg_opfamily_opfowner,
		InvalidAttrNumber,
		OBJECT_OPFAMILY,
		true
	},
	{
		AuthIdRelationId,
		AuthIdOidIndexId,
		AUTHOID,
		AUTHNAME,
		Anum_pg_authid_oid,
		Anum_pg_authid_rolname,
		InvalidAttrNumber,
		InvalidAttrNumber,
		InvalidAttrNumber,
		-1,
		true
	},
	{
		RewriteRelationId,
		RewriteOidIndexId,
		-1,
		-1,
		Anum_pg_rewrite_oid,
		Anum_pg_rewrite_rulename,
		InvalidAttrNumber,
		InvalidAttrNumber,
		InvalidAttrNumber,
		-1,
		false
	},
	{
		NamespaceRelationId,
		NamespaceOidIndexId,
		NAMESPACEOID,
		NAMESPACENAME,
		Anum_pg_namespace_oid,
		Anum_pg_namespace_nspname,
		InvalidAttrNumber,
		Anum_pg_namespace_nspowner,
		Anum_pg_namespace_nspacl,
		OBJECT_SCHEMA,
		true
	},
	{
		RelationRelationId,
		ClassOidIndexId,
		RELOID,
		RELNAMENSP,
		Anum_pg_class_oid,
		Anum_pg_class_relname,
		Anum_pg_class_relnamespace,
		Anum_pg_class_relowner,
		Anum_pg_class_relacl,
		OBJECT_TABLE,
		true
	},
	{
		TableSpaceRelationId,
		TablespaceOidIndexId,
		TABLESPACEOID,
		-1,
		Anum_pg_tablespace_oid,
		Anum_pg_tablespace_spcname,
		InvalidAttrNumber,
		Anum_pg_tablespace_spcowner,
		Anum_pg_tablespace_spcacl,
		OBJECT_TABLESPACE,
		true
	},
	{
		TransformRelationId,
		TransformOidIndexId,
		TRFOID,
		InvalidAttrNumber,
		Anum_pg_transform_oid
	},
	{
		TriggerRelationId,
		TriggerOidIndexId,
		-1,
		-1,
		Anum_pg_trigger_oid,
		Anum_pg_trigger_tgname,
		InvalidAttrNumber,
		InvalidAttrNumber,
		InvalidAttrNumber,
		-1,
		false
	},
	{
		PolicyRelationId,
		PolicyOidIndexId,
		-1,
		-1,
		Anum_pg_policy_oid,
		Anum_pg_policy_polname,
		InvalidAttrNumber,
		InvalidAttrNumber,
		InvalidAttrNumber,
		-1,
		false
	},
	{
		EventTriggerRelationId,
		EventTriggerOidIndexId,
		EVENTTRIGGEROID,
		EVENTTRIGGERNAME,
		Anum_pg_event_trigger_oid,
		Anum_pg_event_trigger_evtname,
		InvalidAttrNumber,
		Anum_pg_event_trigger_evtowner,
		InvalidAttrNumber,
		OBJECT_EVENT_TRIGGER,
		true
	},
	{
		TSConfigRelationId,
		TSConfigOidIndexId,
		TSCONFIGOID,
		TSCONFIGNAMENSP,
		Anum_pg_ts_config_oid,
		Anum_pg_ts_config_cfgname,
		Anum_pg_ts_config_cfgnamespace,
		Anum_pg_ts_config_cfgowner,
		InvalidAttrNumber,
		OBJECT_TSCONFIGURATION,
		true
	},
	{
		TSDictionaryRelationId,
		TSDictionaryOidIndexId,
		TSDICTOID,
		TSDICTNAMENSP,
		Anum_pg_ts_dict_oid,
		Anum_pg_ts_dict_dictname,
		Anum_pg_ts_dict_dictnamespace,
		Anum_pg_ts_dict_dictowner,
		InvalidAttrNumber,
		OBJECT_TSDICTIONARY,
		true
	},
	{
		TSParserRelationId,
		TSParserOidIndexId,
		TSPARSEROID,
		TSPARSERNAMENSP,
		Anum_pg_ts_parser_oid,
		Anum_pg_ts_parser_prsname,
		Anum_pg_ts_parser_prsnamespace,
		InvalidAttrNumber,
		InvalidAttrNumber,
		-1,
		true
	},
	{
		TSTemplateRelationId,
		TSTemplateOidIndexId,
		TSTEMPLATEOID,
		TSTEMPLATENAMENSP,
		Anum_pg_ts_template_oid,
		Anum_pg_ts_template_tmplname,
		Anum_pg_ts_template_tmplnamespace,
		InvalidAttrNumber,
		InvalidAttrNumber,
		-1,
		true,
	},
	{
		TypeRelationId,
		TypeOidIndexId,
		TYPEOID,
		TYPENAMENSP,
		Anum_pg_type_oid,
		Anum_pg_type_typname,
		Anum_pg_type_typnamespace,
		Anum_pg_type_typowner,
		Anum_pg_type_typacl,
		OBJECT_TYPE,
		true
	},
	{
		PublicationRelationId,
		PublicationObjectIndexId,
		PUBLICATIONOID,
		PUBLICATIONNAME,
		Anum_pg_publication_oid,
		Anum_pg_publication_pubname,
		InvalidAttrNumber,
		Anum_pg_publication_pubowner,
		InvalidAttrNumber,
		OBJECT_PUBLICATION,
		true
	},
	{
		SubscriptionRelationId,
		SubscriptionObjectIndexId,
		SUBSCRIPTIONOID,
		SUBSCRIPTIONNAME,
		Anum_pg_subscription_oid,
		Anum_pg_subscription_subname,
		InvalidAttrNumber,
		Anum_pg_subscription_subowner,
		InvalidAttrNumber,
		OBJECT_SUBSCRIPTION,
		true
	},
	{
		StatisticExtRelationId,
		StatisticExtOidIndexId,
		STATEXTOID,
		STATEXTNAMENSP,
		Anum_pg_statistic_ext_oid,
		Anum_pg_statistic_ext_stxname,
		Anum_pg_statistic_ext_stxnamespace,
		Anum_pg_statistic_ext_stxowner,
		InvalidAttrNumber,		/* no ACL (same as relation) */
		OBJECT_STATISTIC_EXT,
		true
	}
};

/*
 * This struct maps the string object types as returned by
 * getObjectTypeDescription into ObjType enum values.  Note that some enum
 * values can be obtained by different names, and that some string object types
 * do not have corresponding values in the output enum.  The user of this map
 * must be careful to test for invalid values being returned.
 *
 * To ease maintenance, this follows the order of getObjectTypeDescription.
 */
static const struct object_type_map
{
	const char *tm_name;
	ObjectType	tm_type;
}

			ObjectTypeMap[] =
{
	/* OCLASS_CLASS, all kinds of relations */
	{
		"table", OBJECT_TABLE
	},
	{
		"index", OBJECT_INDEX
	},
	{
		"sequence", OBJECT_SEQUENCE
	},
	{
		"toast table", -1
	},							/* unmapped */
	{
		"view", OBJECT_VIEW
	},
	{
		"materialized view", OBJECT_MATVIEW
	},
	{
		"composite type", -1
	},							/* unmapped */
	{
		"foreign table", OBJECT_FOREIGN_TABLE
	},
	{
		"table column", OBJECT_COLUMN
	},
	{
		"index column", -1
	},							/* unmapped */
	{
		"sequence column", -1
	},							/* unmapped */
	{
		"toast table column", -1
	},							/* unmapped */
	{
		"view column", -1
	},							/* unmapped */
	{
		"materialized view column", -1
	},							/* unmapped */
	{
		"composite type column", -1
	},							/* unmapped */
	{
		"foreign table column", OBJECT_COLUMN
	},
	/* OCLASS_PROC */
	{
		"aggregate", OBJECT_AGGREGATE
	},
	{
		"function", OBJECT_FUNCTION
	},
	{
		"procedure", OBJECT_PROCEDURE
	},
	/* OCLASS_TYPE */
	{
		"type", OBJECT_TYPE
	},
	/* OCLASS_CAST */
	{
		"cast", OBJECT_CAST
	},
	/* OCLASS_COLLATION */
	{
		"collation", OBJECT_COLLATION
	},
	/* OCLASS_CONSTRAINT */
	{
		"table constraint", OBJECT_TABCONSTRAINT
	},
	{
		"domain constraint", OBJECT_DOMCONSTRAINT
	},
	/* OCLASS_CONVERSION */
	{
		"conversion", OBJECT_CONVERSION
	},
	/* OCLASS_DEFAULT */
	{
		"default value", OBJECT_DEFAULT
	},
	/* OCLASS_LANGUAGE */
	{
		"language", OBJECT_LANGUAGE
	},
	/* OCLASS_LARGEOBJECT */
	{
		"large object", OBJECT_LARGEOBJECT
	},
	/* OCLASS_OPERATOR */
	{
		"operator", OBJECT_OPERATOR
	},
	/* OCLASS_OPCLASS */
	{
		"operator class", OBJECT_OPCLASS
	},
	/* OCLASS_OPFAMILY */
	{
		"operator family", OBJECT_OPFAMILY
	},
	/* OCLASS_AM */
	{
		"access method", OBJECT_ACCESS_METHOD
	},
	/* OCLASS_AMOP */
	{
		"operator of access method", OBJECT_AMOP
	},
	/* OCLASS_AMPROC */
	{
		"function of access method", OBJECT_AMPROC
	},
	/* OCLASS_REWRITE */
	{
		"rule", OBJECT_RULE
	},
	/* OCLASS_TRIGGER */
	{
		"trigger", OBJECT_TRIGGER
	},
	/* OCLASS_SCHEMA */
	{
		"schema", OBJECT_SCHEMA
	},
	/* OCLASS_TSPARSER */
	{
		"text search parser", OBJECT_TSPARSER
	},
	/* OCLASS_TSDICT */
	{
		"text search dictionary", OBJECT_TSDICTIONARY
	},
	/* OCLASS_TSTEMPLATE */
	{
		"text search template", OBJECT_TSTEMPLATE
	},
	/* OCLASS_TSCONFIG */
	{
		"text search configuration", OBJECT_TSCONFIGURATION
	},
	/* OCLASS_ROLE */
	{
		"role", OBJECT_ROLE
	},
	/* OCLASS_DATABASE */
	{
		"database", OBJECT_DATABASE
	},
	/* OCLASS_TBLSPACE */
	{
		"tablespace", OBJECT_TABLESPACE
	},
	/* OCLASS_FDW */
	{
		"foreign-data wrapper", OBJECT_FDW
	},
	/* OCLASS_FOREIGN_SERVER */
	{
		"server", OBJECT_FOREIGN_SERVER
	},
	/* OCLASS_USER_MAPPING */
	{
		"user mapping", OBJECT_USER_MAPPING
	},
	/* OCLASS_DEFACL */
	{
		"default acl", OBJECT_DEFACL
	},
	/* OCLASS_EXTENSION */
	{
		"extension", OBJECT_EXTENSION
	},
	/* OCLASS_EVENT_TRIGGER */
	{
		"event trigger", OBJECT_EVENT_TRIGGER
	},
	/* OCLASS_POLICY */
	{
		"policy", OBJECT_POLICY
	},
	/* OCLASS_PUBLICATION */
	{
		"publication", OBJECT_PUBLICATION
	},
	/* OCLASS_PUBLICATION_REL */
	{
		"publication relation", OBJECT_PUBLICATION_REL
	},
	/* OCLASS_SUBSCRIPTION */
	{
		"subscription", OBJECT_SUBSCRIPTION
	},
	/* OCLASS_TRANSFORM */
	{
		"transform", OBJECT_TRANSFORM
	},
	/* OCLASS_STATISTIC_EXT */
	{
		"statistics object", OBJECT_STATISTIC_EXT
	}
};

const ObjectAddress InvalidObjectAddress =
{
	InvalidOid,
	InvalidOid,
	0
};

static ObjectAddress get_object_address_unqualified(ObjectType objtype,
													Value *strval, bool missing_ok);
static ObjectAddress get_relation_by_qualified_name(ObjectType objtype,
													List *object, Relation *relp,
													LOCKMODE lockmode, bool missing_ok);
static ObjectAddress get_object_address_relobject(ObjectType objtype,
												  List *object, Relation *relp, bool missing_ok);
static ObjectAddress get_object_address_attribute(ObjectType objtype,
												  List *object, Relation *relp,
												  LOCKMODE lockmode, bool missing_ok);
static ObjectAddress get_object_address_attrdef(ObjectType objtype,
												List *object, Relation *relp, LOCKMODE lockmode,
												bool missing_ok);
static ObjectAddress get_object_address_type(ObjectType objtype,
											 TypeName *typename, bool missing_ok);
static ObjectAddress get_object_address_opcf(ObjectType objtype, List *object,
											 bool missing_ok);
static ObjectAddress get_object_address_opf_member(ObjectType objtype,
												   List *object, bool missing_ok);

static ObjectAddress get_object_address_usermapping(List *object,
													bool missing_ok);
static ObjectAddress get_object_address_publication_rel(List *object,
														Relation *relp,
														bool missing_ok);
static ObjectAddress get_object_address_defacl(List *object,
											   bool missing_ok);
static const ObjectPropertyType *get_object_property_data(Oid class_id);

static void getRelationDescription(StringInfo buffer, Oid relid);
static void getOpFamilyDescription(StringInfo buffer, Oid opfid);
static void getRelationTypeDescription(StringInfo buffer, Oid relid,
									   int32 objectSubId);
static void getProcedureTypeDescription(StringInfo buffer, Oid procid);
static void getConstraintTypeDescription(StringInfo buffer, Oid constroid);
static void getOpFamilyIdentity(StringInfo buffer, Oid opfid, List **object);
static void getRelationIdentity(StringInfo buffer, Oid relid, List **object);

/*
 * Translate an object name and arguments (as passed by the parser) to an
 * ObjectAddress.
 *
 * The returned object will be locked using the specified lockmode.  If a
 * sub-object is looked up, the parent object will be locked instead.
 *
 * If the object is a relation or a child object of a relation (e.g. an
 * attribute or constraint), the relation is also opened and *relp receives
 * the open relcache entry pointer; otherwise, *relp is set to NULL.  This
 * is a bit grotty but it makes life simpler, since the caller will
 * typically need the relcache entry too.  Caller must close the relcache
 * entry when done with it.  The relation is locked with the specified lockmode
 * if the target object is the relation itself or an attribute, but for other
 * child objects, only AccessShareLock is acquired on the relation.
 *
 * If the object is not found, an error is thrown, unless missing_ok is
 * true.  In this case, no lock is acquired, relp is set to NULL, and the
 * returned address has objectId set to InvalidOid.
 *
 * We don't currently provide a function to release the locks acquired here;
 * typically, the lock must be held until commit to guard against a concurrent
 * drop operation.
 *
 * Note: If the object is not found, we don't give any indication of the
 * reason.  (It might have been a missing schema if the name was qualified, or
 * a nonexistent type name in case of a cast, function or operator; etc).
 * Currently there is only one caller that might be interested in such info, so
 * we don't spend much effort here.  If more callers start to care, it might be
 * better to add some support for that in this function.
 */
ObjectAddress
get_object_address(ObjectType objtype, Node *object,
				   Relation *relp, LOCKMODE lockmode, bool missing_ok)
{
	ObjectAddress address;
	ObjectAddress old_address = {InvalidOid, InvalidOid, 0};
	Relation	relation = NULL;
	uint64		inval_count;

	/* Some kind of lock must be taken. */
	Assert(lockmode != NoLock);

	for (;;)
	{
		/*
		 * Remember this value, so that, after looking up the object name and
		 * locking it, we can check whether any invalidation messages have
		 * been processed that might require a do-over.
		 */
		inval_count = SharedInvalidMessageCounter;

		/* Look up object address. */
		switch (objtype)
		{
			case OBJECT_INDEX:
			case OBJECT_SEQUENCE:
			case OBJECT_TABLE:
			case OBJECT_VIEW:
			case OBJECT_MATVIEW:
			case OBJECT_FOREIGN_TABLE:
				address =
					get_relation_by_qualified_name(objtype, castNode(List, object),
												   &relation, lockmode,
												   missing_ok);
				break;
			case OBJECT_COLUMN:
				address =
					get_object_address_attribute(objtype, castNode(List, object),
												 &relation, lockmode,
												 missing_ok);
				break;
			case OBJECT_DEFAULT:
				address =
					get_object_address_attrdef(objtype, castNode(List, object),
											   &relation, lockmode,
											   missing_ok);
				break;
			case OBJECT_RULE:
			case OBJECT_TRIGGER:
			case OBJECT_TABCONSTRAINT:
			case OBJECT_POLICY:
				address = get_object_address_relobject(objtype, castNode(List, object),
													   &relation, missing_ok);
				break;
			case OBJECT_DOMCONSTRAINT:
				{
					List	   *objlist;
					ObjectAddress domaddr;
					char	   *constrname;

					objlist = castNode(List, object);
					domaddr = get_object_address_type(OBJECT_DOMAIN,
													  linitial_node(TypeName, objlist),
													  missing_ok);
					constrname = strVal(lsecond(objlist));

					address.classId = ConstraintRelationId;
					address.objectId = get_domain_constraint_oid(domaddr.objectId,
																 constrname, missing_ok);
					address.objectSubId = 0;

				}
				break;
			case OBJECT_DATABASE:
			case OBJECT_EXTENSION:
			case OBJECT_TABLESPACE:
			case OBJECT_ROLE:
			case OBJECT_SCHEMA:
			case OBJECT_LANGUAGE:
			case OBJECT_FDW:
			case OBJECT_FOREIGN_SERVER:
			case OBJECT_EVENT_TRIGGER:
			case OBJECT_ACCESS_METHOD:
			case OBJECT_PUBLICATION:
			case OBJECT_SUBSCRIPTION:
				address = get_object_address_unqualified(objtype,
														 (Value *) object, missing_ok);
				break;
			case OBJECT_TYPE:
			case OBJECT_DOMAIN:
				address = get_object_address_type(objtype, castNode(TypeName, object), missing_ok);
				break;
			case OBJECT_AGGREGATE:
			case OBJECT_FUNCTION:
			case OBJECT_PROCEDURE:
			case OBJECT_ROUTINE:
				address.classId = ProcedureRelationId;
				address.objectId = LookupFuncWithArgs(objtype, castNode(ObjectWithArgs, object), missing_ok);
				address.objectSubId = 0;
				break;
			case OBJECT_OPERATOR:
				address.classId = OperatorRelationId;
				address.objectId = LookupOperWithArgs(castNode(ObjectWithArgs, object), missing_ok);
				address.objectSubId = 0;
				break;
			case OBJECT_COLLATION:
				address.classId = CollationRelationId;
				address.objectId = get_collation_oid(castNode(List, object), missing_ok);
				address.objectSubId = 0;
				break;
			case OBJECT_CONVERSION:
				address.classId = ConversionRelationId;
				address.objectId = get_conversion_oid(castNode(List, object), missing_ok);
				address.objectSubId = 0;
				break;
			case OBJECT_OPCLASS:
			case OBJECT_OPFAMILY:
				address = get_object_address_opcf(objtype, castNode(List, object), missing_ok);
				break;
			case OBJECT_AMOP:
			case OBJECT_AMPROC:
				address = get_object_address_opf_member(objtype, castNode(List, object), missing_ok);
				break;
			case OBJECT_LARGEOBJECT:
				address.classId = LargeObjectRelationId;
				address.objectId = oidparse(object);
				address.objectSubId = 0;
				if (!LargeObjectExists(address.objectId))
				{
					if (!missing_ok)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
								 errmsg("large object %u does not exist",
										address.objectId)));
				}
				break;
			case OBJECT_CAST:
				{
					TypeName   *sourcetype = linitial_node(TypeName, castNode(List, object));
					TypeName   *targettype = lsecond_node(TypeName, castNode(List, object));
					Oid			sourcetypeid;
					Oid			targettypeid;

					sourcetypeid = LookupTypeNameOid(NULL, sourcetype, missing_ok);
					targettypeid = LookupTypeNameOid(NULL, targettype, missing_ok);
					address.classId = CastRelationId;
					address.objectId =
						get_cast_oid(sourcetypeid, targettypeid, missing_ok);
					address.objectSubId = 0;
				}
				break;
			case OBJECT_TRANSFORM:
				{
					TypeName   *typename = linitial_node(TypeName, castNode(List, object));
					char	   *langname = strVal(lsecond(castNode(List, object)));
					Oid			type_id = LookupTypeNameOid(NULL, typename, missing_ok);
					Oid			lang_id = get_language_oid(langname, missing_ok);

					address.classId = TransformRelationId;
					address.objectId =
						get_transform_oid(type_id, lang_id, missing_ok);
					address.objectSubId = 0;
				}
				break;
			case OBJECT_TSPARSER:
				address.classId = TSParserRelationId;
				address.objectId = get_ts_parser_oid(castNode(List, object), missing_ok);
				address.objectSubId = 0;
				break;
			case OBJECT_TSDICTIONARY:
				address.classId = TSDictionaryRelationId;
				address.objectId = get_ts_dict_oid(castNode(List, object), missing_ok);
				address.objectSubId = 0;
				break;
			case OBJECT_TSTEMPLATE:
				address.classId = TSTemplateRelationId;
				address.objectId = get_ts_template_oid(castNode(List, object), missing_ok);
				address.objectSubId = 0;
				break;
			case OBJECT_TSCONFIGURATION:
				address.classId = TSConfigRelationId;
				address.objectId = get_ts_config_oid(castNode(List, object), missing_ok);
				address.objectSubId = 0;
				break;
			case OBJECT_USER_MAPPING:
				address = get_object_address_usermapping(castNode(List, object),
														 missing_ok);
				break;
			case OBJECT_PUBLICATION_REL:
				address = get_object_address_publication_rel(castNode(List, object),
															 &relation,
															 missing_ok);
				break;
			case OBJECT_DEFACL:
				address = get_object_address_defacl(castNode(List, object),
													missing_ok);
				break;
			case OBJECT_STATISTIC_EXT:
				address.classId = StatisticExtRelationId;
				address.objectId = get_statistics_object_oid(castNode(List, object),
															 missing_ok);
				address.objectSubId = 0;
				break;
			default:
				elog(ERROR, "unrecognized objtype: %d", (int) objtype);
				/* placate compiler, in case it thinks elog might return */
				address.classId = InvalidOid;
				address.objectId = InvalidOid;
				address.objectSubId = 0;
		}

		/*
		 * If we could not find the supplied object, return without locking.
		 */
		if (!OidIsValid(address.objectId))
		{
			Assert(missing_ok);
			return address;
		}

		/*
		 * If we're retrying, see if we got the same answer as last time.  If
		 * so, we're done; if not, we locked the wrong thing, so give up our
		 * lock.
		 */
		if (OidIsValid(old_address.classId))
		{
			if (old_address.classId == address.classId
				&& old_address.objectId == address.objectId
				&& old_address.objectSubId == address.objectSubId)
				break;
			if (old_address.classId != RelationRelationId)
			{
				if (IsSharedRelation(old_address.classId))
					UnlockSharedObject(old_address.classId,
									   old_address.objectId,
									   0, lockmode);
				else
					UnlockDatabaseObject(old_address.classId,
										 old_address.objectId,
										 0, lockmode);
			}
		}

		/*
		 * If we're dealing with a relation or attribute, then the relation is
		 * already locked.  Otherwise, we lock it now.
		 */
		if (address.classId != RelationRelationId)
		{
			if (IsSharedRelation(address.classId))
				LockSharedObject(address.classId, address.objectId, 0,
								 lockmode);
			else
				LockDatabaseObject(address.classId, address.objectId, 0,
								   lockmode);
		}

		/*
		 * At this point, we've resolved the name to an OID and locked the
		 * corresponding database object.  However, it's possible that by the
		 * time we acquire the lock on the object, concurrent DDL has modified
		 * the database in such a way that the name we originally looked up no
		 * longer resolves to that OID.
		 *
		 * We can be certain that this isn't an issue if (a) no shared
		 * invalidation messages have been processed or (b) we've locked a
		 * relation somewhere along the line.  All the relation name lookups
		 * in this module ultimately use RangeVarGetRelid() to acquire a
		 * relation lock, and that function protects against the same kinds of
		 * races we're worried about here.  Even when operating on a
		 * constraint, rule, or trigger, we still acquire AccessShareLock on
		 * the relation, which is enough to freeze out any concurrent DDL.
		 *
		 * In all other cases, however, it's possible that the name we looked
		 * up no longer refers to the object we locked, so we retry the lookup
		 * and see whether we get the same answer.
		 */
		if (inval_count == SharedInvalidMessageCounter || relation != NULL)
			break;
		old_address = address;
	}

	/* Return the object address and the relation. */
	*relp = relation;
	return address;
}

/*
 * Return an ObjectAddress based on a RangeVar and an object name. The
 * name of the relation identified by the RangeVar is prepended to the
 * (possibly empty) list passed in as object. This is useful to find
 * the ObjectAddress of objects that depend on a relation. All other
 * considerations are exactly as for get_object_address above.
 */
ObjectAddress
get_object_address_rv(ObjectType objtype, RangeVar *rel, List *object,
					  Relation *relp, LOCKMODE lockmode,
					  bool missing_ok)
{
	if (rel)
	{
		object = lcons(makeString(rel->relname), object);
		if (rel->schemaname)
			object = lcons(makeString(rel->schemaname), object);
		if (rel->catalogname)
			object = lcons(makeString(rel->catalogname), object);
	}

	return get_object_address(objtype, (Node *) object,
							  relp, lockmode, missing_ok);
}

/*
 * Find an ObjectAddress for a type of object that is identified by an
 * unqualified name.
 */
static ObjectAddress
get_object_address_unqualified(ObjectType objtype,
							   Value *strval, bool missing_ok)
{
	const char *name;
	ObjectAddress address;

	name = strVal(strval);

	/* Translate name to OID. */
	switch (objtype)
	{
		case OBJECT_ACCESS_METHOD:
			address.classId = AccessMethodRelationId;
			address.objectId = get_am_oid(name, missing_ok);
			address.objectSubId = 0;
			break;
		case OBJECT_DATABASE:
			address.classId = DatabaseRelationId;
			address.objectId = get_database_oid(name, missing_ok);
			address.objectSubId = 0;
			break;
		case OBJECT_EXTENSION:
			address.classId = ExtensionRelationId;
			address.objectId = get_extension_oid(name, missing_ok);
			address.objectSubId = 0;
			break;
		case OBJECT_TABLESPACE:
			address.classId = TableSpaceRelationId;
			address.objectId = get_tablespace_oid(name, missing_ok);
			address.objectSubId = 0;
			break;
		case OBJECT_ROLE:
			address.classId = AuthIdRelationId;
			address.objectId = get_role_oid(name, missing_ok);
			address.objectSubId = 0;
			break;
		case OBJECT_SCHEMA:
			address.classId = NamespaceRelationId;
			address.objectId = get_namespace_oid(name, missing_ok);
			address.objectSubId = 0;
			break;
		case OBJECT_LANGUAGE:
			address.classId = LanguageRelationId;
			address.objectId = get_language_oid(name, missing_ok);
			address.objectSubId = 0;
			break;
		case OBJECT_FDW:
			address.classId = ForeignDataWrapperRelationId;
			address.objectId = get_foreign_data_wrapper_oid(name, missing_ok);
			address.objectSubId = 0;
			break;
		case OBJECT_FOREIGN_SERVER:
			address.classId = ForeignServerRelationId;
			address.objectId = get_foreign_server_oid(name, missing_ok);
			address.objectSubId = 0;
			break;
		case OBJECT_EVENT_TRIGGER:
			address.classId = EventTriggerRelationId;
			address.objectId = get_event_trigger_oid(name, missing_ok);
			address.objectSubId = 0;
			break;
		case OBJECT_PUBLICATION:
			address.classId = PublicationRelationId;
			address.objectId = get_publication_oid(name, missing_ok);
			address.objectSubId = 0;
			break;
		case OBJECT_SUBSCRIPTION:
			address.classId = SubscriptionRelationId;
			address.objectId = get_subscription_oid(name, missing_ok);
			address.objectSubId = 0;
			break;
		default:
			elog(ERROR, "unrecognized objtype: %d", (int) objtype);
			/* placate compiler, which doesn't know elog won't return */
			address.classId = InvalidOid;
			address.objectId = InvalidOid;
			address.objectSubId = 0;
	}

	return address;
}

/*
 * Locate a relation by qualified name.
 */
static ObjectAddress
get_relation_by_qualified_name(ObjectType objtype, List *object,
							   Relation *relp, LOCKMODE lockmode,
							   bool missing_ok)
{
	Relation	relation;
	ObjectAddress address;

	address.classId = RelationRelationId;
	address.objectId = InvalidOid;
	address.objectSubId = 0;

	relation = relation_openrv_extended(makeRangeVarFromNameList(object),
										lockmode, missing_ok);
	if (!relation)
		return address;

	switch (objtype)
	{
		case OBJECT_INDEX:
			if (relation->rd_rel->relkind != RELKIND_INDEX &&
				relation->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not an index",
								RelationGetRelationName(relation))));
			break;
		case OBJECT_SEQUENCE:
			if (relation->rd_rel->relkind != RELKIND_SEQUENCE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a sequence",
								RelationGetRelationName(relation))));
			break;
		case OBJECT_TABLE:
			if (relation->rd_rel->relkind != RELKIND_RELATION &&
				relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a table",
								RelationGetRelationName(relation))));
			break;
		case OBJECT_VIEW:
			if (relation->rd_rel->relkind != RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a view",
								RelationGetRelationName(relation))));
			break;
		case OBJECT_MATVIEW:
			if (relation->rd_rel->relkind != RELKIND_MATVIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a materialized view",
								RelationGetRelationName(relation))));
			break;
		case OBJECT_FOREIGN_TABLE:
			if (relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a foreign table",
								RelationGetRelationName(relation))));
			break;
		default:
			elog(ERROR, "unrecognized objtype: %d", (int) objtype);
			break;
	}

	/* Done. */
	address.objectId = RelationGetRelid(relation);
	*relp = relation;

	return address;
}

/*
 * Find object address for an object that is attached to a relation.
 *
 * Note that we take only an AccessShareLock on the relation.  We need not
 * pass down the LOCKMODE from get_object_address(), because that is the lock
 * mode for the object itself, not the relation to which it is attached.
 */
static ObjectAddress
get_object_address_relobject(ObjectType objtype, List *object,
							 Relation *relp, bool missing_ok)
{
	ObjectAddress address;
	Relation	relation = NULL;
	int			nnames;
	const char *depname;
	List	   *relname;
	Oid			reloid;

	/* Extract name of dependent object. */
	depname = strVal(llast(object));

	/* Separate relation name from dependent object name. */
	nnames = list_length(object);
	if (nnames < 2)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("must specify relation and object name")));

	/* Extract relation name and open relation. */
	relname = list_truncate(list_copy(object), nnames - 1);
	relation = table_openrv_extended(makeRangeVarFromNameList(relname),
									 AccessShareLock,
									 missing_ok);

	reloid = relation ? RelationGetRelid(relation) : InvalidOid;

	switch (objtype)
	{
		case OBJECT_RULE:
			address.classId = RewriteRelationId;
			address.objectId = relation ?
				get_rewrite_oid(reloid, depname, missing_ok) : InvalidOid;
			address.objectSubId = 0;
			break;
		case OBJECT_TRIGGER:
			address.classId = TriggerRelationId;
			address.objectId = relation ?
				get_trigger_oid(reloid, depname, missing_ok) : InvalidOid;
			address.objectSubId = 0;
			break;
		case OBJECT_TABCONSTRAINT:
			address.classId = ConstraintRelationId;
			address.objectId = relation ?
				get_relation_constraint_oid(reloid, depname, missing_ok) :
				InvalidOid;
			address.objectSubId = 0;
			break;
		case OBJECT_POLICY:
			address.classId = PolicyRelationId;
			address.objectId = relation ?
				get_relation_policy_oid(reloid, depname, missing_ok) :
				InvalidOid;
			address.objectSubId = 0;
			break;
		default:
			elog(ERROR, "unrecognized objtype: %d", (int) objtype);
	}

	/* Avoid relcache leak when object not found. */
	if (!OidIsValid(address.objectId))
	{
		if (relation != NULL)
			table_close(relation, AccessShareLock);

		relation = NULL;		/* department of accident prevention */
		return address;
	}

	/* Done. */
	*relp = relation;
	return address;
}

/*
 * Find the ObjectAddress for an attribute.
 */
static ObjectAddress
get_object_address_attribute(ObjectType objtype, List *object,
							 Relation *relp, LOCKMODE lockmode,
							 bool missing_ok)
{
	ObjectAddress address;
	List	   *relname;
	Oid			reloid;
	Relation	relation;
	const char *attname;
	AttrNumber	attnum;

	/* Extract relation name and open relation. */
	if (list_length(object) < 2)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("column name must be qualified")));
	attname = strVal(lfirst(list_tail(object)));
	relname = list_truncate(list_copy(object), list_length(object) - 1);
	/* XXX no missing_ok support here */
	relation = relation_openrv(makeRangeVarFromNameList(relname), lockmode);
	reloid = RelationGetRelid(relation);

	/* Look up attribute and construct return value. */
	attnum = get_attnum(reloid, attname);
	if (attnum == InvalidAttrNumber)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" of relation \"%s\" does not exist",
							attname, NameListToString(relname))));

		address.classId = RelationRelationId;
		address.objectId = InvalidOid;
		address.objectSubId = InvalidAttrNumber;
		relation_close(relation, lockmode);
		return address;
	}

	address.classId = RelationRelationId;
	address.objectId = reloid;
	address.objectSubId = attnum;

	*relp = relation;
	return address;
}

/*
 * Find the ObjectAddress for an attribute's default value.
 */
static ObjectAddress
get_object_address_attrdef(ObjectType objtype, List *object,
						   Relation *relp, LOCKMODE lockmode,
						   bool missing_ok)
{
	ObjectAddress address;
	List	   *relname;
	Oid			reloid;
	Relation	relation;
	const char *attname;
	AttrNumber	attnum;
	TupleDesc	tupdesc;
	Oid			defoid;

	/* Extract relation name and open relation. */
	if (list_length(object) < 2)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("column name must be qualified")));
	attname = strVal(llast(object));
	relname = list_truncate(list_copy(object), list_length(object) - 1);
	/* XXX no missing_ok support here */
	relation = relation_openrv(makeRangeVarFromNameList(relname), lockmode);
	reloid = RelationGetRelid(relation);

	tupdesc = RelationGetDescr(relation);

	/* Look up attribute number and scan pg_attrdef to find its tuple */
	attnum = get_attnum(reloid, attname);
	defoid = InvalidOid;
	if (attnum != InvalidAttrNumber && tupdesc->constr != NULL)
	{
		Relation	attrdef;
		ScanKeyData keys[2];
		SysScanDesc scan;
		HeapTuple	tup;

		attrdef = relation_open(AttrDefaultRelationId, AccessShareLock);
		ScanKeyInit(&keys[0],
					Anum_pg_attrdef_adrelid,
					BTEqualStrategyNumber,
					F_OIDEQ,
					ObjectIdGetDatum(reloid));
		ScanKeyInit(&keys[1],
					Anum_pg_attrdef_adnum,
					BTEqualStrategyNumber,
					F_INT2EQ,
					Int16GetDatum(attnum));
		scan = systable_beginscan(attrdef, AttrDefaultIndexId, true,
								  NULL, 2, keys);
		if (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_attrdef atdform = (Form_pg_attrdef) GETSTRUCT(tup);

			defoid = atdform->oid;
		}

		systable_endscan(scan);
		relation_close(attrdef, AccessShareLock);
	}
	if (!OidIsValid(defoid))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("default value for column \"%s\" of relation \"%s\" does not exist",
							attname, NameListToString(relname))));

		address.classId = AttrDefaultRelationId;
		address.objectId = InvalidOid;
		address.objectSubId = InvalidAttrNumber;
		relation_close(relation, lockmode);
		return address;
	}

	address.classId = AttrDefaultRelationId;
	address.objectId = defoid;
	address.objectSubId = 0;

	*relp = relation;
	return address;
}

/*
 * Find the ObjectAddress for a type or domain
 */
static ObjectAddress
get_object_address_type(ObjectType objtype, TypeName *typename, bool missing_ok)
{
	ObjectAddress address;
	Type		tup;

	address.classId = TypeRelationId;
	address.objectId = InvalidOid;
	address.objectSubId = 0;

	tup = LookupTypeName(NULL, typename, NULL, missing_ok);
	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" does not exist",
							TypeNameToString(typename))));
		return address;
	}
	address.objectId = typeTypeId(tup);

	if (objtype == OBJECT_DOMAIN)
	{
		if (((Form_pg_type) GETSTRUCT(tup))->typtype != TYPTYPE_DOMAIN)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a domain",
							TypeNameToString(typename))));
	}

	ReleaseSysCache(tup);

	return address;
}

/*
 * Find the ObjectAddress for an opclass or opfamily.
 */
static ObjectAddress
get_object_address_opcf(ObjectType objtype, List *object, bool missing_ok)
{
	Oid			amoid;
	ObjectAddress address;

	/* XXX no missing_ok support here */
	amoid = get_index_am_oid(strVal(linitial(object)), false);
	object = list_copy_tail(object, 1);

	switch (objtype)
	{
		case OBJECT_OPCLASS:
			address.classId = OperatorClassRelationId;
			address.objectId = get_opclass_oid(amoid, object, missing_ok);
			address.objectSubId = 0;
			break;
		case OBJECT_OPFAMILY:
			address.classId = OperatorFamilyRelationId;
			address.objectId = get_opfamily_oid(amoid, object, missing_ok);
			address.objectSubId = 0;
			break;
		default:
			elog(ERROR, "unrecognized objtype: %d", (int) objtype);
			/* placate compiler, which doesn't know elog won't return */
			address.classId = InvalidOid;
			address.objectId = InvalidOid;
			address.objectSubId = 0;
	}

	return address;
}

/*
 * Find the ObjectAddress for an opclass/opfamily member.
 *
 * (The returned address corresponds to a pg_amop/pg_amproc object).
 */
static ObjectAddress
get_object_address_opf_member(ObjectType objtype,
							  List *object, bool missing_ok)
{
	ObjectAddress famaddr;
	ObjectAddress address;
	ListCell   *cell;
	List	   *copy;
	TypeName   *typenames[2];
	Oid			typeoids[2];
	int			membernum;
	int			i;

	/*
	 * The last element of the object list contains the strategy or procedure
	 * number.  We need to strip that out before getting the opclass/family
	 * address.  The rest can be used directly by get_object_address_opcf().
	 */
	membernum = atoi(strVal(llast(linitial(object))));
	copy = list_truncate(list_copy(linitial(object)), list_length(linitial(object)) - 1);

	/* no missing_ok support here */
	famaddr = get_object_address_opcf(OBJECT_OPFAMILY, copy, false);

	/* find out left/right type names and OIDs */
	typenames[0] = typenames[1] = NULL;
	typeoids[0] = typeoids[1] = InvalidOid;
	i = 0;
	foreach(cell, lsecond(object))
	{
		ObjectAddress typaddr;

		typenames[i] = lfirst_node(TypeName, cell);
		typaddr = get_object_address_type(OBJECT_TYPE, typenames[i], missing_ok);
		typeoids[i] = typaddr.objectId;
		if (++i >= 2)
			break;
	}

	switch (objtype)
	{
		case OBJECT_AMOP:
			{
				HeapTuple	tp;

				ObjectAddressSet(address, AccessMethodOperatorRelationId,
								 InvalidOid);

				tp = SearchSysCache4(AMOPSTRATEGY,
									 ObjectIdGetDatum(famaddr.objectId),
									 ObjectIdGetDatum(typeoids[0]),
									 ObjectIdGetDatum(typeoids[1]),
									 Int16GetDatum(membernum));
				if (!HeapTupleIsValid(tp))
				{
					if (!missing_ok)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
								 errmsg("operator %d (%s, %s) of %s does not exist",
										membernum,
										TypeNameToString(typenames[0]),
										TypeNameToString(typenames[1]),
										getObjectDescription(&famaddr))));
				}
				else
				{
					address.objectId = ((Form_pg_amop) GETSTRUCT(tp))->oid;
					ReleaseSysCache(tp);
				}
			}
			break;

		case OBJECT_AMPROC:
			{
				HeapTuple	tp;

				ObjectAddressSet(address, AccessMethodProcedureRelationId,
								 InvalidOid);

				tp = SearchSysCache4(AMPROCNUM,
									 ObjectIdGetDatum(famaddr.objectId),
									 ObjectIdGetDatum(typeoids[0]),
									 ObjectIdGetDatum(typeoids[1]),
									 Int16GetDatum(membernum));
				if (!HeapTupleIsValid(tp))
				{
					if (!missing_ok)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
								 errmsg("function %d (%s, %s) of %s does not exist",
										membernum,
										TypeNameToString(typenames[0]),
										TypeNameToString(typenames[1]),
										getObjectDescription(&famaddr))));
				}
				else
				{
					address.objectId = ((Form_pg_amproc) GETSTRUCT(tp))->oid;
					ReleaseSysCache(tp);
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized objtype: %d", (int) objtype);
	}

	return address;
}

/*
 * Find the ObjectAddress for a user mapping.
 */
static ObjectAddress
get_object_address_usermapping(List *object, bool missing_ok)
{
	ObjectAddress address;
	Oid			userid;
	char	   *username;
	char	   *servername;
	ForeignServer *server;
	HeapTuple	tp;

	ObjectAddressSet(address, UserMappingRelationId, InvalidOid);

	/* fetch string names from input lists, for error messages */
	username = strVal(linitial(object));
	servername = strVal(lsecond(object));

	/* look up pg_authid OID of mapped user; InvalidOid if PUBLIC */
	if (strcmp(username, "public") == 0)
		userid = InvalidOid;
	else
	{
		tp = SearchSysCache1(AUTHNAME,
							 CStringGetDatum(username));
		if (!HeapTupleIsValid(tp))
		{
			if (!missing_ok)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("user mapping for user \"%s\" on server \"%s\" does not exist",
								username, servername)));
			return address;
		}
		userid = ((Form_pg_authid) GETSTRUCT(tp))->oid;
		ReleaseSysCache(tp);
	}

	/* Now look up the pg_user_mapping tuple */
	server = GetForeignServerByName(servername, true);
	if (!server)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("server \"%s\" does not exist", servername)));
		return address;
	}
	tp = SearchSysCache2(USERMAPPINGUSERSERVER,
						 ObjectIdGetDatum(userid),
						 ObjectIdGetDatum(server->serverid));
	if (!HeapTupleIsValid(tp))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("user mapping for user \"%s\" on server \"%s\" does not exist",
							username, servername)));
		return address;
	}

	address.objectId = ((Form_pg_user_mapping) GETSTRUCT(tp))->oid;

	ReleaseSysCache(tp);

	return address;
}

/*
 * Find the ObjectAddress for a publication relation.  The first element of
 * the object parameter is the relation name, the second is the
 * publication name.
 */
static ObjectAddress
get_object_address_publication_rel(List *object,
								   Relation *relp, bool missing_ok)
{
	ObjectAddress address;
	Relation	relation;
	List	   *relname;
	char	   *pubname;
	Publication *pub;

	ObjectAddressSet(address, PublicationRelRelationId, InvalidOid);

	relname = linitial(object);
	relation = relation_openrv_extended(makeRangeVarFromNameList(relname),
										AccessShareLock, missing_ok);
	if (!relation)
		return address;

	/* fetch publication name from input list */
	pubname = strVal(lsecond(object));

	/* Now look up the pg_publication tuple */
	pub = GetPublicationByName(pubname, missing_ok);
	if (!pub)
	{
		relation_close(relation, AccessShareLock);
		return address;
	}

	/* Find the publication relation mapping in syscache. */
	address.objectId =
		GetSysCacheOid2(PUBLICATIONRELMAP, Anum_pg_publication_rel_oid,
						ObjectIdGetDatum(RelationGetRelid(relation)),
						ObjectIdGetDatum(pub->oid));
	if (!OidIsValid(address.objectId))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("publication relation \"%s\" in publication \"%s\" does not exist",
							RelationGetRelationName(relation), pubname)));
		relation_close(relation, AccessShareLock);
		return address;
	}

	*relp = relation;
	return address;
}

/*
 * Find the ObjectAddress for a default ACL.
 */
static ObjectAddress
get_object_address_defacl(List *object, bool missing_ok)
{
	HeapTuple	tp;
	Oid			userid;
	Oid			schemaid;
	char	   *username;
	char	   *schema;
	char		objtype;
	char	   *objtype_str;
	ObjectAddress address;

	ObjectAddressSet(address, DefaultAclRelationId, InvalidOid);

	/*
	 * First figure out the textual attributes so that they can be used for
	 * error reporting.
	 */
	username = strVal(lsecond(object));
	if (list_length(object) >= 3)
		schema = (char *) strVal(lthird(object));
	else
		schema = NULL;

	/*
	 * Decode defaclobjtype.  Only first char is considered; the rest of the
	 * string, if any, is blissfully ignored.
	 */
	objtype = ((char *) strVal(linitial(object)))[0];
	switch (objtype)
	{
		case DEFACLOBJ_RELATION:
			objtype_str = "tables";
			break;
		case DEFACLOBJ_SEQUENCE:
			objtype_str = "sequences";
			break;
		case DEFACLOBJ_FUNCTION:
			objtype_str = "functions";
			break;
		case DEFACLOBJ_TYPE:
			objtype_str = "types";
			break;
		case DEFACLOBJ_NAMESPACE:
			objtype_str = "schemas";
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized default ACL object type \"%c\"", objtype),
					 errhint("Valid object types are \"%c\", \"%c\", \"%c\", \"%c\", \"%c\".",
							 DEFACLOBJ_RELATION,
							 DEFACLOBJ_SEQUENCE,
							 DEFACLOBJ_FUNCTION,
							 DEFACLOBJ_TYPE,
							 DEFACLOBJ_NAMESPACE)));
	}

	/*
	 * Look up user ID.  Behave as "default ACL not found" if the user doesn't
	 * exist.
	 */
	tp = SearchSysCache1(AUTHNAME,
						 CStringGetDatum(username));
	if (!HeapTupleIsValid(tp))
		goto not_found;
	userid = ((Form_pg_authid) GETSTRUCT(tp))->oid;
	ReleaseSysCache(tp);

	/*
	 * If a schema name was given, look up its OID.  If it doesn't exist,
	 * behave as "default ACL not found".
	 */
	if (schema)
	{
		schemaid = get_namespace_oid(schema, true);
		if (schemaid == InvalidOid)
			goto not_found;
	}
	else
		schemaid = InvalidOid;

	/* Finally, look up the pg_default_acl object */
	tp = SearchSysCache3(DEFACLROLENSPOBJ,
						 ObjectIdGetDatum(userid),
						 ObjectIdGetDatum(schemaid),
						 CharGetDatum(objtype));
	if (!HeapTupleIsValid(tp))
		goto not_found;

	address.objectId = ((Form_pg_default_acl) GETSTRUCT(tp))->oid;
	ReleaseSysCache(tp);

	return address;

not_found:
	if (!missing_ok)
	{
		if (schema)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("default ACL for user \"%s\" in schema \"%s\" on %s does not exist",
							username, schema, objtype_str)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("default ACL for user \"%s\" on %s does not exist",
							username, objtype_str)));
	}
	return address;
}

/*
 * Convert an array of TEXT into a List of string Values, as emitted by the
 * parser, which is what get_object_address uses as input.
 */
static List *
textarray_to_strvaluelist(ArrayType *arr)
{
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	List	   *list = NIL;
	int			i;

	deconstruct_array(arr, TEXTOID, -1, false, 'i',
					  &elems, &nulls, &nelems);

	for (i = 0; i < nelems; i++)
	{
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("name or argument lists may not contain nulls")));
		list = lappend(list, makeString(TextDatumGetCString(elems[i])));
	}

	return list;
}

/*
 * SQL-callable version of get_object_address
 */
Datum
pg_get_object_address(PG_FUNCTION_ARGS)
{
	char	   *ttype = TextDatumGetCString(PG_GETARG_DATUM(0));
	ArrayType  *namearr = PG_GETARG_ARRAYTYPE_P(1);
	ArrayType  *argsarr = PG_GETARG_ARRAYTYPE_P(2);
	int			itype;
	ObjectType	type;
	List	   *name = NIL;
	TypeName   *typename = NULL;
	List	   *args = NIL;
	Node	   *objnode = NULL;
	ObjectAddress addr;
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3];
	HeapTuple	htup;
	Relation	relation;

	/* Decode object type, raise error if unknown */
	itype = read_objtype_from_string(ttype);
	if (itype < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unsupported object type \"%s\"", ttype)));
	type = (ObjectType) itype;

	/*
	 * Convert the text array to the representation appropriate for the given
	 * object type.  Most use a simple string Values list, but there are some
	 * exceptions.
	 */
	if (type == OBJECT_TYPE || type == OBJECT_DOMAIN || type == OBJECT_CAST ||
		type == OBJECT_TRANSFORM || type == OBJECT_DOMCONSTRAINT)
	{
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;

		deconstruct_array(namearr, TEXTOID, -1, false, 'i',
						  &elems, &nulls, &nelems);
		if (nelems != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("name list length must be exactly %d", 1)));
		if (nulls[0])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("name or argument lists may not contain nulls")));
		typename = typeStringToTypeName(TextDatumGetCString(elems[0]));
	}
	else if (type == OBJECT_LARGEOBJECT)
	{
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;

		deconstruct_array(namearr, TEXTOID, -1, false, 'i',
						  &elems, &nulls, &nelems);
		if (nelems != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("name list length must be exactly %d", 1)));
		if (nulls[0])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("large object OID may not be null")));
		objnode = (Node *) makeFloat(TextDatumGetCString(elems[0]));
	}
	else
	{
		name = textarray_to_strvaluelist(namearr);
		if (list_length(name) < 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("name list length must be at least %d", 1)));
	}

	/*
	 * If args are given, decode them according to the object type.
	 */
	if (type == OBJECT_AGGREGATE ||
		type == OBJECT_FUNCTION ||
		type == OBJECT_PROCEDURE ||
		type == OBJECT_ROUTINE ||
		type == OBJECT_OPERATOR ||
		type == OBJECT_CAST ||
		type == OBJECT_AMOP ||
		type == OBJECT_AMPROC)
	{
		/* in these cases, the args list must be of TypeName */
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;
		int			i;

		deconstruct_array(argsarr, TEXTOID, -1, false, 'i',
						  &elems, &nulls, &nelems);

		args = NIL;
		for (i = 0; i < nelems; i++)
		{
			if (nulls[i])
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("name or argument lists may not contain nulls")));
			args = lappend(args,
						   typeStringToTypeName(TextDatumGetCString(elems[i])));
		}
	}
	else
	{
		/* For all other object types, use string Values */
		args = textarray_to_strvaluelist(argsarr);
	}

	/*
	 * get_object_address is pretty sensitive to the length of its input
	 * lists; check that they're what it wants.
	 */
	switch (type)
	{
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_CAST:
		case OBJECT_USER_MAPPING:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_DEFACL:
		case OBJECT_TRANSFORM:
			if (list_length(args) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument list length must be exactly %d", 1)));
			break;
		case OBJECT_OPFAMILY:
		case OBJECT_OPCLASS:
			if (list_length(name) < 2)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("name list length must be at least %d", 2)));
			break;
		case OBJECT_AMOP:
		case OBJECT_AMPROC:
			if (list_length(name) < 3)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("name list length must be at least %d", 3)));
			/* fall through to check args length */
			/* FALLTHROUGH */
		case OBJECT_OPERATOR:
			if (list_length(args) != 2)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument list length must be exactly %d", 2)));
			break;
		default:
			break;
	}

	/*
	 * Now build the Node type that get_object_address() expects for the given
	 * type.
	 */
	switch (type)
	{
		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
		case OBJECT_INDEX:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_COLUMN:
		case OBJECT_ATTRIBUTE:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_TSPARSER:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSTEMPLATE:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_DEFAULT:
		case OBJECT_POLICY:
		case OBJECT_RULE:
		case OBJECT_TRIGGER:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_OPCLASS:
		case OBJECT_OPFAMILY:
			objnode = (Node *) name;
			break;
		case OBJECT_ACCESS_METHOD:
		case OBJECT_DATABASE:
		case OBJECT_EVENT_TRIGGER:
		case OBJECT_EXTENSION:
		case OBJECT_FDW:
		case OBJECT_FOREIGN_SERVER:
		case OBJECT_LANGUAGE:
		case OBJECT_PUBLICATION:
		case OBJECT_ROLE:
		case OBJECT_SCHEMA:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABLESPACE:
			if (list_length(name) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("name list length must be exactly %d", 1)));
			objnode = linitial(name);
			break;
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
			objnode = (Node *) typename;
			break;
		case OBJECT_CAST:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_TRANSFORM:
			objnode = (Node *) list_make2(typename, linitial(args));
			break;
		case OBJECT_PUBLICATION_REL:
			objnode = (Node *) list_make2(name, linitial(args));
			break;
		case OBJECT_USER_MAPPING:
			objnode = (Node *) list_make2(linitial(name), linitial(args));
			break;
		case OBJECT_DEFACL:
			objnode = (Node *) lcons(linitial(args), name);
			break;
		case OBJECT_AMOP:
		case OBJECT_AMPROC:
			objnode = (Node *) list_make2(name, args);
			break;
		case OBJECT_FUNCTION:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
		case OBJECT_AGGREGATE:
		case OBJECT_OPERATOR:
			{
				ObjectWithArgs *owa = makeNode(ObjectWithArgs);

				owa->objname = name;
				owa->objargs = args;
				objnode = (Node *) owa;
				break;
			}
		case OBJECT_LARGEOBJECT:
			/* already handled above */
			break;
			/* no default, to let compiler warn about missing case */
	}

	if (objnode == NULL)
		elog(ERROR, "unrecognized object type: %d", type);

	addr = get_object_address(type, objnode,
							  &relation, AccessShareLock, false);

	/* We don't need the relcache entry, thank you very much */
	if (relation)
		relation_close(relation, AccessShareLock);

	tupdesc = CreateTemplateTupleDesc(3);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "classid",
					   OIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "objid",
					   OIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "objsubid",
					   INT4OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = ObjectIdGetDatum(addr.classId);
	values[1] = ObjectIdGetDatum(addr.objectId);
	values[2] = Int32GetDatum(addr.objectSubId);
	nulls[0] = false;
	nulls[1] = false;
	nulls[2] = false;

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

/*
 * Check ownership of an object previously identified by get_object_address.
 */
void
check_object_ownership(Oid roleid, ObjectType objtype, ObjectAddress address,
					   Node *object, Relation relation)
{
	switch (objtype)
	{
		case OBJECT_INDEX:
		case OBJECT_SEQUENCE:
		case OBJECT_TABLE:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_COLUMN:
		case OBJECT_RULE:
		case OBJECT_TRIGGER:
		case OBJECT_POLICY:
		case OBJECT_TABCONSTRAINT:
			if (!pg_class_ownercheck(RelationGetRelid(relation), roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   RelationGetRelationName(relation));
			break;
		case OBJECT_DATABASE:
			if (!pg_database_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   strVal((Value *) object));
			break;
		case OBJECT_TYPE:
		case OBJECT_DOMAIN:
		case OBJECT_ATTRIBUTE:
			if (!pg_type_ownercheck(address.objectId, roleid))
				aclcheck_error_type(ACLCHECK_NOT_OWNER, address.objectId);
			break;
		case OBJECT_DOMCONSTRAINT:
			{
				HeapTuple	tuple;
				Oid			contypid;

				tuple = SearchSysCache1(CONSTROID,
										ObjectIdGetDatum(address.objectId));
				if (!HeapTupleIsValid(tuple))
					elog(ERROR, "constraint with OID %u does not exist",
						 address.objectId);

				contypid = ((Form_pg_constraint) GETSTRUCT(tuple))->contypid;

				ReleaseSysCache(tuple);

				/*
				 * Fallback to type ownership check in this case as this is
				 * what domain constraints rely on.
				 */
				if (!pg_type_ownercheck(contypid, roleid))
					aclcheck_error_type(ACLCHECK_NOT_OWNER, contypid);
			}
			break;
		case OBJECT_AGGREGATE:
		case OBJECT_FUNCTION:
		case OBJECT_PROCEDURE:
		case OBJECT_ROUTINE:
			if (!pg_proc_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   NameListToString((castNode(ObjectWithArgs, object))->objname));
			break;
		case OBJECT_OPERATOR:
			if (!pg_oper_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   NameListToString((castNode(ObjectWithArgs, object))->objname));
			break;
		case OBJECT_SCHEMA:
			if (!pg_namespace_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   strVal((Value *) object));
			break;
		case OBJECT_COLLATION:
			if (!pg_collation_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   NameListToString(castNode(List, object)));
			break;
		case OBJECT_CONVERSION:
			if (!pg_conversion_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   NameListToString(castNode(List, object)));
			break;
		case OBJECT_EXTENSION:
			if (!pg_extension_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   strVal((Value *) object));
			break;
		case OBJECT_FDW:
			if (!pg_foreign_data_wrapper_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   strVal((Value *) object));
			break;
		case OBJECT_FOREIGN_SERVER:
			if (!pg_foreign_server_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   strVal((Value *) object));
			break;
		case OBJECT_EVENT_TRIGGER:
			if (!pg_event_trigger_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   strVal((Value *) object));
			break;
		case OBJECT_LANGUAGE:
			if (!pg_language_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   strVal((Value *) object));
			break;
		case OBJECT_OPCLASS:
			if (!pg_opclass_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   NameListToString(castNode(List, object)));
			break;
		case OBJECT_OPFAMILY:
			if (!pg_opfamily_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   NameListToString(castNode(List, object)));
			break;
		case OBJECT_LARGEOBJECT:
			if (!lo_compat_privileges &&
				!pg_largeobject_ownercheck(address.objectId, roleid))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("must be owner of large object %u",
								address.objectId)));
			break;
		case OBJECT_CAST:
			{
				/* We can only check permissions on the source/target types */
				TypeName   *sourcetype = linitial_node(TypeName, castNode(List, object));
				TypeName   *targettype = lsecond_node(TypeName, castNode(List, object));
				Oid			sourcetypeid = typenameTypeId(NULL, sourcetype);
				Oid			targettypeid = typenameTypeId(NULL, targettype);

				if (!pg_type_ownercheck(sourcetypeid, roleid)
					&& !pg_type_ownercheck(targettypeid, roleid))
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("must be owner of type %s or type %s",
									format_type_be(sourcetypeid),
									format_type_be(targettypeid))));
			}
			break;
		case OBJECT_PUBLICATION:
			if (!pg_publication_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   strVal((Value *) object));
			break;
		case OBJECT_SUBSCRIPTION:
			if (!pg_subscription_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   strVal((Value *) object));
			break;
		case OBJECT_TRANSFORM:
			{
				TypeName   *typename = linitial_node(TypeName, castNode(List, object));
				Oid			typeid = typenameTypeId(NULL, typename);

				if (!pg_type_ownercheck(typeid, roleid))
					aclcheck_error_type(ACLCHECK_NOT_OWNER, typeid);
			}
			break;
		case OBJECT_TABLESPACE:
			if (!pg_tablespace_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   strVal((Value *) object));
			break;
		case OBJECT_TSDICTIONARY:
			if (!pg_ts_dict_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   NameListToString(castNode(List, object)));
			break;
		case OBJECT_TSCONFIGURATION:
			if (!pg_ts_config_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   NameListToString(castNode(List, object)));
			break;
		case OBJECT_ROLE:

			/*
			 * We treat roles as being "owned" by those with CREATEROLE priv,
			 * except that superusers are only owned by superusers.
			 */
			if (superuser_arg(address.objectId))
			{
				if (!superuser_arg(roleid))
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("must be superuser")));
			}
			else
			{
				if (!has_createrole_privilege(roleid))
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("must have CREATEROLE privilege")));
			}
			break;
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_ACCESS_METHOD:
			/* We treat these object types as being owned by superusers */
			if (!superuser_arg(roleid))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("must be superuser")));
			break;
		case OBJECT_STATISTIC_EXT:
			if (!pg_statistics_object_ownercheck(address.objectId, roleid))
				aclcheck_error(ACLCHECK_NOT_OWNER, objtype,
							   NameListToString(castNode(List, object)));
			break;
		default:
			elog(ERROR, "unrecognized object type: %d",
				 (int) objtype);
	}
}

/*
 * get_object_namespace
 *
 * Find the schema containing the specified object.  For non-schema objects,
 * this function returns InvalidOid.
 */
Oid
get_object_namespace(const ObjectAddress *address)
{
	int			cache;
	HeapTuple	tuple;
	bool		isnull;
	Oid			oid;
	const ObjectPropertyType *property;

	/* If not owned by a namespace, just return InvalidOid. */
	property = get_object_property_data(address->classId);
	if (property->attnum_namespace == InvalidAttrNumber)
		return InvalidOid;

	/* Currently, we can only handle object types with system caches. */
	cache = property->oid_catcache_id;
	Assert(cache != -1);

	/* Fetch tuple from syscache and extract namespace attribute. */
	tuple = SearchSysCache1(cache, ObjectIdGetDatum(address->objectId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for cache %d oid %u",
			 cache, address->objectId);
	oid = DatumGetObjectId(SysCacheGetAttr(cache,
										   tuple,
										   property->attnum_namespace,
										   &isnull));
	Assert(!isnull);
	ReleaseSysCache(tuple);

	return oid;
}

/*
 * Return ObjectType for the given object type as given by
 * getObjectTypeDescription; if no valid ObjectType code exists, but it's a
 * possible output type from getObjectTypeDescription, return -1.
 * Otherwise, an error is thrown.
 */
int
read_objtype_from_string(const char *objtype)
{
	int			i;

	for (i = 0; i < lengthof(ObjectTypeMap); i++)
	{
		if (strcmp(ObjectTypeMap[i].tm_name, objtype) == 0)
			return ObjectTypeMap[i].tm_type;
	}
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized object type \"%s\"", objtype)));

	return -1;					/* keep compiler quiet */
}

/*
 * Interfaces to reference fields of ObjectPropertyType
 */
Oid
get_object_oid_index(Oid class_id)
{
	const ObjectPropertyType *prop = get_object_property_data(class_id);

	return prop->oid_index_oid;
}

int
get_object_catcache_oid(Oid class_id)
{
	const ObjectPropertyType *prop = get_object_property_data(class_id);

	return prop->oid_catcache_id;
}

int
get_object_catcache_name(Oid class_id)
{
	const ObjectPropertyType *prop = get_object_property_data(class_id);

	return prop->name_catcache_id;
}

AttrNumber
get_object_attnum_oid(Oid class_id)
{
	const ObjectPropertyType *prop = get_object_property_data(class_id);

	return prop->attnum_oid;
}

AttrNumber
get_object_attnum_name(Oid class_id)
{
	const ObjectPropertyType *prop = get_object_property_data(class_id);

	return prop->attnum_name;
}

AttrNumber
get_object_attnum_namespace(Oid class_id)
{
	const ObjectPropertyType *prop = get_object_property_data(class_id);

	return prop->attnum_namespace;
}

AttrNumber
get_object_attnum_owner(Oid class_id)
{
	const ObjectPropertyType *prop = get_object_property_data(class_id);

	return prop->attnum_owner;
}

AttrNumber
get_object_attnum_acl(Oid class_id)
{
	const ObjectPropertyType *prop = get_object_property_data(class_id);

	return prop->attnum_acl;
}

/*
 * get_object_type
 *
 * Return the object type associated with a given object.  This routine
 * is primarily used to determine the object type to mention in ACL check
 * error messages, so it's desirable for it to avoid failing.
 */
ObjectType
get_object_type(Oid class_id, Oid object_id)
{
	const ObjectPropertyType *prop = get_object_property_data(class_id);

	if (prop->objtype == OBJECT_TABLE)
	{
		/*
		 * If the property data says it's a table, dig a little deeper to get
		 * the real relation kind, so that callers can produce more precise
		 * error messages.
		 */
		return get_relkind_objtype(get_rel_relkind(object_id));
	}
	else
		return prop->objtype;
}

bool
get_object_namensp_unique(Oid class_id)
{
	const ObjectPropertyType *prop = get_object_property_data(class_id);

	return prop->is_nsp_name_unique;
}

/*
 * Return whether we have useful data for the given object class in the
 * ObjectProperty table.
 */
bool
is_objectclass_supported(Oid class_id)
{
	int			index;

	for (index = 0; index < lengthof(ObjectProperty); index++)
	{
		if (ObjectProperty[index].class_oid == class_id)
			return true;
	}

	return false;
}

/*
 * Find ObjectProperty structure by class_id.
 */
static const ObjectPropertyType *
get_object_property_data(Oid class_id)
{
	static const ObjectPropertyType *prop_last = NULL;
	int			index;

	/*
	 * A shortcut to speed up multiple consecutive lookups of a particular
	 * object class.
	 */
	if (prop_last && prop_last->class_oid == class_id)
		return prop_last;

	for (index = 0; index < lengthof(ObjectProperty); index++)
	{
		if (ObjectProperty[index].class_oid == class_id)
		{
			prop_last = &ObjectProperty[index];
			return &ObjectProperty[index];
		}
	}

	ereport(ERROR,
			(errmsg_internal("unrecognized class ID: %u", class_id)));

	return NULL;				/* keep MSC compiler happy */
}

/*
 * Return a copy of the tuple for the object with the given object OID, from
 * the given catalog (which must have been opened by the caller and suitably
 * locked).  NULL is returned if the OID is not found.
 *
 * We try a syscache first, if available.
 */
HeapTuple
get_catalog_object_by_oid(Relation catalog, AttrNumber oidcol, Oid objectId)
{
	HeapTuple	tuple;
	Oid			classId = RelationGetRelid(catalog);
	int			oidCacheId = get_object_catcache_oid(classId);

	if (oidCacheId > 0)
	{
		tuple = SearchSysCacheCopy1(oidCacheId, ObjectIdGetDatum(objectId));
		if (!HeapTupleIsValid(tuple))	/* should not happen */
			return NULL;
	}
	else
	{
		Oid			oidIndexId = get_object_oid_index(classId);
		SysScanDesc scan;
		ScanKeyData skey;

		Assert(OidIsValid(oidIndexId));

		ScanKeyInit(&skey,
					oidcol,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(objectId));

		scan = systable_beginscan(catalog, oidIndexId, true,
								  NULL, 1, &skey);
		tuple = systable_getnext(scan);
		if (!HeapTupleIsValid(tuple))
		{
			systable_endscan(scan);
			return NULL;
		}
		tuple = heap_copytuple(tuple);

		systable_endscan(scan);
	}

	return tuple;
}

/*
 * getObjectDescription: build an object description for messages
 *
 * The result is a palloc'd string.
 */
char *
getObjectDescription(const ObjectAddress *object)
{
	StringInfoData buffer;

	initStringInfo(&buffer);

	switch (getObjectClass(object))
	{
		case OCLASS_CLASS:
			if (object->objectSubId == 0)
				getRelationDescription(&buffer, object->objectId);
			else
			{
				/* column, not whole relation */
				StringInfoData rel;

				initStringInfo(&rel);
				getRelationDescription(&rel, object->objectId);
				/* translator: second %s is, e.g., "table %s" */
				appendStringInfo(&buffer, _("column %s of %s"),
								 get_attname(object->objectId,
											 object->objectSubId,
											 false),
								 rel.data);
				pfree(rel.data);
			}
			break;

		case OCLASS_PROC:
			appendStringInfo(&buffer, _("function %s"),
							 format_procedure(object->objectId));
			break;

		case OCLASS_TYPE:
			appendStringInfo(&buffer, _("type %s"),
							 format_type_be(object->objectId));
			break;

		case OCLASS_CAST:
			{
				Relation	castDesc;
				ScanKeyData skey[1];
				SysScanDesc rcscan;
				HeapTuple	tup;
				Form_pg_cast castForm;

				castDesc = table_open(CastRelationId, AccessShareLock);

				ScanKeyInit(&skey[0],
							Anum_pg_cast_oid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(object->objectId));

				rcscan = systable_beginscan(castDesc, CastOidIndexId, true,
											NULL, 1, skey);

				tup = systable_getnext(rcscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for cast %u",
						 object->objectId);

				castForm = (Form_pg_cast) GETSTRUCT(tup);

				appendStringInfo(&buffer, _("cast from %s to %s"),
								 format_type_be(castForm->castsource),
								 format_type_be(castForm->casttarget));

				systable_endscan(rcscan);
				table_close(castDesc, AccessShareLock);
				break;
			}

		case OCLASS_COLLATION:
			{
				HeapTuple	collTup;
				Form_pg_collation coll;
				char	   *nspname;

				collTup = SearchSysCache1(COLLOID,
										  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(collTup))
					elog(ERROR, "cache lookup failed for collation %u",
						 object->objectId);
				coll = (Form_pg_collation) GETSTRUCT(collTup);

				/* Qualify the name if not visible in search path */
				if (CollationIsVisible(object->objectId))
					nspname = NULL;
				else
					nspname = get_namespace_name(coll->collnamespace);

				appendStringInfo(&buffer, _("collation %s"),
								 quote_qualified_identifier(nspname,
															NameStr(coll->collname)));
				ReleaseSysCache(collTup);
				break;
			}

		case OCLASS_CONSTRAINT:
			{
				HeapTuple	conTup;
				Form_pg_constraint con;

				conTup = SearchSysCache1(CONSTROID,
										 ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(conTup))
					elog(ERROR, "cache lookup failed for constraint %u",
						 object->objectId);
				con = (Form_pg_constraint) GETSTRUCT(conTup);

				if (OidIsValid(con->conrelid))
				{
					StringInfoData rel;

					initStringInfo(&rel);
					getRelationDescription(&rel, con->conrelid);
					/* translator: second %s is, e.g., "table %s" */
					appendStringInfo(&buffer, _("constraint %s on %s"),
									 NameStr(con->conname), rel.data);
					pfree(rel.data);
				}
				else
				{
					appendStringInfo(&buffer, _("constraint %s"),
									 NameStr(con->conname));
				}

				ReleaseSysCache(conTup);
				break;
			}

		case OCLASS_CONVERSION:
			{
				HeapTuple	conTup;
				Form_pg_conversion conv;
				char	   *nspname;

				conTup = SearchSysCache1(CONVOID,
										 ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(conTup))
					elog(ERROR, "cache lookup failed for conversion %u",
						 object->objectId);
				conv = (Form_pg_conversion) GETSTRUCT(conTup);

				/* Qualify the name if not visible in search path */
				if (ConversionIsVisible(object->objectId))
					nspname = NULL;
				else
					nspname = get_namespace_name(conv->connamespace);

				appendStringInfo(&buffer, _("conversion %s"),
								 quote_qualified_identifier(nspname,
															NameStr(conv->conname)));
				ReleaseSysCache(conTup);
				break;
			}

		case OCLASS_DEFAULT:
			{
				Relation	attrdefDesc;
				ScanKeyData skey[1];
				SysScanDesc adscan;
				HeapTuple	tup;
				Form_pg_attrdef attrdef;
				ObjectAddress colobject;

				attrdefDesc = table_open(AttrDefaultRelationId, AccessShareLock);

				ScanKeyInit(&skey[0],
							Anum_pg_attrdef_oid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(object->objectId));

				adscan = systable_beginscan(attrdefDesc, AttrDefaultOidIndexId,
											true, NULL, 1, skey);

				tup = systable_getnext(adscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for attrdef %u",
						 object->objectId);

				attrdef = (Form_pg_attrdef) GETSTRUCT(tup);

				colobject.classId = RelationRelationId;
				colobject.objectId = attrdef->adrelid;
				colobject.objectSubId = attrdef->adnum;

				/* translator: %s is typically "column %s of table %s" */
				appendStringInfo(&buffer, _("default value for %s"),
								 getObjectDescription(&colobject));

				systable_endscan(adscan);
				table_close(attrdefDesc, AccessShareLock);
				break;
			}

		case OCLASS_LANGUAGE:
			appendStringInfo(&buffer, _("language %s"),
							 get_language_name(object->objectId, false));
			break;

		case OCLASS_LARGEOBJECT:
			appendStringInfo(&buffer, _("large object %u"),
							 object->objectId);
			break;

		case OCLASS_OPERATOR:
			appendStringInfo(&buffer, _("operator %s"),
							 format_operator(object->objectId));
			break;

		case OCLASS_OPCLASS:
			{
				HeapTuple	opcTup;
				Form_pg_opclass opcForm;
				HeapTuple	amTup;
				Form_pg_am	amForm;
				char	   *nspname;

				opcTup = SearchSysCache1(CLAOID,
										 ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(opcTup))
					elog(ERROR, "cache lookup failed for opclass %u",
						 object->objectId);
				opcForm = (Form_pg_opclass) GETSTRUCT(opcTup);

				amTup = SearchSysCache1(AMOID,
										ObjectIdGetDatum(opcForm->opcmethod));
				if (!HeapTupleIsValid(amTup))
					elog(ERROR, "cache lookup failed for access method %u",
						 opcForm->opcmethod);
				amForm = (Form_pg_am) GETSTRUCT(amTup);

				/* Qualify the name if not visible in search path */
				if (OpclassIsVisible(object->objectId))
					nspname = NULL;
				else
					nspname = get_namespace_name(opcForm->opcnamespace);

				appendStringInfo(&buffer, _("operator class %s for access method %s"),
								 quote_qualified_identifier(nspname,
															NameStr(opcForm->opcname)),
								 NameStr(amForm->amname));

				ReleaseSysCache(amTup);
				ReleaseSysCache(opcTup);
				break;
			}

		case OCLASS_OPFAMILY:
			getOpFamilyDescription(&buffer, object->objectId);
			break;

		case OCLASS_AM:
			{
				HeapTuple	tup;

				tup = SearchSysCache1(AMOID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for access method %u",
						 object->objectId);
				appendStringInfo(&buffer, _("access method %s"),
								 NameStr(((Form_pg_am) GETSTRUCT(tup))->amname));
				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_AMOP:
			{
				Relation	amopDesc;
				HeapTuple	tup;
				ScanKeyData skey[1];
				SysScanDesc amscan;
				Form_pg_amop amopForm;
				StringInfoData opfam;

				amopDesc = table_open(AccessMethodOperatorRelationId,
									  AccessShareLock);

				ScanKeyInit(&skey[0],
							Anum_pg_amop_oid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(object->objectId));

				amscan = systable_beginscan(amopDesc, AccessMethodOperatorOidIndexId, true,
											NULL, 1, skey);

				tup = systable_getnext(amscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for amop entry %u",
						 object->objectId);

				amopForm = (Form_pg_amop) GETSTRUCT(tup);

				initStringInfo(&opfam);
				getOpFamilyDescription(&opfam, amopForm->amopfamily);

				/*------
				   translator: %d is the operator strategy (a number), the
				   first two %s's are data type names, the third %s is the
				   description of the operator family, and the last %s is the
				   textual form of the operator with arguments.  */
				appendStringInfo(&buffer, _("operator %d (%s, %s) of %s: %s"),
								 amopForm->amopstrategy,
								 format_type_be(amopForm->amoplefttype),
								 format_type_be(amopForm->amoprighttype),
								 opfam.data,
								 format_operator(amopForm->amopopr));

				pfree(opfam.data);

				systable_endscan(amscan);
				table_close(amopDesc, AccessShareLock);
				break;
			}

		case OCLASS_AMPROC:
			{
				Relation	amprocDesc;
				ScanKeyData skey[1];
				SysScanDesc amscan;
				HeapTuple	tup;
				Form_pg_amproc amprocForm;
				StringInfoData opfam;

				amprocDesc = table_open(AccessMethodProcedureRelationId,
										AccessShareLock);

				ScanKeyInit(&skey[0],
							Anum_pg_amproc_oid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(object->objectId));

				amscan = systable_beginscan(amprocDesc, AccessMethodProcedureOidIndexId, true,
											NULL, 1, skey);

				tup = systable_getnext(amscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for amproc entry %u",
						 object->objectId);

				amprocForm = (Form_pg_amproc) GETSTRUCT(tup);

				initStringInfo(&opfam);
				getOpFamilyDescription(&opfam, amprocForm->amprocfamily);

				/*------
				   translator: %d is the function number, the first two %s's
				   are data type names, the third %s is the description of the
				   operator family, and the last %s is the textual form of the
				   function with arguments.  */
				appendStringInfo(&buffer, _("function %d (%s, %s) of %s: %s"),
								 amprocForm->amprocnum,
								 format_type_be(amprocForm->amproclefttype),
								 format_type_be(amprocForm->amprocrighttype),
								 opfam.data,
								 format_procedure(amprocForm->amproc));

				pfree(opfam.data);

				systable_endscan(amscan);
				table_close(amprocDesc, AccessShareLock);
				break;
			}

		case OCLASS_REWRITE:
			{
				Relation	ruleDesc;
				ScanKeyData skey[1];
				SysScanDesc rcscan;
				HeapTuple	tup;
				Form_pg_rewrite rule;
				StringInfoData rel;

				ruleDesc = table_open(RewriteRelationId, AccessShareLock);

				ScanKeyInit(&skey[0],
							Anum_pg_rewrite_oid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(object->objectId));

				rcscan = systable_beginscan(ruleDesc, RewriteOidIndexId, true,
											NULL, 1, skey);

				tup = systable_getnext(rcscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for rule %u",
						 object->objectId);
				rule = (Form_pg_rewrite) GETSTRUCT(tup);

				initStringInfo(&rel);
				getRelationDescription(&rel, rule->ev_class);

				/* translator: second %s is, e.g., "table %s" */
				appendStringInfo(&buffer, _("rule %s on %s"),
								 NameStr(rule->rulename), rel.data);
				pfree(rel.data);
				systable_endscan(rcscan);
				table_close(ruleDesc, AccessShareLock);
				break;
			}

		case OCLASS_TRIGGER:
			{
				Relation	trigDesc;
				ScanKeyData skey[1];
				SysScanDesc tgscan;
				HeapTuple	tup;
				Form_pg_trigger trig;
				StringInfoData rel;

				trigDesc = table_open(TriggerRelationId, AccessShareLock);

				ScanKeyInit(&skey[0],
							Anum_pg_trigger_oid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(object->objectId));

				tgscan = systable_beginscan(trigDesc, TriggerOidIndexId, true,
											NULL, 1, skey);

				tup = systable_getnext(tgscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for trigger %u",
						 object->objectId);
				trig = (Form_pg_trigger) GETSTRUCT(tup);

				initStringInfo(&rel);
				getRelationDescription(&rel, trig->tgrelid);

				/* translator: second %s is, e.g., "table %s" */
				appendStringInfo(&buffer, _("trigger %s on %s"),
								 NameStr(trig->tgname), rel.data);
				pfree(rel.data);
				systable_endscan(tgscan);
				table_close(trigDesc, AccessShareLock);
				break;
			}

		case OCLASS_SCHEMA:
			{
				char	   *nspname;

				nspname = get_namespace_name(object->objectId);
				if (!nspname)
					elog(ERROR, "cache lookup failed for namespace %u",
						 object->objectId);
				appendStringInfo(&buffer, _("schema %s"), nspname);
				break;
			}

		case OCLASS_STATISTIC_EXT:
			{
				HeapTuple	stxTup;
				Form_pg_statistic_ext stxForm;
				char	   *nspname;

				stxTup = SearchSysCache1(STATEXTOID,
										 ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(stxTup))
					elog(ERROR, "could not find tuple for statistics object %u",
						 object->objectId);
				stxForm = (Form_pg_statistic_ext) GETSTRUCT(stxTup);

				/* Qualify the name if not visible in search path */
				if (StatisticsObjIsVisible(object->objectId))
					nspname = NULL;
				else
					nspname = get_namespace_name(stxForm->stxnamespace);

				appendStringInfo(&buffer, _("statistics object %s"),
								 quote_qualified_identifier(nspname,
															NameStr(stxForm->stxname)));

				ReleaseSysCache(stxTup);
				break;
			}

		case OCLASS_TSPARSER:
			{
				HeapTuple	tup;
				Form_pg_ts_parser prsForm;
				char	   *nspname;

				tup = SearchSysCache1(TSPARSEROID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for text search parser %u",
						 object->objectId);
				prsForm = (Form_pg_ts_parser) GETSTRUCT(tup);

				/* Qualify the name if not visible in search path */
				if (TSParserIsVisible(object->objectId))
					nspname = NULL;
				else
					nspname = get_namespace_name(prsForm->prsnamespace);

				appendStringInfo(&buffer, _("text search parser %s"),
								 quote_qualified_identifier(nspname,
															NameStr(prsForm->prsname)));
				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_TSDICT:
			{
				HeapTuple	tup;
				Form_pg_ts_dict dictForm;
				char	   *nspname;

				tup = SearchSysCache1(TSDICTOID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for text search dictionary %u",
						 object->objectId);
				dictForm = (Form_pg_ts_dict) GETSTRUCT(tup);

				/* Qualify the name if not visible in search path */
				if (TSDictionaryIsVisible(object->objectId))
					nspname = NULL;
				else
					nspname = get_namespace_name(dictForm->dictnamespace);

				appendStringInfo(&buffer, _("text search dictionary %s"),
								 quote_qualified_identifier(nspname,
															NameStr(dictForm->dictname)));
				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_TSTEMPLATE:
			{
				HeapTuple	tup;
				Form_pg_ts_template tmplForm;
				char	   *nspname;

				tup = SearchSysCache1(TSTEMPLATEOID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for text search template %u",
						 object->objectId);
				tmplForm = (Form_pg_ts_template) GETSTRUCT(tup);

				/* Qualify the name if not visible in search path */
				if (TSTemplateIsVisible(object->objectId))
					nspname = NULL;
				else
					nspname = get_namespace_name(tmplForm->tmplnamespace);

				appendStringInfo(&buffer, _("text search template %s"),
								 quote_qualified_identifier(nspname,
															NameStr(tmplForm->tmplname)));
				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_TSCONFIG:
			{
				HeapTuple	tup;
				Form_pg_ts_config cfgForm;
				char	   *nspname;

				tup = SearchSysCache1(TSCONFIGOID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for text search configuration %u",
						 object->objectId);
				cfgForm = (Form_pg_ts_config) GETSTRUCT(tup);

				/* Qualify the name if not visible in search path */
				if (TSConfigIsVisible(object->objectId))
					nspname = NULL;
				else
					nspname = get_namespace_name(cfgForm->cfgnamespace);

				appendStringInfo(&buffer, _("text search configuration %s"),
								 quote_qualified_identifier(nspname,
															NameStr(cfgForm->cfgname)));
				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_ROLE:
			{
				appendStringInfo(&buffer, _("role %s"),
								 GetUserNameFromId(object->objectId, false));
				break;
			}

		case OCLASS_DATABASE:
			{
				char	   *datname;

				datname = get_database_name(object->objectId);
				if (!datname)
					elog(ERROR, "cache lookup failed for database %u",
						 object->objectId);
				appendStringInfo(&buffer, _("database %s"), datname);
				break;
			}

		case OCLASS_TBLSPACE:
			{
				char	   *tblspace;

				tblspace = get_tablespace_name(object->objectId);
				if (!tblspace)
					elog(ERROR, "cache lookup failed for tablespace %u",
						 object->objectId);
				appendStringInfo(&buffer, _("tablespace %s"), tblspace);
				break;
			}

		case OCLASS_FDW:
			{
				ForeignDataWrapper *fdw;

				fdw = GetForeignDataWrapper(object->objectId);
				appendStringInfo(&buffer, _("foreign-data wrapper %s"), fdw->fdwname);
				break;
			}

		case OCLASS_FOREIGN_SERVER:
			{
				ForeignServer *srv;

				srv = GetForeignServer(object->objectId);
				appendStringInfo(&buffer, _("server %s"), srv->servername);
				break;
			}

		case OCLASS_USER_MAPPING:
			{
				HeapTuple	tup;
				Oid			useid;
				char	   *usename;
				Form_pg_user_mapping umform;
				ForeignServer *srv;

				tup = SearchSysCache1(USERMAPPINGOID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for user mapping %u",
						 object->objectId);
				umform = (Form_pg_user_mapping) GETSTRUCT(tup);
				useid = umform->umuser;
				srv = GetForeignServer(umform->umserver);

				ReleaseSysCache(tup);

				if (OidIsValid(useid))
					usename = GetUserNameFromId(useid, false);
				else
					usename = "public";

				appendStringInfo(&buffer, _("user mapping for %s on server %s"), usename,
								 srv->servername);
				break;
			}

		case OCLASS_DEFACL:
			{
				Relation	defaclrel;
				ScanKeyData skey[1];
				SysScanDesc rcscan;
				HeapTuple	tup;
				Form_pg_default_acl defacl;
				char	   *rolename;
				char	   *nspname;

				defaclrel = table_open(DefaultAclRelationId, AccessShareLock);

				ScanKeyInit(&skey[0],
							Anum_pg_default_acl_oid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(object->objectId));

				rcscan = systable_beginscan(defaclrel, DefaultAclOidIndexId,
											true, NULL, 1, skey);

				tup = systable_getnext(rcscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for default ACL %u",
						 object->objectId);

				defacl = (Form_pg_default_acl) GETSTRUCT(tup);

				rolename = GetUserNameFromId(defacl->defaclrole, false);

				if (OidIsValid(defacl->defaclnamespace))
					nspname = get_namespace_name(defacl->defaclnamespace);
				else
					nspname = NULL;

				switch (defacl->defaclobjtype)
				{
					case DEFACLOBJ_RELATION:
						if (nspname)
							appendStringInfo(&buffer,
											 _("default privileges on new relations belonging to role %s in schema %s"),
											 rolename, nspname);
						else
							appendStringInfo(&buffer,
											 _("default privileges on new relations belonging to role %s"),
											 rolename);
						break;
					case DEFACLOBJ_SEQUENCE:
						if (nspname)
							appendStringInfo(&buffer,
											 _("default privileges on new sequences belonging to role %s in schema %s"),
											 rolename, nspname);
						else
							appendStringInfo(&buffer,
											 _("default privileges on new sequences belonging to role %s"),
											 rolename);
						break;
					case DEFACLOBJ_FUNCTION:
						if (nspname)
							appendStringInfo(&buffer,
											 _("default privileges on new functions belonging to role %s in schema %s"),
											 rolename, nspname);
						else
							appendStringInfo(&buffer,
											 _("default privileges on new functions belonging to role %s"),
											 rolename);
						break;
					case DEFACLOBJ_TYPE:
						if (nspname)
							appendStringInfo(&buffer,
											 _("default privileges on new types belonging to role %s in schema %s"),
											 rolename, nspname);
						else
							appendStringInfo(&buffer,
											 _("default privileges on new types belonging to role %s"),
											 rolename);
						break;
					case DEFACLOBJ_NAMESPACE:
						Assert(!nspname);
						appendStringInfo(&buffer,
										 _("default privileges on new schemas belonging to role %s"),
										 rolename);
						break;
					default:
						/* shouldn't get here */
						if (nspname)
							appendStringInfo(&buffer,
											 _("default privileges belonging to role %s in schema %s"),
											 rolename, nspname);
						else
							appendStringInfo(&buffer,
											 _("default privileges belonging to role %s"),
											 rolename);
						break;
				}

				systable_endscan(rcscan);
				table_close(defaclrel, AccessShareLock);
				break;
			}

		case OCLASS_EXTENSION:
			{
				char	   *extname;

				extname = get_extension_name(object->objectId);
				if (!extname)
					elog(ERROR, "cache lookup failed for extension %u",
						 object->objectId);
				appendStringInfo(&buffer, _("extension %s"), extname);
				break;
			}

		case OCLASS_EVENT_TRIGGER:
			{
				HeapTuple	tup;

				tup = SearchSysCache1(EVENTTRIGGEROID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for event trigger %u",
						 object->objectId);
				appendStringInfo(&buffer, _("event trigger %s"),
								 NameStr(((Form_pg_event_trigger) GETSTRUCT(tup))->evtname));
				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_POLICY:
			{
				Relation	policy_rel;
				ScanKeyData skey[1];
				SysScanDesc sscan;
				HeapTuple	tuple;
				Form_pg_policy form_policy;
				StringInfoData rel;

				policy_rel = table_open(PolicyRelationId, AccessShareLock);

				ScanKeyInit(&skey[0],
							Anum_pg_policy_oid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(object->objectId));

				sscan = systable_beginscan(policy_rel, PolicyOidIndexId,
										   true, NULL, 1, skey);

				tuple = systable_getnext(sscan);

				if (!HeapTupleIsValid(tuple))
					elog(ERROR, "could not find tuple for policy %u",
						 object->objectId);
				form_policy = (Form_pg_policy) GETSTRUCT(tuple);

				initStringInfo(&rel);
				getRelationDescription(&rel, form_policy->polrelid);

				/* translator: second %s is, e.g., "table %s" */
				appendStringInfo(&buffer, _("policy %s on %s"),
								 NameStr(form_policy->polname), rel.data);
				pfree(rel.data);
				systable_endscan(sscan);
				table_close(policy_rel, AccessShareLock);
				break;
			}

		case OCLASS_PUBLICATION:
			{
				appendStringInfo(&buffer, _("publication %s"),
								 get_publication_name(object->objectId,
													  false));
				break;
			}

		case OCLASS_PUBLICATION_REL:
			{
				HeapTuple	tup;
				char	   *pubname;
				Form_pg_publication_rel prform;
				StringInfoData rel;

				tup = SearchSysCache1(PUBLICATIONREL,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for publication table %u",
						 object->objectId);

				prform = (Form_pg_publication_rel) GETSTRUCT(tup);
				pubname = get_publication_name(prform->prpubid, false);

				initStringInfo(&rel);
				getRelationDescription(&rel, prform->prrelid);

				/* translator: first %s is, e.g., "table %s" */
				appendStringInfo(&buffer, _("publication of %s in publication %s"),
								 rel.data, pubname);
				pfree(rel.data);
				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_SUBSCRIPTION:
			{
				appendStringInfo(&buffer, _("subscription %s"),
								 get_subscription_name(object->objectId,
													   false));
				break;
			}

		case OCLASS_TRANSFORM:
			{
				HeapTuple	trfTup;
				Form_pg_transform trfForm;

				trfTup = SearchSysCache1(TRFOID,
										 ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(trfTup))
					elog(ERROR, "could not find tuple for transform %u",
						 object->objectId);

				trfForm = (Form_pg_transform) GETSTRUCT(trfTup);

				appendStringInfo(&buffer, _("transform for %s language %s"),
								 format_type_be(trfForm->trftype),
								 get_language_name(trfForm->trflang, false));

				ReleaseSysCache(trfTup);
				break;
			}

			/*
			 * There's intentionally no default: case here; we want the
			 * compiler to warn if a new OCLASS hasn't been handled above.
			 */
	}

	return buffer.data;
}

/*
 * getObjectDescriptionOids: as above, except the object is specified by Oids
 */
char *
getObjectDescriptionOids(Oid classid, Oid objid)
{
	ObjectAddress address;

	address.classId = classid;
	address.objectId = objid;
	address.objectSubId = 0;

	return getObjectDescription(&address);
}

/*
 * subroutine for getObjectDescription: describe a relation
 *
 * The result is appended to "buffer".
 */
static void
getRelationDescription(StringInfo buffer, Oid relid)
{
	HeapTuple	relTup;
	Form_pg_class relForm;
	char	   *nspname;
	char	   *relname;

	relTup = SearchSysCache1(RELOID,
							 ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(relTup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	relForm = (Form_pg_class) GETSTRUCT(relTup);

	/* Qualify the name if not visible in search path */
	if (RelationIsVisible(relid))
		nspname = NULL;
	else
		nspname = get_namespace_name(relForm->relnamespace);

	relname = quote_qualified_identifier(nspname, NameStr(relForm->relname));

	switch (relForm->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_PARTITIONED_TABLE:
			appendStringInfo(buffer, _("table %s"),
							 relname);
			break;
		case RELKIND_INDEX:
		case RELKIND_PARTITIONED_INDEX:
			appendStringInfo(buffer, _("index %s"),
							 relname);
			break;
		case RELKIND_SEQUENCE:
			appendStringInfo(buffer, _("sequence %s"),
							 relname);
			break;
		case RELKIND_TOASTVALUE:
			appendStringInfo(buffer, _("toast table %s"),
							 relname);
			break;
		case RELKIND_VIEW:
			appendStringInfo(buffer, _("view %s"),
							 relname);
			break;
		case RELKIND_MATVIEW:
			appendStringInfo(buffer, _("materialized view %s"),
							 relname);
			break;
		case RELKIND_COMPOSITE_TYPE:
			appendStringInfo(buffer, _("composite type %s"),
							 relname);
			break;
		case RELKIND_FOREIGN_TABLE:
			appendStringInfo(buffer, _("foreign table %s"),
							 relname);
			break;
		default:
			/* shouldn't get here */
			appendStringInfo(buffer, _("relation %s"),
							 relname);
			break;
	}

	ReleaseSysCache(relTup);
}

/*
 * subroutine for getObjectDescription: describe an operator family
 */
static void
getOpFamilyDescription(StringInfo buffer, Oid opfid)
{
	HeapTuple	opfTup;
	Form_pg_opfamily opfForm;
	HeapTuple	amTup;
	Form_pg_am	amForm;
	char	   *nspname;

	opfTup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfid));
	if (!HeapTupleIsValid(opfTup))
		elog(ERROR, "cache lookup failed for opfamily %u", opfid);
	opfForm = (Form_pg_opfamily) GETSTRUCT(opfTup);

	amTup = SearchSysCache1(AMOID, ObjectIdGetDatum(opfForm->opfmethod));
	if (!HeapTupleIsValid(amTup))
		elog(ERROR, "cache lookup failed for access method %u",
			 opfForm->opfmethod);
	amForm = (Form_pg_am) GETSTRUCT(amTup);

	/* Qualify the name if not visible in search path */
	if (OpfamilyIsVisible(opfid))
		nspname = NULL;
	else
		nspname = get_namespace_name(opfForm->opfnamespace);

	appendStringInfo(buffer, _("operator family %s for access method %s"),
					 quote_qualified_identifier(nspname,
												NameStr(opfForm->opfname)),
					 NameStr(amForm->amname));

	ReleaseSysCache(amTup);
	ReleaseSysCache(opfTup);
}

/*
 * SQL-level callable version of getObjectDescription
 */
Datum
pg_describe_object(PG_FUNCTION_ARGS)
{
	Oid			classid = PG_GETARG_OID(0);
	Oid			objid = PG_GETARG_OID(1);
	int32		objsubid = PG_GETARG_INT32(2);
	char	   *description;
	ObjectAddress address;

	/* for "pinned" items in pg_depend, return null */
	if (!OidIsValid(classid) && !OidIsValid(objid))
		PG_RETURN_NULL();

	address.classId = classid;
	address.objectId = objid;
	address.objectSubId = objsubid;

	description = getObjectDescription(&address);
	PG_RETURN_TEXT_P(cstring_to_text(description));
}

/*
 * SQL-level callable function to obtain object type + identity
 */
Datum
pg_identify_object(PG_FUNCTION_ARGS)
{
	Oid			classid = PG_GETARG_OID(0);
	Oid			objid = PG_GETARG_OID(1);
	int32		objsubid = PG_GETARG_INT32(2);
	Oid			schema_oid = InvalidOid;
	const char *objname = NULL;
	ObjectAddress address;
	Datum		values[4];
	bool		nulls[4];
	TupleDesc	tupdesc;
	HeapTuple	htup;

	address.classId = classid;
	address.objectId = objid;
	address.objectSubId = objsubid;

	/*
	 * Construct a tuple descriptor for the result row.  This must match this
	 * function's pg_proc entry!
	 */
	tupdesc = CreateTemplateTupleDesc(4);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "type",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "schema",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "identity",
					   TEXTOID, -1, 0);

	tupdesc = BlessTupleDesc(tupdesc);

	if (is_objectclass_supported(address.classId))
	{
		HeapTuple	objtup;
		Relation	catalog = table_open(address.classId, AccessShareLock);

		objtup = get_catalog_object_by_oid(catalog,
										   get_object_attnum_oid(address.classId),
										   address.objectId);
		if (objtup != NULL)
		{
			bool		isnull;
			AttrNumber	nspAttnum;
			AttrNumber	nameAttnum;

			nspAttnum = get_object_attnum_namespace(address.classId);
			if (nspAttnum != InvalidAttrNumber)
			{
				schema_oid = heap_getattr(objtup, nspAttnum,
										  RelationGetDescr(catalog), &isnull);
				if (isnull)
					elog(ERROR, "invalid null namespace in object %u/%u/%d",
						 address.classId, address.objectId, address.objectSubId);
			}

			/*
			 * We only return the object name if it can be used (together with
			 * the schema name, if any) as a unique identifier.
			 */
			if (get_object_namensp_unique(address.classId))
			{
				nameAttnum = get_object_attnum_name(address.classId);
				if (nameAttnum != InvalidAttrNumber)
				{
					Datum		nameDatum;

					nameDatum = heap_getattr(objtup, nameAttnum,
											 RelationGetDescr(catalog), &isnull);
					if (isnull)
						elog(ERROR, "invalid null name in object %u/%u/%d",
							 address.classId, address.objectId, address.objectSubId);
					objname = quote_identifier(NameStr(*(DatumGetName(nameDatum))));
				}
			}
		}

		table_close(catalog, AccessShareLock);
	}

	/* object type */
	values[0] = CStringGetTextDatum(getObjectTypeDescription(&address));
	nulls[0] = false;

	/* schema name */
	if (OidIsValid(schema_oid))
	{
		const char *schema = quote_identifier(get_namespace_name(schema_oid));

		values[1] = CStringGetTextDatum(schema);
		nulls[1] = false;
	}
	else
		nulls[1] = true;

	/* object name */
	if (objname)
	{
		values[2] = CStringGetTextDatum(objname);
		nulls[2] = false;
	}
	else
		nulls[2] = true;

	/* object identity */
	values[3] = CStringGetTextDatum(getObjectIdentity(&address));
	nulls[3] = false;

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

/*
 * SQL-level callable function to obtain object type + identity
 */
Datum
pg_identify_object_as_address(PG_FUNCTION_ARGS)
{
	Oid			classid = PG_GETARG_OID(0);
	Oid			objid = PG_GETARG_OID(1);
	int32		objsubid = PG_GETARG_INT32(2);
	ObjectAddress address;
	char	   *identity;
	List	   *names;
	List	   *args;
	Datum		values[3];
	bool		nulls[3];
	TupleDesc	tupdesc;
	HeapTuple	htup;

	address.classId = classid;
	address.objectId = objid;
	address.objectSubId = objsubid;

	/*
	 * Construct a tuple descriptor for the result row.  This must match this
	 * function's pg_proc entry!
	 */
	tupdesc = CreateTemplateTupleDesc(3);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "type",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "object_names",
					   TEXTARRAYOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "object_args",
					   TEXTARRAYOID, -1, 0);

	tupdesc = BlessTupleDesc(tupdesc);

	/* object type */
	values[0] = CStringGetTextDatum(getObjectTypeDescription(&address));
	nulls[0] = false;

	/* object identity */
	identity = getObjectIdentityParts(&address, &names, &args);
	pfree(identity);

	/* object_names */
	if (names != NIL)
		values[1] = PointerGetDatum(strlist_to_textarray(names));
	else
		values[1] = PointerGetDatum(construct_empty_array(TEXTOID));
	nulls[1] = false;

	/* object_args */
	if (args)
		values[2] = PointerGetDatum(strlist_to_textarray(args));
	else
		values[2] = PointerGetDatum(construct_empty_array(TEXTOID));
	nulls[2] = false;

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

/*
 * Return a palloc'ed string that describes the type of object that the
 * passed address is for.
 *
 * Keep ObjectTypeMap in sync with this.
 */
char *
getObjectTypeDescription(const ObjectAddress *object)
{
	StringInfoData buffer;

	initStringInfo(&buffer);

	switch (getObjectClass(object))
	{
		case OCLASS_CLASS:
			getRelationTypeDescription(&buffer, object->objectId,
									   object->objectSubId);
			break;

		case OCLASS_PROC:
			getProcedureTypeDescription(&buffer, object->objectId);
			break;

		case OCLASS_TYPE:
			appendStringInfoString(&buffer, "type");
			break;

		case OCLASS_CAST:
			appendStringInfoString(&buffer, "cast");
			break;

		case OCLASS_COLLATION:
			appendStringInfoString(&buffer, "collation");
			break;

		case OCLASS_CONSTRAINT:
			getConstraintTypeDescription(&buffer, object->objectId);
			break;

		case OCLASS_CONVERSION:
			appendStringInfoString(&buffer, "conversion");
			break;

		case OCLASS_DEFAULT:
			appendStringInfoString(&buffer, "default value");
			break;

		case OCLASS_LANGUAGE:
			appendStringInfoString(&buffer, "language");
			break;

		case OCLASS_LARGEOBJECT:
			appendStringInfoString(&buffer, "large object");
			break;

		case OCLASS_OPERATOR:
			appendStringInfoString(&buffer, "operator");
			break;

		case OCLASS_OPCLASS:
			appendStringInfoString(&buffer, "operator class");
			break;

		case OCLASS_OPFAMILY:
			appendStringInfoString(&buffer, "operator family");
			break;

		case OCLASS_AM:
			appendStringInfoString(&buffer, "access method");
			break;

		case OCLASS_AMOP:
			appendStringInfoString(&buffer, "operator of access method");
			break;

		case OCLASS_AMPROC:
			appendStringInfoString(&buffer, "function of access method");
			break;

		case OCLASS_REWRITE:
			appendStringInfoString(&buffer, "rule");
			break;

		case OCLASS_TRIGGER:
			appendStringInfoString(&buffer, "trigger");
			break;

		case OCLASS_SCHEMA:
			appendStringInfoString(&buffer, "schema");
			break;

		case OCLASS_STATISTIC_EXT:
			appendStringInfoString(&buffer, "statistics object");
			break;

		case OCLASS_TSPARSER:
			appendStringInfoString(&buffer, "text search parser");
			break;

		case OCLASS_TSDICT:
			appendStringInfoString(&buffer, "text search dictionary");
			break;

		case OCLASS_TSTEMPLATE:
			appendStringInfoString(&buffer, "text search template");
			break;

		case OCLASS_TSCONFIG:
			appendStringInfoString(&buffer, "text search configuration");
			break;

		case OCLASS_ROLE:
			appendStringInfoString(&buffer, "role");
			break;

		case OCLASS_DATABASE:
			appendStringInfoString(&buffer, "database");
			break;

		case OCLASS_TBLSPACE:
			appendStringInfoString(&buffer, "tablespace");
			break;

		case OCLASS_FDW:
			appendStringInfoString(&buffer, "foreign-data wrapper");
			break;

		case OCLASS_FOREIGN_SERVER:
			appendStringInfoString(&buffer, "server");
			break;

		case OCLASS_USER_MAPPING:
			appendStringInfoString(&buffer, "user mapping");
			break;

		case OCLASS_DEFACL:
			appendStringInfoString(&buffer, "default acl");
			break;

		case OCLASS_EXTENSION:
			appendStringInfoString(&buffer, "extension");
			break;

		case OCLASS_EVENT_TRIGGER:
			appendStringInfoString(&buffer, "event trigger");
			break;

		case OCLASS_POLICY:
			appendStringInfoString(&buffer, "policy");
			break;

		case OCLASS_PUBLICATION:
			appendStringInfoString(&buffer, "publication");
			break;

		case OCLASS_PUBLICATION_REL:
			appendStringInfoString(&buffer, "publication relation");
			break;

		case OCLASS_SUBSCRIPTION:
			appendStringInfoString(&buffer, "subscription");
			break;

		case OCLASS_TRANSFORM:
			appendStringInfoString(&buffer, "transform");
			break;

			/*
			 * There's intentionally no default: case here; we want the
			 * compiler to warn if a new OCLASS hasn't been handled above.
			 */
	}

	return buffer.data;
}

/*
 * subroutine for getObjectTypeDescription: describe a relation type
 */
static void
getRelationTypeDescription(StringInfo buffer, Oid relid, int32 objectSubId)
{
	HeapTuple	relTup;
	Form_pg_class relForm;

	relTup = SearchSysCache1(RELOID,
							 ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(relTup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	relForm = (Form_pg_class) GETSTRUCT(relTup);

	switch (relForm->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_PARTITIONED_TABLE:
			appendStringInfoString(buffer, "table");
			break;
		case RELKIND_INDEX:
		case RELKIND_PARTITIONED_INDEX:
			appendStringInfoString(buffer, "index");
			break;
		case RELKIND_SEQUENCE:
			appendStringInfoString(buffer, "sequence");
			break;
		case RELKIND_TOASTVALUE:
			appendStringInfoString(buffer, "toast table");
			break;
		case RELKIND_VIEW:
			appendStringInfoString(buffer, "view");
			break;
		case RELKIND_MATVIEW:
			appendStringInfoString(buffer, "materialized view");
			break;
		case RELKIND_COMPOSITE_TYPE:
			appendStringInfoString(buffer, "composite type");
			break;
		case RELKIND_FOREIGN_TABLE:
			appendStringInfoString(buffer, "foreign table");
			break;
		default:
			/* shouldn't get here */
			appendStringInfoString(buffer, "relation");
			break;
	}

	if (objectSubId != 0)
		appendStringInfoString(buffer, " column");

	ReleaseSysCache(relTup);
}

/*
 * subroutine for getObjectTypeDescription: describe a constraint type
 */
static void
getConstraintTypeDescription(StringInfo buffer, Oid constroid)
{
	Relation	constrRel;
	HeapTuple	constrTup;
	Form_pg_constraint constrForm;

	constrRel = table_open(ConstraintRelationId, AccessShareLock);
	constrTup = get_catalog_object_by_oid(constrRel, Anum_pg_constraint_oid,
										  constroid);
	if (!HeapTupleIsValid(constrTup))
		elog(ERROR, "cache lookup failed for constraint %u", constroid);

	constrForm = (Form_pg_constraint) GETSTRUCT(constrTup);

	if (OidIsValid(constrForm->conrelid))
		appendStringInfoString(buffer, "table constraint");
	else if (OidIsValid(constrForm->contypid))
		appendStringInfoString(buffer, "domain constraint");
	else
		elog(ERROR, "invalid constraint %u", constrForm->oid);

	table_close(constrRel, AccessShareLock);
}

/*
 * subroutine for getObjectTypeDescription: describe a procedure type
 */
static void
getProcedureTypeDescription(StringInfo buffer, Oid procid)
{
	HeapTuple	procTup;
	Form_pg_proc procForm;

	procTup = SearchSysCache1(PROCOID,
							  ObjectIdGetDatum(procid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for procedure %u", procid);
	procForm = (Form_pg_proc) GETSTRUCT(procTup);

	if (procForm->prokind == PROKIND_AGGREGATE)
		appendStringInfoString(buffer, "aggregate");
	else if (procForm->prokind == PROKIND_PROCEDURE)
		appendStringInfoString(buffer, "procedure");
	else						/* function or window function */
		appendStringInfoString(buffer, "function");

	ReleaseSysCache(procTup);
}

/*
 * Obtain a given object's identity, as a palloc'ed string.
 *
 * This is for machine consumption, so it's not translated.  All elements are
 * schema-qualified when appropriate.
 */
char *
getObjectIdentity(const ObjectAddress *object)
{
	return getObjectIdentityParts(object, NULL, NULL);
}

/*
 * As above, but more detailed.
 *
 * There are two sets of return values: the identity itself as a palloc'd
 * string is returned.  objname and objargs, if not NULL, are output parameters
 * that receive lists of C-strings that are useful to give back to
 * get_object_address() to reconstruct the ObjectAddress.
 */
char *
getObjectIdentityParts(const ObjectAddress *object,
					   List **objname, List **objargs)
{
	StringInfoData buffer;

	initStringInfo(&buffer);

	/*
	 * Make sure that both objname and objargs were passed, or none was; and
	 * initialize them to empty lists.  For objname this is useless because it
	 * will be initialized in all cases inside the switch; but we do it anyway
	 * so that we can test below that no branch leaves it unset.
	 */
	Assert(PointerIsValid(objname) == PointerIsValid(objargs));
	if (objname)
	{
		*objname = NIL;
		*objargs = NIL;
	}

	switch (getObjectClass(object))
	{
		case OCLASS_CLASS:
			getRelationIdentity(&buffer, object->objectId, objname);
			if (object->objectSubId != 0)
			{
				char	   *attr;

				attr = get_attname(object->objectId, object->objectSubId,
								   false);
				appendStringInfo(&buffer, ".%s", quote_identifier(attr));
				if (objname)
					*objname = lappend(*objname, attr);
			}
			break;

		case OCLASS_PROC:
			appendStringInfoString(&buffer,
								   format_procedure_qualified(object->objectId));
			if (objname)
				format_procedure_parts(object->objectId, objname, objargs);
			break;

		case OCLASS_TYPE:
			{
				char	   *typeout;

				typeout = format_type_be_qualified(object->objectId);
				appendStringInfoString(&buffer, typeout);
				if (objname)
					*objname = list_make1(typeout);
			}
			break;

		case OCLASS_CAST:
			{
				Relation	castRel;
				HeapTuple	tup;
				Form_pg_cast castForm;

				castRel = table_open(CastRelationId, AccessShareLock);

				tup = get_catalog_object_by_oid(castRel, Anum_pg_cast_oid,
												object->objectId);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for cast %u",
						 object->objectId);

				castForm = (Form_pg_cast) GETSTRUCT(tup);

				appendStringInfo(&buffer, "(%s AS %s)",
								 format_type_be_qualified(castForm->castsource),
								 format_type_be_qualified(castForm->casttarget));

				if (objname)
				{
					*objname = list_make1(format_type_be_qualified(castForm->castsource));
					*objargs = list_make1(format_type_be_qualified(castForm->casttarget));
				}

				table_close(castRel, AccessShareLock);
				break;
			}

		case OCLASS_COLLATION:
			{
				HeapTuple	collTup;
				Form_pg_collation coll;
				char	   *schema;

				collTup = SearchSysCache1(COLLOID,
										  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(collTup))
					elog(ERROR, "cache lookup failed for collation %u",
						 object->objectId);
				coll = (Form_pg_collation) GETSTRUCT(collTup);
				schema = get_namespace_name_or_temp(coll->collnamespace);
				appendStringInfoString(&buffer,
									   quote_qualified_identifier(schema,
																  NameStr(coll->collname)));
				if (objname)
					*objname = list_make2(schema,
										  pstrdup(NameStr(coll->collname)));
				ReleaseSysCache(collTup);
				break;
			}

		case OCLASS_CONSTRAINT:
			{
				HeapTuple	conTup;
				Form_pg_constraint con;

				conTup = SearchSysCache1(CONSTROID,
										 ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(conTup))
					elog(ERROR, "cache lookup failed for constraint %u",
						 object->objectId);
				con = (Form_pg_constraint) GETSTRUCT(conTup);

				if (OidIsValid(con->conrelid))
				{
					appendStringInfo(&buffer, "%s on ",
									 quote_identifier(NameStr(con->conname)));
					getRelationIdentity(&buffer, con->conrelid, objname);
					if (objname)
						*objname = lappend(*objname, pstrdup(NameStr(con->conname)));
				}
				else
				{
					ObjectAddress domain;

					Assert(OidIsValid(con->contypid));
					domain.classId = TypeRelationId;
					domain.objectId = con->contypid;
					domain.objectSubId = 0;

					appendStringInfo(&buffer, "%s on %s",
									 quote_identifier(NameStr(con->conname)),
									 getObjectIdentityParts(&domain, objname, objargs));

					if (objname)
						*objargs = lappend(*objargs, pstrdup(NameStr(con->conname)));
				}

				ReleaseSysCache(conTup);
				break;
			}

		case OCLASS_CONVERSION:
			{
				HeapTuple	conTup;
				Form_pg_conversion conForm;
				char	   *schema;

				conTup = SearchSysCache1(CONVOID,
										 ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(conTup))
					elog(ERROR, "cache lookup failed for conversion %u",
						 object->objectId);
				conForm = (Form_pg_conversion) GETSTRUCT(conTup);
				schema = get_namespace_name_or_temp(conForm->connamespace);
				appendStringInfoString(&buffer,
									   quote_qualified_identifier(schema,
																  NameStr(conForm->conname)));
				if (objname)
					*objname = list_make2(schema,
										  pstrdup(NameStr(conForm->conname)));
				ReleaseSysCache(conTup);
				break;
			}

		case OCLASS_DEFAULT:
			{
				Relation	attrdefDesc;
				ScanKeyData skey[1];
				SysScanDesc adscan;

				HeapTuple	tup;
				Form_pg_attrdef attrdef;
				ObjectAddress colobject;

				attrdefDesc = table_open(AttrDefaultRelationId, AccessShareLock);

				ScanKeyInit(&skey[0],
							Anum_pg_attrdef_oid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(object->objectId));

				adscan = systable_beginscan(attrdefDesc, AttrDefaultOidIndexId,
											true, NULL, 1, skey);

				tup = systable_getnext(adscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for attrdef %u",
						 object->objectId);

				attrdef = (Form_pg_attrdef) GETSTRUCT(tup);

				colobject.classId = RelationRelationId;
				colobject.objectId = attrdef->adrelid;
				colobject.objectSubId = attrdef->adnum;

				appendStringInfo(&buffer, "for %s",
								 getObjectIdentityParts(&colobject,
														objname, objargs));

				systable_endscan(adscan);
				table_close(attrdefDesc, AccessShareLock);
				break;
			}

		case OCLASS_LANGUAGE:
			{
				HeapTuple	langTup;
				Form_pg_language langForm;

				langTup = SearchSysCache1(LANGOID,
										  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(langTup))
					elog(ERROR, "cache lookup failed for language %u",
						 object->objectId);
				langForm = (Form_pg_language) GETSTRUCT(langTup);
				appendStringInfoString(&buffer,
									   quote_identifier(NameStr(langForm->lanname)));
				if (objname)
					*objname = list_make1(pstrdup(NameStr(langForm->lanname)));
				ReleaseSysCache(langTup);
				break;
			}
		case OCLASS_LARGEOBJECT:
			appendStringInfo(&buffer, "%u",
							 object->objectId);
			if (objname)
				*objname = list_make1(psprintf("%u", object->objectId));
			break;

		case OCLASS_OPERATOR:
			appendStringInfoString(&buffer,
								   format_operator_qualified(object->objectId));
			if (objname)
				format_operator_parts(object->objectId, objname, objargs);
			break;

		case OCLASS_OPCLASS:
			{
				HeapTuple	opcTup;
				Form_pg_opclass opcForm;
				HeapTuple	amTup;
				Form_pg_am	amForm;
				char	   *schema;

				opcTup = SearchSysCache1(CLAOID,
										 ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(opcTup))
					elog(ERROR, "cache lookup failed for opclass %u",
						 object->objectId);
				opcForm = (Form_pg_opclass) GETSTRUCT(opcTup);
				schema = get_namespace_name_or_temp(opcForm->opcnamespace);

				amTup = SearchSysCache1(AMOID,
										ObjectIdGetDatum(opcForm->opcmethod));
				if (!HeapTupleIsValid(amTup))
					elog(ERROR, "cache lookup failed for access method %u",
						 opcForm->opcmethod);
				amForm = (Form_pg_am) GETSTRUCT(amTup);

				appendStringInfo(&buffer, "%s USING %s",
								 quote_qualified_identifier(schema,
															NameStr(opcForm->opcname)),
								 quote_identifier(NameStr(amForm->amname)));
				if (objname)
					*objname = list_make3(pstrdup(NameStr(amForm->amname)),
										  schema,
										  pstrdup(NameStr(opcForm->opcname)));

				ReleaseSysCache(amTup);
				ReleaseSysCache(opcTup);
				break;
			}

		case OCLASS_OPFAMILY:
			getOpFamilyIdentity(&buffer, object->objectId, objname);
			break;

		case OCLASS_AM:
			{
				char	   *amname;

				amname = get_am_name(object->objectId);
				if (!amname)
					elog(ERROR, "cache lookup failed for access method %u",
						 object->objectId);
				appendStringInfoString(&buffer, quote_identifier(amname));
				if (objname)
					*objname = list_make1(amname);
			}
			break;

		case OCLASS_AMOP:
			{
				Relation	amopDesc;
				HeapTuple	tup;
				ScanKeyData skey[1];
				SysScanDesc amscan;
				Form_pg_amop amopForm;
				StringInfoData opfam;
				char	   *ltype;
				char	   *rtype;

				amopDesc = table_open(AccessMethodOperatorRelationId,
									  AccessShareLock);

				ScanKeyInit(&skey[0],
							Anum_pg_amop_oid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(object->objectId));

				amscan = systable_beginscan(amopDesc, AccessMethodOperatorOidIndexId, true,
											NULL, 1, skey);

				tup = systable_getnext(amscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for amop entry %u",
						 object->objectId);

				amopForm = (Form_pg_amop) GETSTRUCT(tup);

				initStringInfo(&opfam);
				getOpFamilyIdentity(&opfam, amopForm->amopfamily, objname);

				ltype = format_type_be_qualified(amopForm->amoplefttype);
				rtype = format_type_be_qualified(amopForm->amoprighttype);

				if (objname)
				{
					*objname = lappend(*objname,
									   psprintf("%d", amopForm->amopstrategy));
					*objargs = list_make2(ltype, rtype);
				}

				appendStringInfo(&buffer, "operator %d (%s, %s) of %s",
								 amopForm->amopstrategy,
								 ltype, rtype, opfam.data);

				pfree(opfam.data);

				systable_endscan(amscan);
				table_close(amopDesc, AccessShareLock);
				break;
			}

		case OCLASS_AMPROC:
			{
				Relation	amprocDesc;
				ScanKeyData skey[1];
				SysScanDesc amscan;
				HeapTuple	tup;
				Form_pg_amproc amprocForm;
				StringInfoData opfam;
				char	   *ltype;
				char	   *rtype;

				amprocDesc = table_open(AccessMethodProcedureRelationId,
										AccessShareLock);

				ScanKeyInit(&skey[0],
							Anum_pg_amproc_oid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(object->objectId));

				amscan = systable_beginscan(amprocDesc, AccessMethodProcedureOidIndexId, true,
											NULL, 1, skey);

				tup = systable_getnext(amscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for amproc entry %u",
						 object->objectId);

				amprocForm = (Form_pg_amproc) GETSTRUCT(tup);

				initStringInfo(&opfam);
				getOpFamilyIdentity(&opfam, amprocForm->amprocfamily, objname);

				ltype = format_type_be_qualified(amprocForm->amproclefttype);
				rtype = format_type_be_qualified(amprocForm->amprocrighttype);

				if (objname)
				{
					*objname = lappend(*objname,
									   psprintf("%d", amprocForm->amprocnum));
					*objargs = list_make2(ltype, rtype);
				}

				appendStringInfo(&buffer, "function %d (%s, %s) of %s",
								 amprocForm->amprocnum,
								 ltype, rtype, opfam.data);

				pfree(opfam.data);

				systable_endscan(amscan);
				table_close(amprocDesc, AccessShareLock);
				break;
			}

		case OCLASS_REWRITE:
			{
				Relation	ruleDesc;
				HeapTuple	tup;
				Form_pg_rewrite rule;

				ruleDesc = table_open(RewriteRelationId, AccessShareLock);

				tup = get_catalog_object_by_oid(ruleDesc, Anum_pg_rewrite_oid,
												object->objectId);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for rule %u",
						 object->objectId);

				rule = (Form_pg_rewrite) GETSTRUCT(tup);

				appendStringInfo(&buffer, "%s on ",
								 quote_identifier(NameStr(rule->rulename)));
				getRelationIdentity(&buffer, rule->ev_class, objname);
				if (objname)
					*objname = lappend(*objname, pstrdup(NameStr(rule->rulename)));

				table_close(ruleDesc, AccessShareLock);
				break;
			}

		case OCLASS_TRIGGER:
			{
				Relation	trigDesc;
				HeapTuple	tup;
				Form_pg_trigger trig;

				trigDesc = table_open(TriggerRelationId, AccessShareLock);

				tup = get_catalog_object_by_oid(trigDesc, Anum_pg_trigger_oid,
												object->objectId);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for trigger %u",
						 object->objectId);

				trig = (Form_pg_trigger) GETSTRUCT(tup);

				appendStringInfo(&buffer, "%s on ",
								 quote_identifier(NameStr(trig->tgname)));
				getRelationIdentity(&buffer, trig->tgrelid, objname);
				if (objname)
					*objname = lappend(*objname, pstrdup(NameStr(trig->tgname)));

				table_close(trigDesc, AccessShareLock);
				break;
			}

		case OCLASS_SCHEMA:
			{
				char	   *nspname;

				nspname = get_namespace_name_or_temp(object->objectId);
				if (!nspname)
					elog(ERROR, "cache lookup failed for namespace %u",
						 object->objectId);
				appendStringInfoString(&buffer,
									   quote_identifier(nspname));
				if (objname)
					*objname = list_make1(nspname);
				break;
			}

		case OCLASS_STATISTIC_EXT:
			{
				HeapTuple	tup;
				Form_pg_statistic_ext formStatistic;
				char	   *schema;

				tup = SearchSysCache1(STATEXTOID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for statistics object %u",
						 object->objectId);
				formStatistic = (Form_pg_statistic_ext) GETSTRUCT(tup);
				schema = get_namespace_name_or_temp(formStatistic->stxnamespace);
				appendStringInfoString(&buffer,
									   quote_qualified_identifier(schema,
																  NameStr(formStatistic->stxname)));
				if (objname)
					*objname = list_make2(schema,
										  pstrdup(NameStr(formStatistic->stxname)));
				ReleaseSysCache(tup);
			}
			break;

		case OCLASS_TSPARSER:
			{
				HeapTuple	tup;
				Form_pg_ts_parser formParser;
				char	   *schema;

				tup = SearchSysCache1(TSPARSEROID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for text search parser %u",
						 object->objectId);
				formParser = (Form_pg_ts_parser) GETSTRUCT(tup);
				schema = get_namespace_name_or_temp(formParser->prsnamespace);
				appendStringInfoString(&buffer,
									   quote_qualified_identifier(schema,
																  NameStr(formParser->prsname)));
				if (objname)
					*objname = list_make2(schema,
										  pstrdup(NameStr(formParser->prsname)));
				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_TSDICT:
			{
				HeapTuple	tup;
				Form_pg_ts_dict formDict;
				char	   *schema;

				tup = SearchSysCache1(TSDICTOID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for text search dictionary %u",
						 object->objectId);
				formDict = (Form_pg_ts_dict) GETSTRUCT(tup);
				schema = get_namespace_name_or_temp(formDict->dictnamespace);
				appendStringInfoString(&buffer,
									   quote_qualified_identifier(schema,
																  NameStr(formDict->dictname)));
				if (objname)
					*objname = list_make2(schema,
										  pstrdup(NameStr(formDict->dictname)));
				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_TSTEMPLATE:
			{
				HeapTuple	tup;
				Form_pg_ts_template formTmpl;
				char	   *schema;

				tup = SearchSysCache1(TSTEMPLATEOID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for text search template %u",
						 object->objectId);
				formTmpl = (Form_pg_ts_template) GETSTRUCT(tup);
				schema = get_namespace_name_or_temp(formTmpl->tmplnamespace);
				appendStringInfoString(&buffer,
									   quote_qualified_identifier(schema,
																  NameStr(formTmpl->tmplname)));
				if (objname)
					*objname = list_make2(schema,
										  pstrdup(NameStr(formTmpl->tmplname)));
				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_TSCONFIG:
			{
				HeapTuple	tup;
				Form_pg_ts_config formCfg;
				char	   *schema;

				tup = SearchSysCache1(TSCONFIGOID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for text search configuration %u",
						 object->objectId);
				formCfg = (Form_pg_ts_config) GETSTRUCT(tup);
				schema = get_namespace_name_or_temp(formCfg->cfgnamespace);
				appendStringInfoString(&buffer,
									   quote_qualified_identifier(schema,
																  NameStr(formCfg->cfgname)));
				if (objname)
					*objname = list_make2(schema,
										  pstrdup(NameStr(formCfg->cfgname)));
				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_ROLE:
			{
				char	   *username;

				username = GetUserNameFromId(object->objectId, false);
				if (objname)
					*objname = list_make1(username);
				appendStringInfoString(&buffer,
									   quote_identifier(username));
				break;
			}

		case OCLASS_DATABASE:
			{
				char	   *datname;

				datname = get_database_name(object->objectId);
				if (!datname)
					elog(ERROR, "cache lookup failed for database %u",
						 object->objectId);
				if (objname)
					*objname = list_make1(datname);
				appendStringInfoString(&buffer,
									   quote_identifier(datname));
				break;
			}

		case OCLASS_TBLSPACE:
			{
				char	   *tblspace;

				tblspace = get_tablespace_name(object->objectId);
				if (!tblspace)
					elog(ERROR, "cache lookup failed for tablespace %u",
						 object->objectId);
				if (objname)
					*objname = list_make1(tblspace);
				appendStringInfoString(&buffer,
									   quote_identifier(tblspace));
				break;
			}

		case OCLASS_FDW:
			{
				ForeignDataWrapper *fdw;

				fdw = GetForeignDataWrapper(object->objectId);
				appendStringInfoString(&buffer, quote_identifier(fdw->fdwname));
				if (objname)
					*objname = list_make1(pstrdup(fdw->fdwname));
				break;
			}

		case OCLASS_FOREIGN_SERVER:
			{
				ForeignServer *srv;

				srv = GetForeignServer(object->objectId);
				appendStringInfoString(&buffer,
									   quote_identifier(srv->servername));
				if (objname)
					*objname = list_make1(pstrdup(srv->servername));
				break;
			}

		case OCLASS_USER_MAPPING:
			{
				HeapTuple	tup;
				Oid			useid;
				Form_pg_user_mapping umform;
				ForeignServer *srv;
				const char *usename;

				tup = SearchSysCache1(USERMAPPINGOID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for user mapping %u",
						 object->objectId);
				umform = (Form_pg_user_mapping) GETSTRUCT(tup);
				useid = umform->umuser;
				srv = GetForeignServer(umform->umserver);

				ReleaseSysCache(tup);

				if (OidIsValid(useid))
					usename = GetUserNameFromId(useid, false);
				else
					usename = "public";

				if (objname)
				{
					*objname = list_make1(pstrdup(usename));
					*objargs = list_make1(pstrdup(srv->servername));
				}

				appendStringInfo(&buffer, "%s on server %s",
								 quote_identifier(usename),
								 srv->servername);
				break;
			}

		case OCLASS_DEFACL:
			{
				Relation	defaclrel;
				ScanKeyData skey[1];
				SysScanDesc rcscan;
				HeapTuple	tup;
				Form_pg_default_acl defacl;
				char	   *schema;
				char	   *username;

				defaclrel = table_open(DefaultAclRelationId, AccessShareLock);

				ScanKeyInit(&skey[0],
							Anum_pg_default_acl_oid,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(object->objectId));

				rcscan = systable_beginscan(defaclrel, DefaultAclOidIndexId,
											true, NULL, 1, skey);

				tup = systable_getnext(rcscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for default ACL %u",
						 object->objectId);

				defacl = (Form_pg_default_acl) GETSTRUCT(tup);

				username = GetUserNameFromId(defacl->defaclrole, false);
				appendStringInfo(&buffer,
								 "for role %s",
								 quote_identifier(username));

				if (OidIsValid(defacl->defaclnamespace))
				{
					schema = get_namespace_name_or_temp(defacl->defaclnamespace);
					appendStringInfo(&buffer,
									 " in schema %s",
									 quote_identifier(schema));
				}
				else
					schema = NULL;

				switch (defacl->defaclobjtype)
				{
					case DEFACLOBJ_RELATION:
						appendStringInfoString(&buffer,
											   " on tables");
						break;
					case DEFACLOBJ_SEQUENCE:
						appendStringInfoString(&buffer,
											   " on sequences");
						break;
					case DEFACLOBJ_FUNCTION:
						appendStringInfoString(&buffer,
											   " on functions");
						break;
					case DEFACLOBJ_TYPE:
						appendStringInfoString(&buffer,
											   " on types");
						break;
					case DEFACLOBJ_NAMESPACE:
						appendStringInfoString(&buffer,
											   " on schemas");
						break;
				}

				if (objname)
				{
					*objname = list_make1(username);
					if (schema)
						*objname = lappend(*objname, schema);
					*objargs = list_make1(psprintf("%c", defacl->defaclobjtype));
				}

				systable_endscan(rcscan);
				table_close(defaclrel, AccessShareLock);
				break;
			}

		case OCLASS_EXTENSION:
			{
				char	   *extname;

				extname = get_extension_name(object->objectId);
				if (!extname)
					elog(ERROR, "cache lookup failed for extension %u",
						 object->objectId);
				appendStringInfoString(&buffer, quote_identifier(extname));
				if (objname)
					*objname = list_make1(extname);
				break;
			}

		case OCLASS_EVENT_TRIGGER:
			{
				HeapTuple	tup;
				Form_pg_event_trigger trigForm;
				char	   *evtname;

				tup = SearchSysCache1(EVENTTRIGGEROID,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for event trigger %u",
						 object->objectId);
				trigForm = (Form_pg_event_trigger) GETSTRUCT(tup);
				evtname = pstrdup(NameStr(trigForm->evtname));
				appendStringInfoString(&buffer, quote_identifier(evtname));
				if (objname)
					*objname = list_make1(evtname);
				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_POLICY:
			{
				Relation	polDesc;
				HeapTuple	tup;
				Form_pg_policy policy;

				polDesc = table_open(PolicyRelationId, AccessShareLock);

				tup = get_catalog_object_by_oid(polDesc, Anum_pg_policy_oid,
												object->objectId);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for policy %u",
						 object->objectId);

				policy = (Form_pg_policy) GETSTRUCT(tup);

				appendStringInfo(&buffer, "%s on ",
								 quote_identifier(NameStr(policy->polname)));
				getRelationIdentity(&buffer, policy->polrelid, objname);
				if (objname)
					*objname = lappend(*objname, pstrdup(NameStr(policy->polname)));

				table_close(polDesc, AccessShareLock);
				break;
			}

		case OCLASS_PUBLICATION:
			{
				char	   *pubname;

				pubname = get_publication_name(object->objectId, false);
				appendStringInfoString(&buffer,
									   quote_identifier(pubname));
				if (objname)
					*objname = list_make1(pubname);
				break;
			}

		case OCLASS_PUBLICATION_REL:
			{
				HeapTuple	tup;
				char	   *pubname;
				Form_pg_publication_rel prform;

				tup = SearchSysCache1(PUBLICATIONREL,
									  ObjectIdGetDatum(object->objectId));
				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for publication table %u",
						 object->objectId);

				prform = (Form_pg_publication_rel) GETSTRUCT(tup);
				pubname = get_publication_name(prform->prpubid, false);

				getRelationIdentity(&buffer, prform->prrelid, objname);
				appendStringInfo(&buffer, " in publication %s", pubname);

				if (objargs)
					*objargs = list_make1(pubname);

				ReleaseSysCache(tup);
				break;
			}

		case OCLASS_SUBSCRIPTION:
			{
				char	   *subname;

				subname = get_subscription_name(object->objectId, false);
				appendStringInfoString(&buffer,
									   quote_identifier(subname));
				if (objname)
					*objname = list_make1(subname);
				break;
			}

		case OCLASS_TRANSFORM:
			{
				Relation	transformDesc;
				HeapTuple	tup;
				Form_pg_transform transform;
				char	   *transformLang;
				char	   *transformType;

				transformDesc = table_open(TransformRelationId, AccessShareLock);

				tup = get_catalog_object_by_oid(transformDesc,
												Anum_pg_transform_oid,
												object->objectId);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for transform %u",
						 object->objectId);

				transform = (Form_pg_transform) GETSTRUCT(tup);

				transformType = format_type_be_qualified(transform->trftype);
				transformLang = get_language_name(transform->trflang, false);

				appendStringInfo(&buffer, "for %s on language %s",
								 transformType,
								 transformLang);
				if (objname)
				{
					*objname = list_make1(transformType);
					*objargs = list_make1(pstrdup(transformLang));
				}

				table_close(transformDesc, AccessShareLock);
			}
			break;

			/*
			 * There's intentionally no default: case here; we want the
			 * compiler to warn if a new OCLASS hasn't been handled above.
			 */
	}

	/*
	 * If a get_object_address representation was requested, make sure we are
	 * providing one.  We don't check objargs, because many of the cases above
	 * leave it as NIL.
	 */
	if (objname && *objname == NIL)
		elog(ERROR, "requested object address for unsupported object class %d: text result \"%s\"",
			 (int) getObjectClass(object), buffer.data);

	return buffer.data;
}

static void
getOpFamilyIdentity(StringInfo buffer, Oid opfid, List **object)
{
	HeapTuple	opfTup;
	Form_pg_opfamily opfForm;
	HeapTuple	amTup;
	Form_pg_am	amForm;
	char	   *schema;

	opfTup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfid));
	if (!HeapTupleIsValid(opfTup))
		elog(ERROR, "cache lookup failed for opfamily %u", opfid);
	opfForm = (Form_pg_opfamily) GETSTRUCT(opfTup);

	amTup = SearchSysCache1(AMOID, ObjectIdGetDatum(opfForm->opfmethod));
	if (!HeapTupleIsValid(amTup))
		elog(ERROR, "cache lookup failed for access method %u",
			 opfForm->opfmethod);
	amForm = (Form_pg_am) GETSTRUCT(amTup);

	schema = get_namespace_name_or_temp(opfForm->opfnamespace);
	appendStringInfo(buffer, "%s USING %s",
					 quote_qualified_identifier(schema,
												NameStr(opfForm->opfname)),
					 NameStr(amForm->amname));

	if (object)
		*object = list_make3(pstrdup(NameStr(amForm->amname)),
							 pstrdup(schema),
							 pstrdup(NameStr(opfForm->opfname)));

	ReleaseSysCache(amTup);
	ReleaseSysCache(opfTup);
}

/*
 * Append the relation identity (quoted qualified name) to the given
 * StringInfo.
 */
static void
getRelationIdentity(StringInfo buffer, Oid relid, List **object)
{
	HeapTuple	relTup;
	Form_pg_class relForm;
	char	   *schema;

	relTup = SearchSysCache1(RELOID,
							 ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(relTup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	relForm = (Form_pg_class) GETSTRUCT(relTup);

	schema = get_namespace_name_or_temp(relForm->relnamespace);
	appendStringInfoString(buffer,
						   quote_qualified_identifier(schema,
													  NameStr(relForm->relname)));
	if (object)
		*object = list_make2(schema, pstrdup(NameStr(relForm->relname)));

	ReleaseSysCache(relTup);
}

/*
 * Auxiliary function to build a TEXT array out of a list of C-strings.
 */
ArrayType *
strlist_to_textarray(List *list)
{
	ArrayType  *arr;
	Datum	   *datums;
	bool	   *nulls;
	int			j = 0;
	ListCell   *cell;
	MemoryContext memcxt;
	MemoryContext oldcxt;
	int			lb[1];

	/* Work in a temp context; easier than individually pfree'ing the Datums */
	memcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "strlist to array",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(memcxt);

	datums = (Datum *) palloc(sizeof(Datum) * list_length(list));
	nulls = palloc(sizeof(bool) * list_length(list));

	foreach(cell, list)
	{
		char	   *name = lfirst(cell);

		if (name)
		{
			nulls[j] = false;
			datums[j++] = CStringGetTextDatum(name);
		}
		else
			nulls[j] = true;
	}

	MemoryContextSwitchTo(oldcxt);

	lb[0] = 1;
	arr = construct_md_array(datums, nulls, 1, &j,
							 lb, TEXTOID, -1, false, 'i');

	MemoryContextDelete(memcxt);

	return arr;
}

/*
 * get_relkind_objtype
 *
 * Return the object type for the relkind given by the caller.
 *
 * If an unexpected relkind is passed, we say OBJECT_TABLE rather than
 * failing.  That's because this is mostly used for generating error messages
 * for failed ACL checks on relations, and we'd rather produce a generic
 * message saying "table" than fail entirely.
 */
ObjectType
get_relkind_objtype(char relkind)
{
	switch (relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_PARTITIONED_TABLE:
			return OBJECT_TABLE;
		case RELKIND_INDEX:
		case RELKIND_PARTITIONED_INDEX:
			return OBJECT_INDEX;
		case RELKIND_SEQUENCE:
			return OBJECT_SEQUENCE;
		case RELKIND_VIEW:
			return OBJECT_VIEW;
		case RELKIND_MATVIEW:
			return OBJECT_MATVIEW;
		case RELKIND_FOREIGN_TABLE:
			return OBJECT_FOREIGN_TABLE;
		case RELKIND_TOASTVALUE:
			return OBJECT_TABLE;
		default:
			/* Per above, don't raise an error */
			return OBJECT_TABLE;
	}
}
