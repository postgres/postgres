.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_sequence.l,v 1.2 1997/06/03 03:19:08 vadim Exp $
.TH "CREATE SEQUENCE" SQL 04/01/97 PostgreSQL PostgreSQL
.SH NAME
create sequence \(em create a new sequence number generator
.SH SYNOPSIS
.nf
\fBcreate sequence\fR seqname 
	[\fBincrement\fP incby_value] 
	[\fBminvalue\fP min_value] 
	[\fBmaxvalue\fP max_value] 
	[\fBstart\fP start_value] 
	[\fBcache\fP cache_value] 
	[\fBcycle\fP] 
.fi
.SH DESCRIPTION
.BR "Create sequence"
will enter a new sequence number generator into the current data base.
Actually, new single block
.BR table
with name 
.IR seqname
will be created and initialized. 
The generator will be
\*(lqowned\*(rq by the user issuing the command.
.PP
The 
.BR "increment"
is optional clause. Positive value will make ascending sequence,
negative - descending. Default value is 1. 
.PP
The optional integer
.BR minvalue
determines the minimum value a sequence can be. Defaults are
1/-2147483647 for ascending/descending sequences.
.PP
Use optional integer
.BR maxvalue
to determine the maximum value for sequence. Defaults are
2147483647/-1 for ascending/descending sequences.
.PP
The optinal
.BR "start"
value enables sequence to begin anywhere.  Default is 
.BR minvalue
for ascending sequences and 
.BR maxvalue
for descending ones.
.PP
The
.BR cache
option enables sequence numbers to be preallocated and 
stored in memory for faster access. The minimum value is 1 
(i.e. - no cache) and it is default.
.BR NOTE: 
each backend uses own cache to store allocated numbers.
Cached but not used in current session numbers will be lost.
.PP
The optional
.BR cycle
keyword may be used to enable sequence to continue when the 
.BR maxvalue/minvalue 
has been reached by ascending/descending sequence.
If the limit is reached, the next number generated will be 
whatever the 
.BR minvalue/maxvalue 
is.
.PP
After sequence created, You may use function
.BR nextval
with sequence name as argument to get new number from sequence
specified.
Function
.BR currval
('sequence_name')
may be used to determine number returned by last call to
.BR nextval
for specified sequence in current session.
.PP
.nf
Use query like

select * from <sequence_name>;

to get parameters of a sequence.
.fi
.PP
Low-level locking is used to enable multiple simultaneous calls
to a generator.
.PP
.SH EXAMPLES
.nf
--
-- Create sequence seq caching 2 numbers, starting with 10
--
create sequence seq cache 2 start 10;
.fi
.nf
--
-- Select next number from sequence
--
select nextval ('seq');
.fi
.nf
--
-- Use sequence in insert
--
insert into table _table_ values (nextval ('seq'),...);
.fi
.SH "SEE ALSO"
drop sequence(l).
