/*-------------------------------------------------------------------------
 *
 * catname.h
 *	  POSTGRES system catalog relation name definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catname.h,v 1.18 2001/01/24 19:43:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATNAME_H
#define CATNAME_H



#define  AggregateRelationName "pg_aggregate"
#define  AccessMethodRelationName "pg_am"
#define  AccessMethodOperatorRelationName "pg_amop"
#define  AccessMethodProcedureRelationName "pg_amproc"
#define  AttributeRelationName "pg_attribute"
#define  DatabaseRelationName "pg_database"
#define  DescriptionRelationName "pg_description"
#define  GroupRelationName "pg_group"
#define  IndexRelationName "pg_index"
#define  InheritProcedureRelationName "pg_inheritproc"
#define  InheritsRelationName "pg_inherits"
#define  InheritancePrecidenceListRelationName "pg_ipl"
#define  LanguageRelationName "pg_language"
#define  LargeObjectRelationName "pg_largeobject"
#define  ListenerRelationName "pg_listener"
#define  LogRelationName "pg_log"
#define  OperatorClassRelationName "pg_opclass"
#define  OperatorRelationName "pg_operator"
#define  ProcedureRelationName "pg_proc"
#define  RelationRelationName "pg_class"
#define  RewriteRelationName "pg_rewrite"
#define  ShadowRelationName "pg_shadow"
#define  StatisticRelationName "pg_statistic"
#define  TypeRelationName "pg_type"
#define  VariableRelationName "pg_variable"
#define  VersionRelationName "pg_version"
#define  AttrDefaultRelationName "pg_attrdef"
#define  RelCheckRelationName "pg_relcheck"
#define  TriggerRelationName "pg_trigger"

extern char *SharedSystemRelationNames[];

#endif	 /* CATNAME_H */
