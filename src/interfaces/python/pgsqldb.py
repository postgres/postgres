# pgsqldb.py
# Written by D'Arcy J.M. Cain

# This library implements the DB-SIG API
# It includes the pg module and builds on it

from _pg import *

import string

class _cursor:
	"""For cursor object"""

	def __init__(self, conn):
		self.conn = conn
		self.cursor = None
		self.arraysize = 1
		self.description = None
		self.name = string.split(`self`)[3][:-1]

	def close(self):
		if self.conn == None: raise self.conn.error, "Cursor has been closed"
		if self.cursor == None: raise self.conn.error, "No cursor created"
		self.conn.query('CLOSE %s' % self.name)
		self.conn = None

	def __del__(self):
		if self.cursor != None and self.conn != None:
			self.conn.query('CLOSE %s' % self.name)

	
class pgsqldb:
	"""This class wraps the pg connection type in a DB-SIG API interface"""

	def __init__(self, *args, **kw):
		self.db = apply(connect, args, kw)

		# Create convience methods, in a way that is still overridable.
		for e in ('query', 'reset', 'close', 'getnotify', 'inserttable',
						'putline', 'getline', 'endcopy',
						'host', 'port', 'db', 'options',
						'tty', 'error', 'status', 'user',
						'locreate', 'getlo', 'loimport'):
			if not hasattr(self,e) and hasattr(self.db,e):
				exec 'self.%s = self.db.%s' % ( e, e )

