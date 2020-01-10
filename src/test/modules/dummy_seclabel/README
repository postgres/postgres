The dummy_seclabel module exists only to support regression testing of
the SECURITY LABEL statement.  It is not intended to be used in production.

Rationale
=========

The SECURITY LABEL statement allows the user to assign security labels to
database objects; however, security labels can only be assigned when
specifically allowed by a loadable module, so this module is provided to
allow proper regression testing.

Security label providers intended to be used in production will typically be
dependent on a platform-specific feature such as SELinux.  This module is
platform-independent, and therefore better-suited to regression testing.

Usage
=====

Here's a simple example of usage:

# postgresql.conf
shared_preload_libraries = 'dummy_seclabel'

postgres=# CREATE TABLE t (a int, b text);
CREATE TABLE
postgres=# SECURITY LABEL ON TABLE t IS 'classified';
SECURITY LABEL

The dummy_seclabel module provides only four hardcoded
labels: unclassified, classified,
secret, and top secret.
It does not allow any other strings as security labels.

These labels are not used to enforce access controls.  They are only used
to check whether the SECURITY LABEL statement works as expected,
or not.

Author
======

KaiGai Kohei <kaigai@ak.jp.nec.com>
