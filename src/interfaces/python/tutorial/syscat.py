# syscat.py - parses some system catalogs
# inspired from the PostgreSQL tutorial 
# adapted to Python 1995 by Pascal ANDRE

print """
__________________________________________________________________
MODULE SYSCAT.PY : PARSES SOME POSTGRESQL SYSTEM CATALOGS

This module is designed for being imported from python prompt

In order to run the samples included here, first create a connection
using :                        cnx = advanced.DB(...)

The "..." should be replaced with whatever arguments you need to open an 
existing database.  Usually all you need is the name of the database and,
in fact, if it is the same as your login name, you can leave it empty.

then start the demo with:      syscat.demo(cnx)

Some results may be empty, depending on your base status."

__________________________________________________________________
"""

from pg import DB
import sys

# waits for a key
def wait_key():
	print "Press <enter>"
	sys.stdin.read(1)

# lists all simple indices
def list_simple_ind(pgcnx):
	result = pgcnx.query("""SELECT bc.relname AS class_name,
			ic.relname AS index_name, a.attname
		FROM pg_class bc, pg_class ic, pg_index i, pg_attribute a
		WHERE i.indrelid = bc.oid AND i.indexrelid = bc.oid
				AND i.indkey[0] = a.attnum AND a.attrelid = bc.oid
				AND i.indproc = '0'::oid
		ORDER BY class_name, index_name, attname""")
	return result

# list all user defined attributes and their type in user-defined classes
def list_all_attr(pgcnx):
	result = pgcnx.query("""SELECT c.relname, a.attname, t.typname
		FROM pg_class c, pg_attribute a, pg_type t
		WHERE c.relkind = 'r' and c.relname !~ '^pg_'
			AND c.relname !~ '^Inv' and a.attnum > 0
			AND a.attrelid = c.oid and a.atttypid = t.oid
			ORDER BY relname, attname""")
	return result

# list all user defined base type
def list_user_base_type(pgcnx):
	result = pgcnx.query("""SELECT u.usename, t.typname
			FROM pg_type t, pg_user u
			WHERE u.usesysid = int2in(int4out(t.typowner))
				AND t.typrelid = '0'::oid and t.typelem = '0'::oid 
				AND u.usename <> 'postgres' order by usename, typname""")
	return result 

# list all right-unary operators
def list_right_unary_operator(pgcnx):
	result = pgcnx.query("""SELECT o.oprname AS right_unary,
			lt.typname AS operand, result.typname AS return_type
		FROM pg_operator o, pg_type lt, pg_type result
		WHERE o.oprkind='r' and o.oprleft = lt.oid
			AND o.oprresult = result.oid
		ORDER BY operand""")
	return result

# list all left-unary operators
def list_left_unary_operator(pgcnx):
	result = pgcnx.query("""SELECT o.oprname AS left_unary,
			rt.typname AS operand, result.typname AS return_type
		FROM pg_operator o, pg_type rt, pg_type result
		WHERE o.oprkind='l' AND o.oprright = rt.oid
			AND o.oprresult = result.oid
		ORDER BY operand""")
	return result

# list all binary operators
def list_binary_operator(pgcnx):
	result = pgcnx.query("""SELECT o.oprname AS binary_op,
			rt.typname AS right_opr, lt.typname AS left_opr,
			result.typname AS return_type
		FROM pg_operator o, pg_type rt, pg_type lt, pg_type result
		WHERE o.oprkind = 'b' AND o.oprright = rt.oid
			AND o.oprleft = lt.oid AND o.oprresult = result.oid""")
	return result

# returns the name, args and return type from all function of lang l
def list_lang_func(pgcnx, l):
	result = pgcnx.query("""SELECT p.proname, p.pronargs, t.typname
		FROM pg_proc p, pg_language l, pg_type t
		WHERE p.prolang = l.oid AND p.prorettype = t.oid
			AND l.lanname = '%s'
		ORDER BY proname""" % l)
	return result

# lists all the aggregate functions and the type to which they can be applied
def list_agg_func(pgcnx):
	result = pgcnx.query("""SELECT a.aggname, t.typname
		FROM pg_aggregate a, pg_type t
		WHERE a.aggbasetype = t.oid
		ORDER BY aggname, typname""")
	return result

# lists all the operator classes that can be used with each access method as
# well as the operators that can be used with the respective operator classes
def list_op_class(pgcnx):
	result = pgcnx.query("""SELECT am.amname, opc.opcname, opr.oprname
		FROM pg_am am, pg_amop amop, pg_opclass opc, pg_operator opr
		WHERE amop.amopid = am.oid and amop.amopclaid = opc.oid
			AND amop.amopopr = opr.oid order by amname, opcname, oprname""")
	return result

# demo function - runs all examples
def demo(pgcnx):
	import sys, os
	save_stdout = sys.stdout
	sys.stdout = os.popen("more", "w")
	print "Listing simple indices ..."
	print list_simple_ind(pgcnx)
	print "Listing all attributes ..."
	print list_all_attr(pgcnx)
	print "Listing all user-defined base types ..."
	print list_user_base_type(pgcnx)
	print "Listing all left-unary operators defined ..."
	print list_left_unary_operator(pgcnx)
	print "Listing all right-unary operators defined ..."
	print list_right_unary_operator(pgcnx)
	print "Listing all binary operators ..."
	print list_binary_operator(pgcnx)
	print "Listing C external function linked ..."
	print list_lang_func(pgcnx, 'C')
	print "Listing C internal functions ..."
	print list_lang_func(pgcnx, 'internal')
	print "Listing SQL functions defined ..."
	print list_lang_func(pgcnx, 'sql')
	print "Listing 'aggregate functions' ..."
	print list_agg_func(pgcnx)
	print "Listing 'operator classes' ..."
	print list_op_class(pgcnx)
	del sys.stdout
	sys.stdout = save_stdout
