/*-------------------------------------------------------------------------
 *
 * catname.h--
 *	  POSTGRES system catalog relation name definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catname.h,v 1.6 1997/09/08 02:34:47 momjian Exp $
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
#define  DefaultsRelationName "pg_defaults"
#define  DemonRelationName "pg_demon"
#define  GroupRelationName "pg_group"
#define  HostsRelationName "pg_hosts"
#define  IndexRelationName "pg_index"
#define  InheritProcedureRelationName "pg_inheritproc"
#define  InheritsRelationName "pg_inherits"
#define  InheritancePrecidenceListRelationName "pg_ipl"
#define  LanguageRelationName "pg_language"
#define  ListenerRelationName "pg_listener"
#define  LogRelationName "pg_log"
#define  MagicRelationName "pg_magic"
#define  OperatorClassRelationName "pg_opclass"
#define  OperatorRelationName "pg_operator"
#define  ProcedureRelationName "pg_proc"
#define  RelationRelationName "pg_class"
#define  RewriteRelationName "pg_rewrite"
#define  ServerRelationName "pg_server"
#define  StatisticRelationName "pg_statistic"
#define  TimeRelationName "pg_time"
#define  TypeRelationName "pg_type"
#define  UserRelationName "pg_user"
#define  VariableRelationName "pg_variable"
#define  VersionRelationName "pg_version"
#define  AttrDefaultRelationName "pg_attrdef"
#define  RelCheckRelationName "pg_relcheck"
#define  TriggerRelationName "pg_trigger"

extern char *SharedSystemRelationNames[];

#endif							/* CATNAME_H */
