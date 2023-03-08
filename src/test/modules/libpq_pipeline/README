Test programs and libraries for libpq

If you have Test::Differences installed, any differences in the trace files
are displayed in a format that's easier to read than the standard format.
=====================================

This module was developed to test libpq's "pipeline" mode, but it can
be used for any libpq test that requires specialized C code.

"make check" will run all the tests in the module against a temporary
server installation.

You can manually run a specific test by running:

    ./libpq_pipeline <name of test> [ <connection string> ]

This will not start a new server, but rather connect to the server
specified by the connection string, or your default server if you
leave that out.  To discover the available test names, run:

    ./libpq_pipeline tests

To add a new test to this module, you need to edit libpq_pipeline.c.
Add a function to perform the test, and arrange for main() to call it
when the name of your new test is passed to the program.  Don't forget
to add the name of your test to the print_test_list() function, else
the TAP test won't run it.

If the order in which Postgres protocol messages are sent is deterministic
in your test, you should arrange for the message sequence to be verified
by the TAP test.  First generate a reference trace file, using a command
like:

   ./libpq_pipeline -t traces/mynewtest.trace mynewtest

Then add your test's name to the list in the $cmptrace definition in the
t/001_libpq_pipeline.pl file.  Run "make check" a few times to verify
that the trace output actually is stable.
