.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_user.l,v 1.1 1998/01/25 07:42:01 scrappy Exp $
.TH "CREATE USER" SQL 01/26/98 PostgreSQL PostgreSQL
.SH NAME
create user -- create a new user within a PostgreSQL instance
.SH SYNOPSIS
.nf
\fBcreate user <username>
		[\fBwith password\fR password]
		[\fBcreatedb\fR | \fBnocreatedb\fR]
		[\fBcreateuser\fR | \fBnocreateuser\fR]
		[\fBin group\fR group-1, ..., group-n]
		[\fBvalid until '\fRabstime\fB'\fR]
.fi
.SH DESCRIPTION
.BR "create user"
will add a new user to an instance of PostgreSQL.  The new user will be
given a usesysid of 'SELECT max(usesysid) + 1 FROM pg_user'.  This means
that a PostgreSQL user's usesysid will not correspond to their operating
system(OS) user id.  The exception to this rule is the 'postgres' user,
whose OS user id is used as the usesysid during the initdb process.  If
you still want the OS user id and the usesysid to match for any given
user, then use the createuser(1) script provided with the PostgreSQL
distribution. 

The 'with password' clause sets the user's password within the pg_user
relation.  For this reason, pg_user is no longer accessible to the
'public' group.  Please note that when initdb(1) is executed for an
instance of PostgreSQL that the postgres user's password is initially set
to NULL.  When a user's password in the pg_user relation is NULL, then
user authentication proceeds as it historically has (HBA, PG_PASSWORD,
etc).  However, if a password is set for a user, then a new authentication
system supplants any other configured for the PostgreSQL instance, and the
password stored in the pg_user relation is used for authentication.  For
more details on how this authentication system functions see pg_crypt(3). 
If the 'with password' clause is omitted, then the user's password is set
to the empty string with equates to a NULL value in the authentication
system mentioned above. 

The createdb/nocreatedb clause defines a user's ability to create
databases.  If createdb is specified, then the user being defined will be
allowed to create his/her own databases.  Using nocreatedb will deny a
user the ability to create databases.  If this clause is omitted, then
nocreatedb is used by default. 

The createuser/nocreateuser clause allows/prevents a user from creating
new users in an instance of PostgreSQL.  Omitting this clause will set the
user's value of this attribute to be nocreateuser. 

At the current time the 'in group' clause is non-functional.  The intent
is to use this clause to affect the groups a user is a member of (as
defined in the pg_group relation). 

Finally, the 'valid until' clause sets an absolute time after which the
user's PostgreSQL login is no longer valid.  Please note that if a user
does not have a password defined in the pg_user relation, then the valid
until date will not be checked during user authentication.  If this clause
is omitted, then a NULL value is stored in pg_user for this attribute, and
the login will be valid for all time. 

.SH EXAMPLES
.nf
---
--- Create a user with no password
---
create user tab;
.fi
.nf
---
--- Create a user with a password
---
create user tab with password jw8s0F4;
.fi
.nf
---
--- Create a user with a password, whose account is valid thru 2001
--- Note that after one second has ticked in 2002, the account is not
--- valid
---
create user tab with password jw8s0F4 valid until 'Jan 1 2002';
.fi
.nf
---
--- Create an account where the user can create databases.
---
create user tab with password jw8s0F4 createdb;
.fi
.SH "SEE ALSO"
pg_crypt(3), alter_user(l), drop_user(l).
