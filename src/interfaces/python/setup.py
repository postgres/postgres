#!/usr/bin/env python

include_dirs=['/usr/include/pgsql']
library_dirs=['usr/lib/pgsql']
optional_libs=['pq']

# Setup script for the PyGreSQL version 3
# created 2000/04 Mark Alexander <mwa@gate.net>
# tweaked 2000/05 Jeremy Hylton <jeremy@cnri.reston.va.us>

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

setup (name = "PyGreSQL",
    version = "3.0",
    description = "Python PostgreSQL Interfaces",
    author = "D'Arcy J. M. Cain",
    author_email = "darcy@druid.net",
    url = "http://www.druid.net/pygresql/",
    licence = "Python",

	py_modules = ['pg', 'pgdb'],
    ext_modules = [ ('_pgmodule', {
        'sources': ['pgmodule.c'],
        'include_dirs': include_dirs,
        'library_dirs': library_dirs,
        'libraries': optional_libs
        }
    )]
)

