.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop_rule.l,v 1.1 1996/11/14 10:17:07 scrappy Exp $
.TH "DROP RULE" SQL 11/05/95 Postgres95 Postgres95
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
create rule(l),
drop view(l).
.SH BUGS
Once a rule is dropped, access to historical information the rule has
written may disappear.
