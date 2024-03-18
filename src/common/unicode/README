This directory contains tools to download new Unicode data files and
generate static tables. These tables are used to normalize or
determine various properties of Unicode data.

The generated header files are copied to src/include/common/, and
included in the source tree, so these tools are not normally required
to build PostgreSQL.

Update Unicode Version
----------------------

Edit src/Makefile.global.in and src/common/unicode/meson.build
to update the UNICODE_VERSION.

Then, generate the new header files with:

    make update-unicode

or if using meson:

    ninja update-unicode

from the top level of the source tree. Examine the result to make sure
the changes look reasonable (that is, that the diff size and scope is
comparable to the Unicode changes since the last update), and then
commit it.

Tests
-----

Normalization tests:

The Unicode consortium publishes a comprehensive test suite for the
normalization algorithm, in a file called NormalizationTest.txt. This
directory also contains a perl script and some C code, to run our
normalization code with all the test strings in NormalizationTest.txt.
To download NormalizationTest.txt and run the tests:

    make normalization-check

This is also run as part of the update-unicode target.

Category, Property and Case tests:

The files case_test.c and category_test.c test Unicode categories,
properties, and case mapping by exhaustively comparing results with
ICU. For these tests to be effective, the version of the Unicode data
files must be similar to the version of Unicode on which ICU is
based. Mismatched Unicode versions will cause the tests to skip over
codepoints that are assigned in one version and not the other, and may
falsely report failures. This test is run as a part of the
update-unicode target.
