# func.py - demonstrate the use of SQL functions
# inspired from the PostgreSQL tutorial 
# adapted to Python 1995 by Pascal ANDRE

print """
__________________________________________________________________
MODULE FUNC.PY : SQL FUNCTION DEFINITION TUTORIAL

This module is designed for being imported from python prompt

In order to run the samples included here, first create a connection
using :                        cnx = advanced.DB(...)

The "..." should be replaced with whatever arguments you need to open an
existing database.  Usually all you need is the name of the database and,
in fact, if it is the same as your login name, you can leave it empty.

then start the demo with:      func.demo(cnx)
__________________________________________________________________
""" 

from pg import DB
import sys

# waits for a key
def wait_key():
	print "Press <enter>"
	sys.stdin.read(1)

# basic functions declaration
def base_func(pgcnx):
	print "-----------------------------"
	print "-- Creating SQL Functions on Base Types"
	print "--  a CREATE FUNCTION statement lets you create a new "
	print "--  function that can be used in expressions (in SELECT, "
	print "--  INSERT, etc.). We will start with functions that "
	print "--  return values of base types."
	print "-----------------------------"
	print
	print "--"
	print "-- let's create a simple SQL function that takes no arguments"
	print "-- and returns 1"
	print
	print "CREATE FUNCTION one() RETURNS int4"
	print "   AS 'SELECT 1 as ONE' LANGUAGE 'sql'"
	pgcnx.query("""CREATE FUNCTION one() RETURNS int4
        AS 'SELECT 1 as ONE' LANGUAGE 'sql'""")
	wait_key()
	print
	print "--"
	print "-- functions can be used in any expressions (eg. in the target"
	print "-- list or qualifications)"
	print
	print "SELECT one() AS answer"
	print pgcnx.query("SELECT one() AS answer")
	print
	print "--"
	print "-- here's how you create a function that takes arguments. The"
	print "-- following function returns the sum of its two arguments:"
	print
	print "CREATE FUNCTION add_em(int4, int4) RETURNS int4"
	print "   AS 'SELECT $1 + $2' LANGUAGE 'sql'"
	pgcnx.query("""CREATE FUNCTION add_em(int4, int4) RETURNS int4
        AS 'SELECT $1 + $2' LANGUAGE 'sql'""")
	print
	print "SELECT add_em(1, 2) AS answer"
	print pgcnx.query("SELECT add_em(1, 2) AS answer")

