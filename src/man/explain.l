.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/explain.l,v 1.4 1997/01/16 14:56:59 momjian Exp $
.TH EXPLAIN SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
explain \(em explains statement execution details
.SH SYNOPSIS
.nf
\fBexplain [verbose]\fR query
.fi
.SH DESCRIPTION
This command outputs details about the supplied query.  The default
output is the computed query cost.  \f2verbose\f1 displays the full query
plan and cost.
.PP
The query cost and plan can be affected by running vacuum.

