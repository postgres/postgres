.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_sequence.l,v 1.6 1998/08/30 21:03:19 scrappy Exp $
.TH "CREATE SEQUENCE" SQL 07/13/98 PostgreSQL PostgreSQL
.SH NAME
create sequence - create a new sequence number generator
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
Actually, a new single-record
.BR table
with name 
.IR seqname
will be created and initialized. 
The generator will be
\*(lqowned\*(rq by the user issuing the command.
.PP
The 
.BR increment
clause is optional. A positive value will make an ascending sequence,
negative - descending. Default value is 1. 
.PP
The optional integer
.BR minvalue
determines the minimum value the sequence can generate. Defaults are
1/-2147483647 for ascending/descending sequences.
.PP
The optional integer
.BR maxvalue
determines the maximum value the sequence can generate. Defaults are
2147483647/-1 for ascending/descending sequences.
.PP
The optional
.BR start
value sets the first value to be generated.  Default is 
.BR minvalue
for ascending sequences and 
.BR maxvalue
for descending ones.
.PP
The
.BR cache
option enables sequence numbers to be preallocated and 
stored in memory for faster access. The minimum value is 1 
(one value will be allocated at a time, i.e., no cache)
and that is the default.  See below for details.
.PP
The optional
.BR cycle
keyword may be used to enable the sequence to continue after the 
.BR maxvalue/minvalue 
has been reached by ascending/descending sequence.
If the limit is reached, the next number generated will be 
whatever the 
.BR minvalue/maxvalue 
is.
.PP
After a sequence object has been created, you may use the function
.BR nextval
with the sequence name as argument to generate a new number from the
specified sequence.
.PP
The function
.BR currval
('sequence_name')
may be used to re-fetch the number returned by the last call to
.BR nextval
for the specified sequence in the current session.
.BR NOTE: 
currval will return an error if nextval has never been called for the
given sequence in the current backend session.  Also beware that it
does not give the last number ever allocated, only the last one allocated
by this backend.
.PP
The function
.BR setval
('sequence_name', value)
may be used to set the current value of the specified sequence.
The next call to
.BR nextval
will return the given value + the sequence increment.
.PP
Use a query like
.nf
SELECT * FROM <sequence_name>;
.fi
to get the parameters of a sequence.  Aside from fetching the original
parameters, you can use
.nf
SELECT last_value FROM <sequence_name>;
.fi
to obtain the last value allocated by any backend.
.PP
Low-level locking is used to ensure that multiple backends can safely use
a sequence object concurrently.
.PP
.BR NOTE: 
Unexpected results may be obtained if a cache setting greater than one
is used for a sequence object that will be used concurrently by multiple
backends.  Each backend will allocate "cache" successive sequence values
during one access to the sequence object and increase the sequence
object's last_value accordingly.  Then, the next cache-1 uses of nextval
within that backend simply return the preallocated values without touching
the shared object.  So, numbers allocated but not used in the current session
will be lost.  Furthermore, although multiple backends are guaranteed to
allocate distinct sequence values, the values may be generated out of
sequence when all the backends are considered.  (For example, with a cache
setting of 10, backend A might reserve values 1..10 and return nextval=1, then
backend B might reserve values 11..20 and return nextval=11 before backend
A has generated nextval=2.)  Thus, with a cache setting of one it is safe
to assume that nextval values are generated sequentially; with a cache
setting greater than one you should only assume that the nextval values
are all distinct, not that they are generated purely sequentially.
Also, last_value will reflect the latest value reserved by any backend,
whether or not it has yet been returned by nextval.
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
.nf
--
-- Set the sequence value after a copy in
--
create function table_id_max() returns int4
    as 'select max(id) from _table_' 
    language 'sql';
copy _table_ from 'input_file';
select setval('seq', table_id_max());
.fi
.SH "SEE ALSO"
drop_sequence(l).
