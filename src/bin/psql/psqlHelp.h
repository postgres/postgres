/*-------------------------------------------------------------------------
 *
 * psqlHelp.h--
 *	  Help for query language syntax
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: psqlHelp.h,v 1.50 1998/08/25 21:36:58 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

struct _helpStruct
{
	char	   *cmd;			/* the command name */
	char	   *help;			/* the help associated with it */
	char	   *syntax;			/* the syntax associated with it */
};

static struct _helpStruct QL_HELP[] = {
	{"abort transaction",
		"abort the current transaction",
    "\
\tabort [transaction|work];"},
	{"alter table",
		"add/rename attributes, rename tables",
	"\
\tALTER TABLE class_name [*] ADD COLUMN attr type\n\
\tALTER TABLE class_name [*] RENAME [COLUMN] attr1 TO attr2\n\
\tALTER TABLE class_name1 RENAME TO class_name2"},
	{"alter user",
		"alter system information for a user",
	"\
\tALTER USER user_name\n\
\t[WITH PASSWORD password]\n\
\t[CREATEDB | NOCCREATEDB]\n\
\t[CREATEUSER | NOCREATEUSER]\n\
\t[IN GROUP group_1, ...groupN]\n\
\t[VALID UNTIL 'abstime'];"},
	{"begin work",
		"begin a new transaction",
	"\
\tBEGIN [WORK|TRANSACTION];"},
	{"cluster",
		"create a clustered index (from an existing index)",
	"\
\tCLUSTER index_name ON relation_name"},
	{"close",
		"close an existing cursor (cursor)",
	"\
\tCLOSE cursorname;"},
	{"commit work",
		"commit a transaction",
	"\
\tCOMMIT [WORK|TRANSACTION]"},
	{"copy",
		"copy data to and from a table",
	"\
\tCOPY [BINARY] class_name [WITH OIDS]\n\
\tTO|FROM filename|STDIN|STDOUT [USING DELIMITERS 'delim'];"},
	{"create",
		"Please be more specific:",
	"\
\tcreate aggregate\n\
\tcreate database\n\
\tcreate function\n\
\tcreate index\n\
\tcreate operator\n\
\tcreate rule\n\
\tcreate sequence\n\
\tcreate table\n\
\tcreate trigger\n\
\tcreate type\n\
\tcreate view"},
	{"create aggregate",
		"define an aggregate function",
	"\
\tCREATE AGGREGATE agg_name [AS] (BASETYPE = data_type, \n\
\t[SFUNC1 = sfunc_1, STYPE1 = sfunc1_return_type]\n\
\t[SFUNC2 = sfunc_2, STYPE2 = sfunc2_return_type]\n\
\t[,FINALFUNC = final-function]\n\
\t[,INITCOND1 = initial-cond1][,INITCOND2 = initial-cond2]);"},
	{"create database",
		"create a database",
	"\
\tCREATE DATABASE dbname [WITH LOCATION = 'dbpath']"},
	{"create function",
		"create a user-defined function",
	"\
\tCREATE FUNCTION function_name ([type1, ...typeN]) RETURNS return_type\n\
\tAS 'object_filename'|'sql-queries'\n\
\tLANGUAGE 'c'|'sql'|'internal';"},
	{"create index",
		"construct an index",
	"\
\tCREATE [UNIQUE] INDEX indexname ON class_name [USING access_method]\n\
( attr1 [type_class1], ...attrN | funcname(attr1, ...) [type_class] );"},
	{"create operator",
		"create a user-defined operator",
	"\
\tCREATE OPERATOR operator_name (\n\
\t[LEFTARG = type1][,RIGHTARG = type2]\n\
\t,PROCEDURE = func_name,\n\
\t[,COMMUTATOR = com_op][,NEGATOR = neg_op]\n\
\t[,RESTRICT = res_proc][,HASHES]\n\
\t[,JOIN = join_proc][,SORT = sort_op1...sort_opN]);"},
	{"create rule",
		"define a new rule",
	"\
\tCREATE RULE rule_name AS ON\n\
\t[SELECT|UPDATE|DELETE|INSERT]\n\
\tTO object [WHERE qual]\n\
\tDO [INSTEAD] [action|NOTHING|[actions]];"},
	{"create sequence",
		"create a new sequence number generator",
	"\
\tCREATE SEQUENCE sequence_name\n\
\t[INCREMENT number]\n\
\t[START number]\n\
\t[MINVALUE number]\n\
\t[MAXVALUE number]\n\
\t[CACHE number]\n\
\t[CYCLE];"},
	{"create table",
		"create a new table",
	"\
\tCREATE TABLE class_name\n\
\t(attr1 type1 [DEFAULT expression] [NOT NULL], ...attrN)\n\
\t[INHERITS (class_name1, ...class_nameN)\n\
\t[[CONSTRAINT name] CHECK condition1, ...conditionN] ]\n\
;"},
	{"create trigger",
		"create a new trigger",
	"\
\tCREATE TRIGGER trigger_name AFTER|BEFORE event1 [OR event2 [OR event3] ]\n\
\tON class_name FOR EACH ROW|STATEMENT\n\
\tEXECUTE PROCEDURE func_name ([arguments])\n\
\n\
\teventX is one of INSERT, DELETE, UPDATE"},
	{"create type",
		"create a new user-defined base data type",
	"\
\tCREATE TYPE typename (\n\
\tINTERNALLENGTH = (number|VARIABLE),\n\
\t[EXTERNALLENGTH = (number|VARIABLE),]\n\
\tINPUT = input_function, OUTPUT = output_function\n\
\t[,ELEMENT = typename][,DELIMITER = character][,DEFAULT=\'<string>\']\n\
\t[,SEND = send_function][,RECEIVE = receive_function][,PASSEDBYVALUE]);"},
	{"create user",
		"create a new user",
	"\
\tCREATE USER user_name\n\
\t[WITH PASSWORD password]\n\
\t[CREATEDB | NOCREATEDB]\n\
\t[CREATEUSER | NOCREATEUSER]\n\
\t[IN GROUP group1, ...groupN]\n\
\t[VALID UNTIL 'abstime'];"},
	{"create view",
		"create a view",
	"\
\tCREATE VIEW view_name AS\n\
\tSELECT [DISTINCT [ON attrN]]\n\
\texpr1 [AS attr1], ...exprN\n\
\t[FROM from_list]\n\
\t[WHERE qual]\n\
\t[GROUP BY group_list];"},
	{"declare",
		"set up a cursor",
	"\
\tDECLARE cursorname [BINARY] CURSOR FOR\n\
\tSELECT [DISTINCT [ON attrN]]\n\
\texpr1 [AS attr1], ...exprN\n\
\t[FROM from_list]\n\
\t[WHERE qual]\n\
\t[GROUP BY group_list]\n\
\t[HAVING having_clause]\n\
\t[ORDER BY attr1 [USING op1], ...attrN]\n\
\t[UNION [ALL] SELECT ...];"},
	{"delete",
		"delete tuples",
	"\
\tDELETE FROM class_name [WHERE qual];"},
	{"drop",
		"Please be more specific:",
	"\
\tdrop aggregate\n\
\tdrop database\n\
\tdrop function\n\
\tdrop index\n\
\tdrop operator\n\
\tdrop rule\n\
\tdrop sequence\n\
\tdrop table\n\
\tdrop trigger\n\
\tdrop type\n\
\tdrop view"},
	{"drop aggregate",
		"remove an aggregate function",
	"\
\tDROP AGGREGATE agg_name agg_type|*;"},
	{"drop database",
		"remove a database",
	"\
\tDROP DATABASE dbname"},
	{"drop function",
		"remove a user-defined function",
	"\
\tDROP FUNCTION funcname ([type1, ...typeN]);"},
	{"drop index",
		"remove an existing index",
	"\
\tDROP INDEX indexname;"},
	{"drop operator",
		"remove a user-defined operator",
	"\
\tDROP OPERATOR operator_name ([ltype|NONE],[RTYPE|none]);"},
	{"drop rule",
		"remove a rule",
	"\
\tDROP RULE rulename;"},
	{"drop sequence",
		"remove a sequence number generator",
	"\
\tDROP SEQUENCE sequence_name[, ...sequence_nameN];"},
	{"drop table",
		"remove a table",
	"\
\tDROP TABLE class_name1, ...class_nameN;"},
	{"drop trigger",
		"remove a trigger",
	"\
\tDROP TRIGGER trigger_name ON class_name;"},
	{"drop type",
		"remove a user-defined base type",
	"\
\tDROP TYPE typename;"},
	{"drop user",
		"remove a user from the system",
	"\
\tDROP USER user_name;"},
	{"drop view",
		"remove a view",
	"\
\tDROP VIEW view_name"},
	{"end work",
		"end the current transaction",
	"\
\tEND [WORK|TRANSACTION];"},
	{"explain",
		"explain the query execution plan",
	"\
\tEXPLAIN [VERBOSE] query"},
	{"fetch",
		"retrieve tuples from a cursor",
	"\
\tFETCH [FORWARD|BACKWARD] [number|ALL] [IN cursorname];"},
	{"grant",
		"grant access control to a user or group",
	"\
\tGRANT privilege1, ...privilegeN ON rel1, ...relN TO \n\
[PUBLIC|GROUP group|username]\n\
\t privilege is ALL|SELECT|INSERT|UPDATE|DELETE|RULE"},
	{"insert",
		"insert tuples",
	"\
\tINSERT INTO class_name [(attr1, ...attrN)]\n\
\tVALUES (expr1,..exprN) |\n\
\tSELECT [DISTINCT [ON attrN]]\n\
\texpr1, ...exprN\n\
\t[FROM from_clause]\n\
\t[WHERE qual]\n\
\t[GROUP BY group_list]\n\
\t[HAVING having_clause]\n\
\t[UNION [ALL] SELECT ...];"},
	{"listen",
		"listen for notification on a relation name",
	"\
\tLISTEN class_name|\"name\""},
	{"load",
		"dynamically load a module",
	"\
\tLOAD 'filename';"},
	{"lock",
		"exclusive lock a table inside a transaction",
	"\
\tLOCK [TABLE] class_name;"},
	{"move",
		"move an cursor position",
	"\
\tMOVE [FORWARD|BACKWARD] [number|ALL] [IN cursorname];"},
	{"notify",
		"signal all frontends and backends listening on a relation",
	"\
\tNOTIFY class_name"},
	{"reset",
		"set run-time environment back to default",
#ifdef MULTIBYTE
	"\
\tRESET DateStyle|GEQO|R_PLANS|CLIENT_ENCODING"},
#else
	"\
\tRESET DateStyle|GEQO|R_PLANS"},
#endif
	{"revoke",
		"revoke access control from a user or group",
	"\
\tREVOKE privilege1, ...privilegeN ON rel1, ...relN FROM \n\
[PUBLIC|GROUP group|username]\n\
\t privilege is ALL|SELECT|INSERT|UPDATE|DELETE|RULE"},
	{"rollback work",
		"abort a transaction",
	"\
\tROLLBACK [WORK|TRANSACTION]"},
	{"select",
		"retrieve tuples",
	"\
\tSELECT [DISTINCT [ON attrN]] expr1 [AS attr1], ...exprN\n\
\t[INTO [TABLE] class_name]\n\
\t[FROM from_list]\n\
\t[WHERE qual]\n\
\t[GROUP BY group_list]\n\
\t[HAVING having_clause]\n\
\t[ORDER BY attr1 [ASC|DESC] [USING op1], ...attrN ]\n\
\t[UNION [ALL] SELECT ...];"},
	{"set",
		"set run-time environment",
#ifdef MULTIBYTE
	"\
\tSET DateStyle TO 'ISO'|'SQL'|'Postgres'|'European'|'US'|'NonEuropean'\n\
set GEQO TO 'ON[=#]'|'OFF'\n\
set R_PLANS TO 'ON'|'OFF'\n\
set CLIENT_ENCODING TO 'EUC_JP'|'SJIS'|'EUC_CN'|'EUC_KR'|'EUC_TW'|'MULE_INTERNAL'|'LATIN1'|'LATIN2'|'LATIN3'|'LATIN4'|'LATIN5'"},
#else
	"\
\tSET DateStyle TO 'ISO'|'SQL'|'Postgres'|'European'|'US'|'NonEuropean'\n\
set GEQO TO 'ON[=#]'|'OFF'\n\
set R_PLANS TO 'ON'| 'OFF'"},
#endif
	{"show",
		"show current run-time environment",
#ifdef MULTIBYTE
	"\
\tSHOW DateStyle|GEQO|R_PLANS|CLIENT_ENCODING"},
#else
	"\
\tSHOW DateStyle|GEQO|R_PLANS"},
#endif
	{"unlisten",
		"unlisten for notification on a relation name",
	"\
\tUNLISTEN class_name|\"name\"|\"*\""},
	{"update",
		"update tuples",
	"\
\tUPDATE class_name SET attr1 = expr1, ...attrN = exprN\n\
\t [FROM from_clause]\n\
\t[WHERE qual];"},
	{"vacuum",
		"vacuum the database, i.e. cleans out deleted records, updates statistics",
	"\
\tVACUUM [VERBOSE] [ANALYZE] [table]\n\
\tor\n\
\tVACUUM [VERBOSE]  ANALYZE  [table [(attr1, ...attrN)]];"},
	{NULL, NULL, NULL}			/* important to keep a NULL terminator
								 * here! */
};
