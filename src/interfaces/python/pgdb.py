""" pgdb - DB-SIG compliant module for PygreSQL.

	(c) 1999, Pascal Andre <andre@via.ecp.fr>.
	See package documentation for further information on copyright.

	Even though this file is distributed with a release version of
	PyGreSQL, this is beta software. Inline documentation is sparse.
	See DB-SIG 2.0 specification for usage information.

		basic usage:

		pgdb.connect(connect_string) -> connection
			connect_string = 'host:database:user:password:opt:tty'
			All parts are optional. You may also pass host through
			password as keyword arguments. To pass a port, pass it in
			the host keyword parameter:
				pgdb.connect(host='localhost:5432')

		connection.cursor() -> cursor

		connection.commit()

		connection.close()

		connection.rollback()

		cursor.execute(query[, params])
			execute a query, binding params (a dictionary) if it is
			passed. The binding syntax is the same as the % operator
			for dictionaries, and no quoting is done.

		cursor.executemany(query, list of params)
			execute a query many times, binding each param dictionary
			from the list.

		cursor.fetchone() -> [value, value, ...]

		cursor.fetchall() -> [[value, value, ...], ...]

		cursor.fetchmany([size]) -> [[value, value, ...], ...]
			returns size or cursor.arraysize number of rows from result
			set. Default cursor.arraysize is 1.

		cursor.description -> [(column_name, type_name, display_size,
			internal_size, precision, scale, null_ok), ...]

			Note that precision, scale and null_ok are not implemented.

		cursor.rowcount
			number of rows available in the result set. Available after
			a call to execute.

		cursor.close()

"""

import _pg
import string
import exceptions
import types
import DateTime
import time

### module constants

# compliant with DB SIG 2.0
apilevel = '2.0'

# module may be shared, but not connections
threadsafety = 1

# this module use extended python format codes
paramstyle = 'pyformat'

### exception hierarchy

class Warning(StandardError):
	pass

class Error(StandardError):
	pass

class InterfaceError(Error):
	pass

class DatabaseError(Error):
	pass

class DataError(DatabaseError):
	pass

class OperationalError(DatabaseError):
	pass

class IntegrityError(DatabaseError):
	pass

class InternalError(DatabaseError):
	pass

class ProgrammingError(DatabaseError):
	pass

class NotSupportedError(DatabaseError):
	pass

### internal type handling class
class pgdbTypeCache:

	def __init__(self, cnx):
		self.__source = cnx.source()
		self.__type_cache = {}

	def typecast(self, typ, value):
		# for NULL values, no typecast is necessary
		if value == None:
			return value

		if typ == STRING:
			pass
		elif typ == BINARY:
			pass
		elif typ == BOOL:
			value = (value[:1] in ['t','T'])
		elif typ == INTEGER:
			value = int(value)
		elif typ == LONG:
			value = long(value)
		elif typ == FLOAT:
			value = float(value)
		elif typ == MONEY:
			value = string.replace(value, "$", "")
			value = string.replace(value, ",", "")
			value = float(value)
		elif typ == DATETIME:
			# format may differ ... we'll give string
			pass
		elif typ == ROWID:
			value = long(value)
		return value

	def getdescr(self, oid):
		try:
			return self.__type_cache[oid]
		except:
			self.__source.execute(
				"SELECT typname, typprtlen, typlen "
				"FROM pg_type WHERE oid = %s" % oid
			)
			res = self.__source.fetch(1)[0]
			# column name is omitted from the return value. It will
			# have to be prepended by the caller.
			res = (
				res[0],
				string.atoi(res[1]), string.atoi(res[2]),
				None, None, None
			)
			self.__type_cache[oid] = res
			return res

### cursor object

class pgdbCursor:

	def __init__(self, src, cache):
		self.__cache = cache
		self.__source = src
		self.description = None
		self.rowcount = -1
		self.arraysize = 5

	def close(self):
		self.__source.close()
		self.description = None
		self.rowcount = -1

	def execute(self, operation, params = None):
		if type(params) == types.TupleType or type(params) == types.ListType:
			self.executemany(operation, params)
		else:
			self.executemany(operation, (params,))

	def executemany(self, operation, param_seq):
		self.description = None
		self.rowcount = -1

		# first try to execute all queries
		totrows = 0
		sql = "INIT"
		try:
			for params in param_seq:
				if params != None:
					sql = operation % params
				else:
					sql = operation
				rows = self.__source.execute(sql)
				if rows != None: # true is __source is NOT a DQL
					totrows = totrows + rows
		except _pg.error, msg:
			raise DatabaseError, "error '%s' in '%s'" % ( msg, sql )
		except:
			raise OperationalError, "internal error in '%s'" % sql

		# then initialize result raw count and description
		if self.__source.resulttype == _pg.RESULT_DQL:
			self.rowcount = self.__source.ntuples
			d = []
			for typ in self.__source.listinfo():
				# listinfo is a sequence of
				# (index, column_name, type_oid)
				# getdescr returns all items needed for a
				# description tuple except the column_name.
				desc = typ[1:2]+self.__cache.getdescr(typ[2])
				d.append(desc)
			self.description = d
		else:
			self.rowcount = totrows
			self.description = None

	def fetchone(self):
		res = self.fetchmany(1, 0)
		try:
			return res[0]
		except:
			return None

	def fetchall(self):
		return self.fetchmany(-1, 0)

	def fetchmany(self, size = None, keep = 1):
		if size == None:
			size = self.arraysize
		if keep == 1:
			self.arraysize = size
		res = self.__source.fetch(size)
		result = []
		for r in res:
			row = []
			for i in range(len(r)):
				row.append(self.__cache.typecast(
						self.description[i][1],
						r[i]
					)
				)
			result.append(row)
		return result

	def setinputsizes(self, sizes):
		pass

	def setoutputsize(self, size, col = 0):
		pass

