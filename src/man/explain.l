.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/explain.l,v 1.2 1996/12/29 03:55:36 momjian Exp $
.TH EXPLAIN SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
explain \(em explains statement execution details
.SH SYNOPSIS
.nf
\fBexplain [with\fP \fB{cost|full}]\fR query
.fi
.SH DESCRIPTION
This command outputs details about the supplied query.  The default
output is the computed query cost.  \f2full\f1 displays a full query plan
and cost.
