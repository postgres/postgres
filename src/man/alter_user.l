.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/alter_user.l,v 1.2 1998/03/06 18:02:49 momjian Exp $
.TH "ALTER USER" SQL 01/26/98 PostgreSQL PostgreSQL
.SH NAME
alter user -- alter user account information within a PostgreSQL instance
.SH SYNOPSIS
.nf
\fBalter user\fR username
		[\fBwith password\fR password]
		[\fBcreatedb\fR | \fBnocreatedb\fR]
		[\fBcreateuser\fR | \fBnocreateuser\fR]
		[\fBin group\fR group-1, ..., group-n]
		[\fBvalid until '\fRabstime\fB'\fR]
.fi
.SH DESCRIPTION
.BR "alter user"
is used to change the attributes of a user's PostgreSQL account.  For a
detailed description of each of the clause in the alter user statement,
please see the create_user(l) manual page.  Please note that it is not
possible to alter a user's usesysid via the alter user statement.  Also,
it is only possible for the postgres user or any user with read and modify
permissions on pg_shadow to alter user passwords. 

If any of the clauses of the alter user statement are omitted, the
corresponding value in the pg_shadow relation is left unchanged. 

This statement can be used to modify users created with createuser(1).

.SH EXAMPLES
.nf
---
--- Change a user password
---
alter user tab with password hu8jmn3;
.fi
.nf
---
--- Change a user's valid until date
---
alter user tab valid until 'Jan 31 2030';
.fi
.nf
---
--- Give a user the ability to create other users.
---
alter user tab createuser;
.fi
.SH "SEE ALSO"
create_user(l), drop_user(l).
