.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/vacuum.l,v 1.9 1998/03/25 01:54:53 momjian Exp $
.TH VACUUM SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
vacuum - vacuum a database
.SH SYNOPSIS
.nf
\fBvacuum [verbose] [analyze] [\fPtable\fB]\fP
\fBvacuum [verbose]  analyze  [\fPtable [(column,...)]\fB]\fP
.fi
.SH DESCRIPTION
.BR Vacuum
is the Postgres vacuum cleaner.  It opens every class in the database,
cleans out records from rolled back transactions, and updates statistics in the
system catalogs.  The statistics maintained include the number of
tuples and number of pages stored in all classes.  Running
.BR vacuum
periodically will increase Postgres's speed in processing user queries.
.PP
\fBverbose\fP prints a detailed vacuum activity report for each table.
.PP
\fBanalyze\fP also updates column statistics used by the optimizer to
determine the most efficient way to execute a query.
The statistics represent the disbursion of the data in each column.
This information is valuable when several execution paths are possible.
.PP
The open database is the one that is vacuumed.  
.PP
We recommend that production databases be vacuumed nightly, in order
to keep statistics relatively current.  The
.BR vacuum
query may be executed at any time, however.  In particular, after
copying a large class into Postgres or deleting a large number of
records, it may be a good idea to issue a
.BR vacuum
query.  This will update the system catalogs with the results of all
recent changes, and allow the Postgres query optimizer to make better
choices in planning user queries.
.PP
If the server crashes during a vacuum command, chances are it will leave
a lock file hanging around.  Attempts to re-run the vacuum command
result in an error message about the creation of a lock file.  If you
are sure vacuum is not running, remove the pg_vlock file in your
database directory(i.e. data/base/dbname/pg_vlock).
