#! /usr/local/bin/python
# advanced.py - demo of advanced features of PostGres. Some may not be ANSI.
# inspired from the Postgres tutorial 
# adapted to Python 1995 by Pascal Andre

print "__________________________________________________________________"
print "MODULE ADVANCED.PY : ADVANCED POSTGRES SQL COMMANDS TUTORIAL"
print
print "This module is designed for being imported from python prompt"
print
print "In order to run the samples included here, first create a connection"
print "using :                        cnx = advanced.connect(...)"
print "then start the demo with:      advanced.demo(cnx)"
print "__________________________________________________________________"

from pgtools import *
from pgext import *

# inheritance features
def inherit_demo(pgcnx):
	print "-----------------------------"
	print "-- Inheritance:"
	print "--	a table can inherit from zero or more tables. A query"
	print "--	can reference either all rows of a table or all rows "
	print "--	of a table plus all of its descendants."
	print "-----------------------------"
	print
	print "-- For example, the capitals table inherits from cities table."
	print "-- (It inherits  all data fields from cities.)"
	print
	print "CREATE TABLE cities ("
	print "    name		text,"
	print "	   population	float8,"
	print "    altitude	int"
	print ")"
	print
	print "CREATE TABLE capitals ("
	print "    state	char2"
	print ") INHERITS (cities)"
	pgcnx.query("CREATE TABLE cities ("	\
		"name		text,"		\
		"population	float8,"	\
		"altitude	int)")
	pgcnx.query("CREATE TABLE capitals ("	\
		"state		char2) INHERITS (cities)")
	wait_key()
	print
	print "-- now, let's populate the tables"
	print
	print "INSERT INTO cities VALUES ('San Francisco', 7.24E+5, 63)"
	print "INSERT INTO cities VALUES ('Las Vegas', 2.583E+5, 2174)"
	print "INSERT INTO cities VALUES ('Mariposa', 1200, 1953)"
	print
	print "INSERT INTO capitals VALUES ('Sacramento', 3.694E+5, 30, 'CA')"
	print "INSERT INTO capitals VALUES ('Madison', 1.913E+5, 845, 'WI')"
	print
	pgcnx.query(
		"INSERT INTO cities VALUES ('San Francisco', 7.24E+5, 63)")
	pgcnx.query(
		"INSERT INTO cities VALUES ('Las Vegas', 2.583E+5, 2174)")
	pgcnx.query(
		"INSERT INTO cities VALUES ('Mariposa', 1200, 1953)")
	pgcnx.query("INSERT INTO capitals"	\
		" VALUES ('Sacramento', 3.694E+5, 30, 'CA')")
	pgcnx.query("INSERT INTO capitals"	\
		" VALUES ('Madison', 1.913E+5, 845, 'WI')")
	print
	print "SELECT * FROM cities"
	q = pgcnx.query("SELECT * FROM cities")
	display(q.listfields(), q.getresult())
	print "SELECT * FROM capitals"
	q = pgcnx.query("SELECT * FROM capitals")
	display(q.listfields(), q.getresult())
	print
	print "-- like before, a regular query references rows of the base"
	print "-- table only"
	print
	print "SELECT name, altitude"
	print "FROM cities"
	print "WHERE altitude > 500;"
	q = pgcnx.query("SELECT name, altitude "	\
		"FROM cities "			\
		"WHERE altitude > 500")
	display(q.listfields(), q.getresult())
	print
	print "-- on the other hand, you can find all cities, including "
	print "-- capitals, that are located at an altitude of 500 'ft "
	print "-- or higher by:"
	print
	print "SELECT c.name, c.altitude"
	print "FROM cities* c"
	print "WHERE c.altitude > 500"
	q = pgcnx.query("SELECT c.name, c.altitude "	\
		"FROM cities* c "			\
		"WHERE c.altitude > 500")
	display(q.listfields(), q.getresult())

