from pg import *

# This library file contains some common functions not directly provided by the
# PostGres C library. It offers too a keyword interface for pgmodule connect
# function.

# encapsulate pg connect function for keywords enabling
def doconnect(dbname = None, host = None, port = None, opt = None, tty = None):
	return connect(dbname, host, port, opt, tty)

# list all databases on the server 
def ListDB(pgcnx):
	list = []
	for node in pgcnx.query("SELECT datname FROM pg_database").getresult():
		list.append(node[0])
	return list

# list all tables (classes) in the selected database
def ListTables(pgcnx):
	list = []
	for node in pgcnx.query("""SELECT relname FROM pg_class
				WHERE relkind = 'r' AND
					relname !~ '^Inv' AND
					relname !~ '^pg_'""").getresult():
		list.append(node[0])
	return list

# list table fields (attribute) in given table
def ListAllFields(pgcnx, table):
	list = []
	for node in pgcnx.query("""SELECT c.relname, a.attname, t.typname
							FROM pg_class c, pg_attribute a, pg_type t
							WHERE c.relname = '%s' AND
								a.attnum > 0 AND
								a.attrelid = c.oid AND
								a.atttypid = t.oid
							ORDER BY relname, attname""" % table).getresult():
		list.append(node[1], node[2])
	return list