### connection object

class pgdbCnx:

	def __init__(self, cnx):
		self.__cnx = cnx
		self.__cache = pgdbTypeCache(cnx)
		try:
			src = self.__cnx.source()
			src.execute("BEGIN")
		except:
			raise OperationalError, "invalid connection."

	def close(self):
		self.__cnx.close()

	def commit(self):
		try:
			src = self.__cnx.source()
			src.execute("COMMIT")
			src.execute("BEGIN")
		except:
			raise OperationalError, "can't commit."

	def rollback(self):
		try:
			src = self.__cnx.source()
			src.execute("ROLLBACK")
			src.execute("BEGIN")
		except:
			raise OperationalError, "can't rollback."

	def cursor(self):
		try:
			src = self.__cnx.source()
			return pgdbCursor(src, self.__cache)
		except:
			raise pgOperationalError, "invalid connection."

### module interface

# connects to a database
def connect(dsn = None, user = None, password = None, host = None, database = None):
	# first get params from DSN
	dbport = -1
	dbhost = ""
	dbbase = ""
	dbuser = ""
	dbpasswd = ""
	dbopt = ""
	dbtty = ""
	try:
		params = string.split(dsn, ":")
		dbhost = params[0]
		dbbase = params[1]
		dbuser = params[2]
		dbpasswd = params[3]
		dbopt = params[4]
		dbtty = params[5]
	except:
		pass

	# override if necessary
	if user != None:
		dbuser = user
	if password != None:
		dbpasswd = password
	if database != None:
		dbbase = database
	if host != None:
		try:
			params = string.split(host, ":")
			dbhost = params[0]
			dbport = int(params[1])
		except:
			pass

	# empty host is localhost
	if dbhost == "":
		dbhost = None
	if dbuser == "":
		dbuser = None

	# open the connection
	cnx = _pg.connect(host = dbhost, dbname = dbbase, port = dbport,
						opt = dbopt, tty = dbtty,
						user = dbuser, passwd = dbpasswd)
	return pgdbCnx(cnx)

### types handling

# PostgreSQL is object-oriented: types are dynamic. We must thus use type names
# as internal type codes.

class pgdbType:

	def __init__(self, *values):
		self.values=  values

	def __cmp__(self, other):
		if other in self.values:
			return 0
		if other < self.values:
			return 1
		else:
			return -1

STRING = pgdbType(
	'char', 'name', 'text', 'varchar'
)

# BLOB support is pg specific
BINARY = pgdbType()
INTEGER = pgdbType('int2', 'int4', 'serial')
LONG = pgdbType('int8')
FLOAT = pgdbType('float4', 'float8', 'numeric')
BOOL = pgdbType('bool') 
MONEY = pgdbType('money')

# this may be problematic as type are quite different ... I hope it won't hurt
DATETIME = pgdbType(
	'abstime', 'reltime', 'tinterval', 'date', 'time', 'timespan', 'timestamp'
)

# OIDs are used for everything (types, tables, BLOBs, rows, ...). This may cause
# confusion, but we are unable to find out what exactly is behind the OID (at
# least not easily enough). Should this be undefined as BLOBs ?
ROWID = pgdbType(
	'oid', 'oid8'
)

# mandatory type helpers
def Date(year, month, day):
	return DateTime.DateTime(year, month, day)

def Time(hour, minute, second):
	return DateTime.TimeDelta(hour, minute, second)

def Timestamp(year, month, day, hour, minute, second):
	return DateTime.DateTime(year, month, day, hour, minute, second)

def DateFromTicks(ticks):
	return apply(Date, time.localtime(ticks)[:3])

def TimeFromTicks(ticks):
	return apply(Time, time.localtime(ticks)[3:6])

def TimestampFromTicks(ticks):
	return apply(Timestamp, time.localtime(ticks)[:6])