# time travel features
def time_travel(pgcnx):
	print "-----------------------------"
	print "-- Time Travel:"
	print "--	this feature allows you to run historical queries. "
	print "-----------------------------"
	print
	print "-- first, let's make some changes to the cities table (suppose"
	print "-- Mariposa's population grows 10% this year)"
	print
	print "UPDATE cities"
	print "SET population = population * 1.1"
	print "WHERE name = 'Mariposa';"
	pgcnx.query("UPDATE cities "			\
		"SET population = population * 1.1"	\
		"WHERE name = 'Mariposa'")
	wait_key()
	print
	print "-- the default time is the current time ('now'):"
	print
	print "SELECT * FROM cities WHERE name = 'Mariposa';"
	q = pgcnx.query("SELECT * FROM cities WHERE name = 'Mariposa'")
	display(q.listfields(), q.getresult())
	print
	print "-- we can also retrieve the population of Mariposa ever has. "
	print "-- ('epoch' is the earliest time representable by the system)"
	print
	print "SELECT name, population"
	print "FROM cities['epoch', 'now']  -- can be abbreviated to cities[,]"
	print "WHERE name = 'Mariposa';"
	q = pgcnx.query("SELECT name, population "
		"FROM cities['epoch', 'now'] "
		"WHERE name = 'Mariposa'")
	display(q.listfields(), q.getresult())

# arrays attributes 
def array_demo(pgcnx):
	print "----------------------"
	print "-- Arrays:"
	print "--      attributes can be arrays of base types or user-defined "
	print "--      types"
	print "----------------------"
	print
	print "CREATE TABLE sal_emp ("
	print "    name			text,"
	print "    pay_by_quarter	int4[],"
	print "    schedule		char16[][]"
	print ")"
	pgcnx.query("CREATE TABLE sal_emp ("		\
		"name	text,"				\
		"pay_by_quarter	int4[],"		\
		"schedule	char16[][])")
	wait_key()
	print
	print "-- insert instances with array attributes.  "
	print "   Note the use of braces"
	print
	print "INSERT INTO sal_emp VALUES ("
	print "    'Bill',"
	print "    '{10000,10000,10000,10000}',"
	print "    '{{\"meeting\", \"lunch\"}, {}}')"
	print
	print "INSERT INTO sal_emp VALUES ("
	print "    'Carol',"
	print "    '{20000,25000,25000,25000}',"
	print "    '{{\"talk\", \"consult\"}, {\"meeting\"}}')"
	print
	pgcnx.query("INSERT INTO sal_emp VALUES ("	\
		"'Bill', '{10000,10000,10000,10000}',"	\
		"'{{\"meeting\", \"lunch\"}, {}}')")
	pgcnx.query("INSERT INTO sal_emp VALUES ("	\
		"'Carol', '{20000,25000,25000,25000}',"	\
		"'{{\"talk\", \"consult\"}, {\"meeting\"}}')")
	wait_key()
	print
	print "----------------------"
	print "-- queries on array attributes"
	print "----------------------"
	print
	print "SELECT name FROM sal_emp WHERE"
	print "  sal_emp.pay_by_quarter[1] <> sal_emp.pay_by_quarter[2]"
	print
	q = pgcnx.query("SELECT name FROM sal_emp WHERE "	\
		"sal_emp.pay_by_quarter[1] <> sal_emp.pay_by_quarter[2]")
	display(q.listfields(), q.getresult())
	print
	print "-- retrieve third quarter pay of all employees"
	print 
	print "SELECT sal_emp.pay_by_quarter[3] FROM sal_emp"
	print
	q = pgcnx.query("SELECT sal_emp.pay_by_quarter[3] FROM sal_emp")
	display(q.listfields(), q.getresult())
	print
	print "-- select subarrays"
	print 
	print "SELECT sal_emp.schedule[1:2][1:1] FROM sal_emp WHERE	"
	print "     sal_emp.name = 'Bill'"
	q = pgcnx.query("SELECT sal_emp.schedule[1:2][1:1] FROM sal_emp WHERE " \
		"sal_emp.name = 'Bill'")
	display(q.listfields(), q.getresult())

# base cleanup
def demo_cleanup(pgcnx):
	print "-- clean up (you must remove the children first)"
	print "DROP TABLE sal_emp"
	print "DROP TABLE capitals"
	print "DROP TABLE cities;"
	pgcnx.query("DROP TABLE sal_emp")
	pgcnx.query("DROP TABLE capitals")
	pgcnx.query("DROP TABLE cities")

# main demo function
def demo(pgcnx):
	inherit_demo(pgcnx)
	time_travel(pgcnx)
	array_demo(pgcnx)
	demo_cleanup(pgcnx)
