from pg import *

# This library file contains some common functions not directly provided by the
# PostGres C library. It offers too a keyword interface for pgmodule connect
# function.

# encapsulate pg connect function for keywords enabling
def doconnect(dbname = None, host = None, port = None, opt = None, tty = None):
	return connect(dbname, host, port, opt, tty)

# list all databases on the server 
def ListDB(pgcnx):
	result = pgcnx.query("select datname from pg_database")
	list = []
	for node in result:
		list.append(result[i][0])
	return list

# list all tables (classes) in the selected database
def ListTables(pgcnx):
	result = pgcnx.query("select relname from pg_class "	\
		"where relkind = 'r' "				\
		"  and relname !~ '^Inv' "			\
		"  and relname !~ '^pg_'")
	list = []
	for node in result:
		list.append(node[0])
	return list

# list table fields (attribute) in given table
def ListAllFields(pgcnx, table):
	result = pgcnx.query("select c.relname, a.attname, t.typname " \
		"from pg_class c, pg_attribute a, pg_type t "	\
		"where c.relname = '%s' "			\
		"  and a.attnum > 0"				\
		"  and a.attrelid = c.oid"			\
		"  and a.atttypid = t.oid "			\
		"order by relname, attname" % table)
	# personnal preference ... so I leave the original query
	list = []
	for node in result:
		list.append(node[1], node[2])
	return list
