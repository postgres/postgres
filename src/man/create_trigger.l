.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_trigger.l,v 1.4 1998/06/23 17:52:33 momjian Exp $
.TH "CREATE TRIGGER" SQL 09/25/97 PostgreSQL
.SH NAME
create trigger - create a new trigger
.SH SYNOPSIS
.nf
\fBcreate trigger\fR trigname \fB{before|after}\fP 
	\fB{Event1 [OR Event2 [OR Event3]]}\fB
	\fBon\fR relname \fBfor each {row|statement}\fR
	\fBexecute procedure\fR funcname \fB(\fR arguments \fB)\fR

where \fBEventX\fR is one of INSERT, DELETE, UPDATE
.fi
.SH DESCRIPTION
.BR "Create Trigger"
will enter a new trigger into the current data base.  The trigger will be
associated with the relation
.IR relname
and will execute the specified
.IR funcname .

Only relation owner may create a trigger on this relation.

Currently, STATEMENT triggers are not implemented.
.PP
The trigger can be specified to fire either
.BR before
the operation is attempted on a tuple (e.g. before constraints are checked and
the insert/update/delete is attempted) or
.BR after
the operation has been attempted (e.g. after constraints are checked and the
insert/update/delete has completed).
If the trigger fires
.BR before
then the trigger may
skip the operation for the current tuple, 
or change tuple being inserted (for insert/update operations only).
If the trigger fires
.BR after
then all changes including the last insertion/updation/deletion
are "visible" to trigger.

.PP
Refer to the SPI and trigger programming guides for more information.
.SH EXAMPLES
Examples are included in the contrib area of the source distribution.
.SH "SEE ALSO"
drop_trigger(l).
