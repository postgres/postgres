.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/doc/man/Attic/createdb.l,v 1.1.1.1 1996/08/18 22:14:22 scrappy Exp $
.TH "CREATE DATABASE" SQL 11/05/95 Postgres95 Postgres95
.SH NAME
create database \(em create a new database
.SH SYNOPSIS
.nf
\fBcreate database\fP dbname
.fi
.SH DESCRIPTION
.BR "Create database"
creates a new Postgres database.  The creator becomes the administrator
of the new database.
.SH "SEE ALSO"
createdb(1),
drop database(l),
destroydb(1),
initdb(1).
.SH BUGS
This command should 
.BR NOT
be executed interactively.  The 
.IR createdb (1)
script should be used instead.
