.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop_rule.l,v 1.4 1999/02/07 22:10:10 wieck Exp $
.TH "DROP RULE" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
drop rule \- removes a current rule from Postgres 
.SH SYNOPSIS
.nf
\fBdrop rule\fR rule_name
.fi
.SH DESCRIPTION
This command drops the rule named rule_name from the specified Postgres
rule system.  Postgres will immediately cease enforcing it and will purge
its definition from the system catalogs.
.SH EXAMPLE
.nf
--
--This example drops the rewrite rule example_1
--
drop rule example_1
.fi
.SH "SEE ALSO"
create_rule(l),
drop_view(l),
drop_trigger(l).
