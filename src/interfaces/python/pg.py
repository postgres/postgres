# pgutil.py
# Written by D'Arcy J.M. Cain

# This library implements some basic database management stuff
# It includes the pg module and builds on it

from _pg import *
import string, re, sys

# utility function
# We expect int, seq, decimal, text or date (more later)
def _quote(d, t):
	if d == None:
		return "NULL"

	if t in ['int', 'seq']:
		if d == "": return "NULL"
		return "%d" % int(d)

	if t == 'decimal':
		if d == "": return "NULL"
		return "%f" % float(d)

	if t == 'money':
		if d == "": return "NULL"
		return "'%.2f'" % float(d)

	if t == 'bool':
		# Can't run upper() on these
		if d in (0, 1): return ('f', 't')[d]

		if string.upper(d) in ['T', 'TRUE', 'Y', 'YES', '1', 'ON']:
			return "'t'"
		else:
			return "'f'"

	if t == 'date' and d == '': return "NULL"
	if t in ('inet', 'cidr') and d == '': return "NULL"

	return "'%s'" % string.strip(re.sub("'", "''", \
							 re.sub("\\\\", "\\\\\\\\", "%s" %d)))

class DB:
	"""This class wraps the pg connection type"""

	def __init__(self, *args, **kw):
		self.db = apply(connect, args, kw)

		# Create convience methods, in a way that is still overridable.
		for e in ( 'query', 'reset', 'close', 'getnotify', 'inserttable',
					'putline', 'getline', 'endcopy',
					'host', 'port', 'db', 'options', 
					'tty', 'error', 'status', 'user',
					'locreate', 'getlo', 'loimport' ):
			if not hasattr(self,e) and hasattr(self.db,e):
				exec 'self.%s = self.db.%s' % ( e, e )

		self.__attnames__ = {}
		self.__pkeys__ = {}
		self.debug = None	# For debugging scripts, set to output format
							# that takes a single string arg.  For example
							# in a CGI set to "%s<BR>"

		# Get all the primary keys at once
		for rel, att in self.db.query("""SELECT
							pg_class.relname, pg_attribute.attname
						FROM pg_class, pg_attribute, pg_index
						WHERE pg_class.oid = pg_attribute.attrelid AND
							pg_class.oid = pg_index.indrelid AND
							pg_index.indkey[0] = pg_attribute.attnum AND 
							pg_index.indisprimary = 't'""").getresult():
			self.__pkeys__[rel] = att

	# wrap query for debugging
	def query(self, qstr):
		if self.debug != None:
			print self.debug % qstr
		return self.db.query(qstr)

	# If third arg supplied set primary key to it
	def pkey(self, cl, newpkey = None):
		if newpkey:
			self.__pkeys__[cl] = newpkey

		# will raise an exception if primary key doesn't exist
		return self.__pkeys__[cl]

	def get_databases(self):
		l = []
		for n in self.db.query("SELECT datname FROM pg_database").getresult():
			l.append(n[0])
		return l

	def get_tables(self):
		l = []
		for n in self.db.query("""SELECT relname FROM pg_class
						WHERE relkind = 'r' AND
							relname !~ '^Inv' AND
							relname !~ '^pg_'""").getresult():
			l.append(n[0])
		return l

	def get_attnames(self, cl):
		# May as well cache them
		if self.__attnames__.has_key(cl):
			return self.__attnames__[cl]

		query = """SELECT pg_attribute.attname, pg_type.typname
					FROM pg_class, pg_attribute, pg_type
					WHERE pg_class.relname = '%s' AND
						pg_attribute.attnum > 0 AND
						pg_attribute.attrelid = pg_class.oid AND
						pg_attribute.atttypid = pg_type.oid"""

		l = {}
		for attname, typname in self.db.query(query % cl).getresult():
			if re.match("^int", typname):
				l[attname] = 'int'
			elif re.match("^oid", typname):
				l[attname] = 'int'
			elif re.match("^text", typname):
				l[attname] = 'text'
			elif re.match("^char", typname):
				l[attname] = 'text'
			elif re.match("^name", typname):
				l[attname] = 'text'
			elif re.match("^abstime", typname):
				l[attname] = 'date'
			elif re.match("^date", typname):
				l[attname] = 'date'
			elif re.match("^timestamp", typname):
				l[attname] = 'date'
			elif re.match("^bool", typname):
				l[attname] = 'bool'
			elif re.match("^float", typname):
				l[attname] = 'decimal'
			elif re.match("^money", typname):
				l[attname] = 'money'
			else:
				l[attname] = 'text'

		self.__attnames__[cl] = l
		return self.__attnames__[cl]

	# return a tuple from a database
	def get(self, cl, arg, keyname = None, view = 0):
		if cl[-1] == '*':			# need parent table name
			xcl = cl[:-1]
		else:
			xcl = cl

		if keyname == None:			# use the primary key by default
			keyname = self.__pkeys__[xcl]

		fnames = self.get_attnames(xcl)

		if type(arg) == type({}):
			# To allow users to work with multiple tables we munge the
			# name when the key is "oid"
			if keyname == 'oid': k = arg['oid_%s' % xcl]
			else: k = arg[keyname]
		else:
			k = arg
			arg = {}

		# We want the oid for later updates if that isn't the key
		if keyname == 'oid':
			q = "SELECT * FROM %s WHERE oid = %s" % (cl, k)
		elif view:
			q = "SELECT * FROM %s WHERE %s = %s" % \
				(cl, keyname, _quote(k, fnames[keyname]))
		else:
			q = "SELECT oid AS oid_%s, %s FROM %s WHERE %s = %s" % \
				(xcl, string.join(fnames.keys(), ','),\
					cl, keyname, _quote(k, fnames[keyname]))

		if self.debug != None: print self.debug % q
		res = self.db.query(q).dictresult()
		if res == []:
			raise error, \
				"No such record in %s where %s is %s" % \
								(cl, keyname, _quote(k, fnames[keyname]))
			return None

		for k in res[0].keys():
			arg[k] = res[0][k]

		return arg

	# Inserts a new tuple into a table
	# We currently don't support insert into views although PostgreSQL does
	def insert(self, cl, a):
		fnames = self.get_attnames(cl)
		l = []
		n = []
		for f in fnames.keys():
			if a.has_key(f):
				l.append(_quote(a[f], fnames[f]))
				n.append(f)

		try:
			q = "INSERT INTO %s (%s) VALUES (%s)" % \
				(cl, string.join(n, ','), string.join(l, ','))
			if self.debug != None: print self.debug % q
			a['oid_%s' % cl] = self.db.query(q)
		except:
			raise error, "Error inserting into %s: %s" % (cl, sys.exc_value)

		# reload the dictionary to catch things modified by engine
		# note that get() changes 'oid' below to oid_table
		# if no read perms (it can and does happen) return None
		try: return self.get(cl, a, 'oid')
		except: return None

	# Update always works on the oid which get returns if available
	# otherwise use the primary key.  Fail if neither.
	def update(self, cl, a):
		foid = 'oid_%s' % cl
		if a.has_key(foid):
			where = "oid = %s" % a[foid]
		elif self.__pkeys__.has_key(cl) and a.has_key(self.__pkeys__[cl]):
			where = "%s = '%s'" % (self.__pkeys__[cl], a[self.__pkeys__[cl]])
		else:
			raise error, "Update needs primary key or oid as %s" % foid

		v = []
		k = 0
		fnames = self.get_attnames(cl)

		for ff in fnames.keys():
			if a.has_key(ff):
				v.append("%s = %s" % (ff, _quote(a[ff], fnames[ff])))

		if v == []:
			return None

		try:
			q = "UPDATE %s SET %s WHERE %s" % \
							(cl, string.join(v, ','), where)
			if self.debug != None: print self.debug % q
			self.db.query(q)
		except:
			raise error, "Can't update %s: %s" % (cl, sys.exc_value)

		# reload the dictionary to catch things modified by engine
		if a.has_key(foid):
			return self.get(cl, a, 'oid')
		else:
			return self.get(cl, a)

	# At some point we will need a way to get defaults from a table
	def clear(self, cl, a = {}):
		fnames = self.get_attnames(cl)
		for ff in fnames.keys():
			if fnames[ff] in ['int', 'decimal', 'seq', 'money']:
				a[ff] = 0
			else:
				a[ff] = ""

		a['oid'] = 0
		return a

	# Like update, delete works on the oid
	# one day we will be testing that the record to be deleted
	# isn't referenced somewhere (or else PostgreSQL will)
	def delete(self, cl, a):
		try:
			q = "DELETE FROM %s WHERE oid = %s" % (cl, a['oid_%s' % cl])
			if self.debug != None: print self.debug % q
			self.db.query(q)
		except:
			raise error, "Can't delete %s: %s" % (cl, sys.exc_value)

		return None