# functions on composite types
def comp_func(pgcnx):
	print "-----------------------------"
	print "-- Creating SQL Functions on Composite Types"
	print "--  it is also possible to create functions that return"
	print "--  values of composite types."
	print "-----------------------------"
	print
	print "-- before we create more sophisticated functions, let's "
	print "-- populate an EMP table"
	print
	print "CREATE TABLE EMP ("
	print "    name    text,"
	print "    salary  int4,"
	print "    age     int4,"
	print "    dept    varchar(16)"
	print ")"
	pgcnx.query("""CREATE TABLE EMP (
        name        text,
        salary      int4,
        age         int4,
        dept        varchar(16))""")
	print
	print "INSERT INTO EMP VALUES ('Sam', 1200, 16, 'toy')"
	print "INSERT INTO EMP VALUES ('Claire', 5000, 32, 'shoe')"
	print "INSERT INTO EMP VALUES ('Andy', -1000, 2, 'candy')"
	print "INSERT INTO EMP VALUES ('Bill', 4200, 36, 'shoe')"
	print "INSERT INTO EMP VALUES ('Ginger', 4800, 30, 'candy')"
	pgcnx.query("INSERT INTO EMP VALUES ('Sam', 1200, 16, 'toy')")
	pgcnx.query("INSERT INTO EMP VALUES ('Claire', 5000, 32, 'shoe')")
	pgcnx.query("INSERT INTO EMP VALUES ('Andy', -1000, 2, 'candy')")
	pgcnx.query("INSERT INTO EMP VALUES ('Bill', 4200, 36, 'shoe')")
	pgcnx.query("INSERT INTO EMP VALUES ('Ginger', 4800, 30, 'candy')")
	wait_key()
	print
	print "-- the argument of a function can also be a tuple. For "
	print "-- instance, double_salary takes a tuple of the EMP table"
	print
	print "CREATE FUNCTION double_salary(EMP) RETURNS int4"
	print "   AS 'SELECT $1.salary * 2 AS salary' LANGUAGE 'sql'"
	pgcnx.query("""CREATE FUNCTION double_salary(EMP) RETURNS int4
        AS 'SELECT $1.salary * 2 AS salary' LANGUAGE 'sql'""")
	print
	print "SELECT name, double_salary(EMP) AS dream"
	print "FROM EMP"
	print "WHERE EMP.dept = 'toy'"
	print pgcnx.query("""SELECT name, double_salary(EMP) AS dream
        FROM EMP WHERE EMP.dept = 'toy'""")
	print
	print "-- the return value of a function can also be a tuple. However,"
	print "-- make sure that the expressions in the target list is in the "
	print "-- same order as the columns of EMP."
	print
	print "CREATE FUNCTION new_emp() RETURNS EMP"
	print "   AS 'SELECT \'None\'::text AS name,"
	print "              1000 AS salary,"
	print "              25 AS age,"
	print "              \'none\'::varchar(16) AS dept'"
	print "   LANGUAGE 'sql'"
	pgcnx.query("""CREATE FUNCTION new_emp() RETURNS EMP
        AS 'SELECT \\\'None\\\'::text AS name,
            1000 AS salary,
            25 AS age,
            \\\'none\\\'::varchar(16) AS dept'
        LANGUAGE 'sql'""")
	wait_key()
	print
	print "-- you can then project a column out of resulting the tuple by"
	print "-- using the \"function notation\" for projection columns. "
	print "-- (ie. bar(foo) is equivalent to foo.bar) Note that we don't"
	print "-- support new_emp().name at this moment."
	print
	print "SELECT name(new_emp()) AS nobody"
	print pgcnx.query("SELECT name(new_emp()) AS nobody")
	print
	print "-- let's try one more function that returns tuples"
	print "CREATE FUNCTION high_pay() RETURNS setof EMP"
	print "   AS 'SELECT * FROM EMP where salary > 1500'"
	print "   LANGUAGE 'sql'"
	pgcnx.query("""CREATE FUNCTION high_pay() RETURNS setof EMP
        AS 'SELECT * FROM EMP where salary > 1500'
        LANGUAGE 'sql'""")
	print
	print "SELECT name(high_pay()) AS overpaid"
	print pgcnx.query("SELECT name(high_pay()) AS overpaid")

# function with multiple SQL commands
def mult_func(pgcnx):
	print "-----------------------------"
	print "-- Creating SQL Functions with multiple SQL statements"
	print "--  you can also create functions that do more than just a"
	print "--  SELECT."
	print "-----------------------------"
	print
	print "-- you may have noticed that Andy has a negative salary. We'll"
	print "-- create a function that removes employees with negative "
	print "-- salaries."
	print
	print "SELECT * FROM EMP"
	print pgcnx.query("SELECT * FROM EMP")
	print
	print "CREATE FUNCTION clean_EMP () RETURNS int4"
	print "   AS 'DELETE FROM EMP WHERE EMP.salary <= 0"
	print "       SELECT 1 AS ignore_this'"
	print "   LANGUAGE 'sql'"
	pgcnx.query("CREATE FUNCTION clean_EMP () RETURNS int4 AS 'DELETE FROM EMP WHERE EMP.salary <= 0; SELECT 1 AS ignore_this' LANGUAGE 'sql'")
	print
	print "SELECT clean_EMP()"
	print pgcnx.query("SELECT clean_EMP()")
	print
	print "SELECT * FROM EMP"
	print pgcnx.query("SELECT * FROM EMP")

# base cleanup
def demo_cleanup(pgcnx):
	print "-- remove functions that were created in this file"
	print
	print "DROP FUNCTION clean_EMP()"
	print "DROP FUNCTION high_pay()"
	print "DROP FUNCTION new_emp()"
	print "DROP FUNCTION add_em(int4, int4)"
	print "DROP FUNCTION one()"
	print
	print "DROP TABLE EMP"
	pgcnx.query("DROP FUNCTION clean_EMP()")
	pgcnx.query("DROP FUNCTION high_pay()")
	pgcnx.query("DROP FUNCTION new_emp()")
	pgcnx.query("DROP FUNCTION add_em(int4, int4)")
	pgcnx.query("DROP FUNCTION one()")
	pgcnx.query("DROP TABLE EMP")

# main demo function
def demo(pgcnx):
	base_func(pgcnx)
	comp_func(pgcnx)
	mult_func(pgcnx)
	demo_cleanup(pgcnx)
