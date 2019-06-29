Test extensions and libraries
=============================

src/test/modules contains PostgreSQL extensions that are primarily or entirely
intended for testing PostgreSQL and/or to serve as example code. The extensions
here aren't intended to be installed in a production server and aren't suitable
for "real work".

Furthermore, while you can do "make install" and "make installcheck" in
this directory or its children, it is NOT ADVISABLE to do so with a server
containing valuable data.  Some of these tests may have undesirable
side-effects on roles or other global objects within the tested server.
"make installcheck-world" at the top level does not recurse into this
directory.

Most extensions have their own pg_regress tests or isolationtester specs. Some
are also used by tests elsewhere in the tree.

If you're adding new hooks or other functionality exposed as C-level API this
is where to add the tests for it.
