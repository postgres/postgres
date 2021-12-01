src/test/subscription/README

Regression tests for subscription/logical replication
=====================================================

This directory contains a test suite for subscription/logical replication.

Running the tests
=================

NOTE: You must have given the --enable-tap-tests argument to configure.
Also, to use "make installcheck", you must have built and installed
contrib/hstore in addition to the core code.

Run
    make check
or
    make installcheck
You can use "make installcheck" if you previously did "make install".
In that case, the code in the installation tree is tested.  With
"make check", a temporary installation tree is built from the current
sources and then tested.

Either way, this test initializes, starts, and stops several test Postgres
clusters.

See src/test/perl/README for more info about running these tests.
