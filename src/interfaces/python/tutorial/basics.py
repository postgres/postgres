#! /usr/bin/env python
# basics.py - basic SQL commands tutorial
# inspired from the Postgres95 tutorial 
# adapted to Python 1995 by Pascal ANDRE

print """
__________________________________________________________________
MODULE BASICS.PY : BASIC POSTGRES SQL COMMANDS TUTORIAL
    
This module is designed for being imported from python prompt
    
In order to run the samples included here, first create a connection
using :                        cnx = basics.DB(...)
  
The "..." should be replaced with whatever arguments you need to open an
existing database.  Usually all you need is the name of the database and,
in fact, if it is the same as your login name, you can leave it empty.
        
then start the demo with:      basics.demo(cnx)
__________________________________________________________________
""" 

from pg import DB
import sys

# waits for a key
def wait_key():
	print "Press <enter>"
	sys.stdin.read(1)

# table creation commands
def create_table(pgcnx):
	print "-----------------------------"
	print "-- Creating a table:"
	print "--  a CREATE TABLE is used to create base tables. POSTGRES"
	print "--  SQL has its own set of built-in types. (Note that"
	print "--  keywords are case-insensitive but identifiers are "
	print "--  case-sensitive.)"
	print "-----------------------------"
	print
	print "Sending query :"
	print "CREATE TABLE weather ("
        print "    city            varchar(80),"
        print "    temp_lo         int,"
        print "    temp_hi         int,"
        print "    prcp            float8,"
        print "    date            date"
        print ")"
        pgcnx.query("""CREATE TABLE weather (city varchar(80), temp_lo int,
            temp_hi int, prcp float8, date date)""")
	print
	print "Sending query :"
	print "CREATE TABLE cities ("
	print "    name        varchar(80),"
	print "    location    point"
	print ")"
	pgcnx.query("""CREATE TABLE cities (
        name     varchar(80),
        location point)""")

# data insertion commands
def insert_data(pgcnx):
	print "-----------------------------"
	print "-- Inserting data:"
	print "--  an INSERT statement is used to insert a new row into"
	print "--  a table. There are several ways you can specify what"
	print "--  columns the data should go to."
	print "-----------------------------"
	print
	print "-- 1. the simplest case is when the list of value correspond to"
	print "--    the order of the columns specified in CREATE TABLE."
	print
	print "Sending query :"
	print "INSERT INTO weather "
	print "   VALUES ('San Francisco', 46, 50, 0.25, '11/27/1994')"
	pgcnx.query("""INSERT INTO weather
        VALUES ('San Francisco', 46, 50, 0.25, '11/27/1994')""")
	print
	print "Sending query :"
	print "INSERT INTO cities "
	print "   VALUES ('San Francisco', '(-194.0, 53.0)')"
	pgcnx.query("""INSERT INTO cities
        VALUES ('San Francisco', '(-194.0, 53.0)')""")
	print
	wait_key()
	print "-- 2. you can also specify what column the values correspond "
	print "     to. (The columns can be specified in any order. You may "
	print "     also omit any number of columns. eg. unknown precipitation"
	print "     below)"
	print "Sending query :"
	print "INSERT INTO weather (city, temp_lo, temp_hi, prcp, date)"
	print "   VALUES ('San Francisco', 43, 57, 0.0, '11/29/1994')"
	pgcnx.query("INSERT INTO weather (date, city, temp_hi, temp_lo)" \
        "VALUES ('11/29/1994', 'Hayward', 54, 37)")

# direct selection commands
def select_data1(pgcnx):
	print "-----------------------------"
	print "-- Retrieving data:"
	print "--  a SELECT statement is used for retrieving data. The "
	print "--  basic syntax is:"
	print "--      SELECT columns FROM tables WHERE predicates"
	print "-----------------------------"
	print
	print "-- a simple one would be the query:"
	print "SELECT * FROM weather"
	print 
	print "The result is :"
	q = pgcnx.query("SELECT * FROM weather")
	print q
	print
	print "-- you may also specify expressions in the target list (the "
	print "-- 'AS column' specifies the column name of the result. It is "
	print "-- optional.)"
	print "The query :"
	print "   SELECT city, (temp_hi+temp_lo)/2 AS temp_avg, date "
	print "   FROM weather"
	print "Gives :"
	print pgcnx.query("""SELECT city, (temp_hi+temp_lo)/2
        AS temp_avg, date FROM weather""")
	print
	print "-- if you want to retrieve rows that satisfy certain condition"
	print "-- (ie. a restriction), specify the condition in WHERE. The "
	print "-- following retrieves the weather of San Francisco on rainy "
	print "-- days."
	print "SELECT *"
	print "FROM weather"
	print "WHERE city = 'San Francisco' "
	print "  and prcp > 0.0"
	print pgcnx.query("""SELECT * FROM weather WHERE city = 'San Francisco'
        AND prcp > 0.0""")
	print
	print "-- here is a more complicated one. Duplicates are removed when "
	print "-- DISTINCT is specified. ORDER BY specifies the column to sort"
	print "-- on. (Just to make sure the following won't confuse you, "
	print "-- DISTINCT and ORDER BY can be used separately.)"
	print "SELECT DISTINCT city"
	print "FROM weather"
	print "ORDER BY city;"
	print pgcnx.query("SELECT DISTINCT city FROM weather ORDER BY city")

