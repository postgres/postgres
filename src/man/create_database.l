.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_database.l,v 1.4 1997/11/07 06:33:55 thomas Exp $
.TH "CREATE DATABASE" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
create database \(em create a new database
.SH SYNOPSIS
.nf
\fBcreate database\fP dbname [\fBwith location = '\fPdbpath\fB'\fP]
.fi
.SH DESCRIPTION
.BR "create database"
creates a new Postgres database.  The creator becomes the administrator
of the new database. An alternate location can be specified as either an
environment variable known to the backend server (e.g. 'PGDATA2') or as
an absolute path name (e.g. '/home/postgres/data'). In either case, the
location must be pre-configured by
.BR initarea

.SH "SEE ALSO"
createdb(1),
drop database(l),
destroydb(1),
initarea(1),
initdb(1).
