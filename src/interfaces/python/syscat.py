# syscat.py - parses some system catalogs
# inspired from the PostgreSQL tutorial 
# adapted to Python 1995 by Pascal ANDRE

print "____________________________________________________________________"
print
print "MODULE SYSCAT.PY : PARSES SOME POSTGRESQL SYSTEM CATALOGS"
print
print "This module is designed for being imported from python prompt"
print
print "In order to run the samples included here, first create a connection"
print "using :                        cnx = syscat.connect(...)"
print "then start the demo with:      syscat.demo(cnx)"
print
print "Some results may be empty, depending on your base status."
print
print "____________________________________________________________________"
print

from pgext import *
from pgtools import *

# lists all simple indices
def list_simple_ind(pgcnx):
    result = pgcnx.query("select bc.relname "                        \
	 "as class_name, ic.relname as index_name, a.attname "         \
	 "from pg_class bc, pg_class ic, pg_index i, pg_attribute a "  \
	 "where i.indrelid = bc.oid and i.indexrelid = bc.oid "        \
	 "  and i.indkey[0] = a.attnum and a.attrelid = bc.oid "       \
	 "  and i.indproc = '0'::oid "                                 \
	 "order by class_name, index_name, attname")
    return result

# list all user defined attributes and their type in user-defined classes
def list_all_attr(pgcnx):
    result = pgcnx.query("select c.relname, a.attname, t.typname "   \
	 "from pg_class c, pg_attribute a, pg_type t "                 \
	 "where c.relkind = 'r' and c.relname !~ '^pg_' "              \
         "  and c.relname !~ '^Inv' and a.attnum > 0 "                 \
         "  and a.attrelid = c.oid and a.atttypid = t.oid "            \
         "order by relname, attname")
    return result

# list all user defined base type
def list_user_base_type(pgcnx):
    result = pgcnx.query("select u.usename, t.typname "              \
	 "from pg_type t, pg_user u "                                  \
         "where u.usesysid = int2in(int4out(t.typowner)) "             \
         "  and t.typrelid = '0'::oid and t.typelem = '0'::oid "       \
         "  and u.usename <> 'postgres' order by usename, typname")
    return result 

# list all right-unary operators
def list_right_unary_operator(pgcnx):
    result = pgcnx.query("select o.oprname as right_unary, "          \
         "  lt.typname as operand, result.typname as return_type "    \
         "from pg_operator o, pg_type lt, pg_type result "            \
         "where o.oprkind='r' and o.oprleft = lt.oid "                \
         "  and o.oprresult = result.oid order by operand")
    return result

# list all left-unary operators
def list_left_unary_operator(pgcnx):
    result = pgcnx.query("select o.oprname as left_unary, "          \
         "  rt.typname as operand, result.typname as return_type "  \
         "from pg_operator o, pg_type rt, pg_type result "          \
         "where o.oprkind='l' and o.oprright = rt.oid "             \
         "  and o.oprresult = result.oid order by operand")
    return result

# list all binary operators
def list_binary_operator(pgcnx):
    result = pgcnx.query("select o.oprname as binary_op, "           \
        "  rt.typname as right_opr, lt.typname as left_opr, "     \
        "  result.typname as return_type "                             \
        "from pg_operator o, pg_type rt, pg_type lt, pg_type result " \
        "where o.oprkind = 'b' and o.oprright = rt.oid "            \
        "  and o.oprleft = lt.oid and o.oprresult = result.oid")
    return result

# returns the name, args and return type from all function of lang l
def list_lang_func(pgcnx, l):
    result = pgcnx.query("select p.proname, p.pronargs, t.typname "  \
        "from pg_proc p, pg_language l, pg_type t "                    \
        "where p.prolang = l.oid and p.prorettype = t.oid "            \
        "  and l.lanname = '%s' order by proname" % l)
    return result

# lists all the aggregate functions and the type to which they can be applied
def list_agg_func(pgcnx):
    result = pgcnx.query("select a.aggname, t.typname "              \
         "from pg_aggregate a, pg_type t "                             \
         "where a.aggbasetype = t.oid order by aggname, typname")
    return result

# lists all the operator classes that can be used with each access method as
# well as the operators that can be used with the respective operator classes
def list_op_class(pgcnx):
    result = pgcnx.query("select am.amname, opc.opcname, opr.oprname " \
        "from pg_am am, pg_amop amop, pg_opclass opc, pg_operator opr "  \
        "where amop.amopid = am.oid and amop.amopclaid = opc.oid "       \
        "  and amop.amopopr = opr.oid order by amname, opcname, oprname")
    return result

# demo function - runs all examples
def demo(pgcnx):
	#import sys, os
	#save_stdout = sys.stdout
	#sys.stdout = os.popen("more", "w")
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
	#del sys.stdout
	#sys.stdout = save_stdout