# selection to a temporary table
def select_data2(pgcnx):
	print "-----------------------------"
	print "-- Retrieving data into other classes:"
	print "--  a SELECT ... INTO statement can be used to retrieve "
	print "--  data into another class."
	print "-----------------------------"
	print 
	print "The query :"
	print "SELECT * INTO TABLE temptab "
	print "FROM weather"
	print "WHERE city = 'San Francisco' "
	print "  and prcp > 0.0"
	pgcnx.query("""SELECT * INTO TABLE temptab FROM weather
        WHERE city = 'San Francisco' and prcp > 0.0""")
	print "Fills the table temptab, that can be listed with :"
	print "SELECT * from temptab"
	print pgcnx.query("SELECT * from temptab")

# aggregate creation commands
def create_aggregate(pgcnx):
	print "-----------------------------"
	print "-- Aggregates"
	print "-----------------------------"
	print
	print "Let's consider the query :"
	print "SELECT max(temp_lo)"
	print "FROM weather;"
	print pgcnx.query("SELECT max(temp_lo) FROM weather")
	print 
	print "-- Aggregate with GROUP BY"
	print "SELECT city, max(temp_lo)"
	print "FROM weather "
	print "GROUP BY city;"
	print pgcnx.query( """SELECT city, max(temp_lo)
        FROM weather GROUP BY city""")

# table join commands
def join_table(pgcnx):
	print "-----------------------------"
	print "-- Joining tables:"
	print "--  queries can access multiple tables at once or access"
	print "--  the same table in such a way that multiple instances"
	print "--  of the table are being processed at the same time."
	print "-----------------------------"
	print
	print "-- suppose we want to find all the records that are in the "
	print "-- temperature range of other records. W1 and W2 are aliases "
	print "--for weather."
	print
	print "SELECT W1.city, W1.temp_lo, W1.temp_hi, "
	print "    W2.city, W2.temp_lo, W2.temp_hi"
	print "FROM weather W1, weather W2"
	print "WHERE W1.temp_lo < W2.temp_lo "
	print "  and W1.temp_hi > W2.temp_hi"
	print
	print pgcnx.query("""SELECT W1.city, W1.temp_lo, W1.temp_hi,
        W2.city, W2.temp_lo, W2.temp_hi FROM weather W1, weather W2
        WHERE W1.temp_lo < W2.temp_lo and W1.temp_hi > W2.temp_hi""")
	print
	print "-- let's join two tables. The following joins the weather table"
	print "-- and the cities table."
	print
	print "SELECT city, location, prcp, date"
	print "FROM weather, cities"
	print "WHERE name = city"
	print
	print pgcnx.query("""SELECT city, location, prcp, date FROM weather, cities
        WHERE name = city""")
	print
	print "-- since the column names are all different, we don't have to "
	print "-- specify the table name. If you want to be clear, you can do "
	print "-- the following. They give identical results, of course."
	print
	print "SELECT w.city, c.location, w.prcp, w.date"
	print "FROM weather w, cities c"
	print "WHERE c.name = w.city;"
	print
	print pgcnx.query("""SELECT w.city, c.location, w.prcp, w.date
        FROM weather w, cities c WHERE c.name = w.city""")

# data updating commands
def update_data(pgcnx):
	print "-----------------------------"
	print "-- Updating data:"
	print "--  an UPDATE statement is used for updating data. "
	print "-----------------------------"
	print 
	print "-- suppose you discover the temperature readings are all off by"
	print "-- 2 degrees as of Nov 28, you may update the data as follow:"
	print
	print "UPDATE weather"
	print "  SET temp_hi = temp_hi - 2,  temp_lo = temp_lo - 2"
	print "  WHERE date > '11/28/1994'"
	print
	pgcnx.query("""UPDATE weather
        SET temp_hi = temp_hi - 2,  temp_lo = temp_lo - 2
        WHERE date > '11/28/1994'""")
	print
	print "SELECT * from weather"
	print pgcnx.query("SELECT * from weather")

# data deletion commands
def delete_data(pgcnx):
	print "-----------------------------"
	print "-- Deleting data:"
	print "--  a DELETE statement is used for deleting rows from a "
	print "--  table."
	print "-----------------------------"
	print
	print "-- suppose you are no longer interested in the weather of "
	print "-- Hayward, you can do the following to delete those rows from"
	print "-- the table"
	print
	print "DELETE FROM weather WHERE city = 'Hayward'"
	pgcnx.query("DELETE FROM weather WHERE city = 'Hayward'")
	print
	print "SELECT * from weather"
	print
	print pgcnx.query("SELECT * from weather")
	print
	print "-- you can also delete all the rows in a table by doing the "
	print "-- following. (This is different from DROP TABLE which removes "
	print "-- the table in addition to the removing the rows.)"
	print
	print "DELETE FROM weather"
	pgcnx.query("DELETE FROM weather")
	print
	print "SELECT * from weather"
	print pgcnx.query("SELECT * from weather")

# table removal commands
def remove_table(pgcnx):
	print "-----------------------------"
	print "-- Removing the tables:"
	print "--  DROP TABLE is used to remove tables. After you have"
	print "--  done this, you can no longer use those tables."
	print "-----------------------------"
	print
	print "DROP TABLE weather, cities, temptab"
	pgcnx.query("DROP TABLE weather, cities, temptab")

# main demo function
def demo(pgcnx):
	create_table(pgcnx)
	wait_key()
	insert_data(pgcnx)
	wait_key()
	select_data1(pgcnx)
	select_data2(pgcnx)
	create_aggregate(pgcnx)
	join_table(pgcnx)
	update_data(pgcnx)
	delete_data(pgcnx)
	remove_table(pgcnx)
