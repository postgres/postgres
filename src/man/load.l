.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/load.l,v 1.3 1998/01/11 22:17:43 momjian Exp $
.TH LOAD SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
load - dynamically load an object file
.SH SYNOPSIS
.nf
\fBload\fR "filename"
.fi
.SH DESCRIPTION
.BR Load
loads an object (or ".o") file into Postgres's address space.  Once a
file is loaded, all functions in that file can be accessed.  This
function is used in support of ADT's.
.PP
If a file is not loaded using the 
.BR load
command, the file will be loaded automatically the first time the
function is called by Postgres.
.BR Load
can also be used to reload an object file if it has been edited and
recompiled.  Only objects created from C language files are supported
at this time.
.SH EXAMPLE
.nf
--
--Load the file /usr/postgres/demo/circle.o
--
load "/usr/postgres/demo/circle.o"
.fi
.SH CAVEATS
Functions in loaded object files should not call functions in other
object files loaded through the
.BR load
command, meaning, for example, that all functions in file A should
call each other, functions in the standard or math libraries, or in
Postgres itself.  They should not call functions defined in a different
loaded file B.  This is because if B is reloaded, the Postgres loader is
not \*(lqsmart\*(rq enough to relocate the calls from the functions in A into
the new address space of B.  If B is not reloaded, however, there will
not be a problem.
.PP
On DECstations, you must use 
.IR /bin/cc
with the \*(lq-G 0\*(rq option when compiling object files to be
loaded.
.PP
Note that if you are porting Postgres to a new platform, the
.BR load
command will have to work in order to support ADTs.
