#!/usr/bin/env python

# Setup script for the PyGreSQL version 3
# created 2000/04 Mark Alexander <mwa@gate.net>
# tweaked 2000/05 Jeremy Hylton <jeremy@cnri.reston.va.us>
# win32 support 2001/01 Gerhard Haering <gerhard@bigfoot.de>

# requires distutils; standard in Python 1.6, otherwise download from
# http://www.python.org/sigs/distutils-sig/download.html

# You may have to change the first 3 variables (include_dirs,
# library_dirs, optional_libs) to match your postgres distribution.

# Now, you can:
#   python setup.py build   # to build the module
#   python setup.py install # to install it

# See http://www.python.org/sigs/distutils-sig/doc/ for more information
# on using distutils to install Python programs.

from distutils.core import setup
from distutils.extension import Extension
import sys

if sys.platform == "win32":
	# If you want to build from source; you must have built a win32 native libpq	# before and copied libpq.dll into the PyGreSQL root directory.
	win_pg_build_root = 'd:/dev/pg/postgresql-7.0.2/'
	include_dirs=[ win_pg_build_root + 'src/include', win_pg_build_root + '/src/include/libpq', win_pg_build_root + 'src', win_pg_build_root + 'src/interfaces/libpq' ]
	library_dirs=[ win_pg_build_root + 'src/interfaces/libpq/Release' ]
	optional_libs=[ 'libpqdll', 'wsock32', 'advapi32' ]
	data_files = [ 'libpq.dll' ]
else:
	include_dirs=['/usr/include/pgsql']
	library_dirs=['usr/lib/pgsql']
	optional_libs=['pq']
	data_files = []

setup (name = "PyGreSQL",
	version = "3.1",
	description = "Python PostgreSQL Interfaces",
	author = "D'Arcy J. M. Cain",
	author_email = "darcy@druid.net",
	url = "http://www.druid.net/pygresql/",
	licence = "Python",

	py_modules = ['pg', 'pgdb'],
	ext_modules = [ Extension(
		name='_pg',
		sources = ['pgmodule.c'],
		include_dirs = include_dirs,
		library_dirs = library_dirs,
		libraries = optional_libs
	)],
	data_files = data_files
)

