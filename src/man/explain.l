.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/explain.l,v 1.3 1996/12/29 19:31:16 momjian Exp $
.TH EXPLAIN SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
explain \(em explains statement execution details
.SH SYNOPSIS
.nf
\fBexplain [with\fP \fB{cost|plan|full}]\fR query
.fi
.SH DESCRIPTION
This command outputs details about the supplied query.  The default
output is the computed query cost.  \f2plan\f1 displays the full query
plan. \f2full\f1 display both query plan and query cost.
.PP
The query cost and plan can be affected by running vacuum.

