.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop_user.l,v 1.1 1998/01/25 07:42:02 scrappy Exp $
.TH "DROP USER" SQL 01/26/98 PostgreSQL PostgreSQL
.SH NAME
drop user -- drop user from within a PostgreSQL instance
.SH SYNOPSIS
.nf
\fBdrop user\fR username
.fi
.SH DESCRIPTION
.BR "drop user"

statement removes the named user from a PostgreSQL instance, along with
any databases owned by the user.  It does not remove tables, views, or
triggers owned by the named user in database not owned by the user.  This
statement can be used in the place of destroyuser(1), regardless of how
the user was created. 

.SH EXAMPLES
.nf
---
--- Drop a user
---
drop user tab;
.fi

.SH "SEE ALSO"
alter_user(l), create_user(l).

