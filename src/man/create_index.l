.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_index.l,v 1.1 1996/11/14 10:15:55 scrappy Exp $
.TH "CREATE INDEX" SQL 11/05/95 Postgres95 Postgres95
.SH NAME
create index \(em construct a secondary index
.SH SYNOPSIS
.nf
\fBcreate\fR \fBindex\fR index-name
	\fBon\fR classname [\fBusing\fR am-name]
	\fB(\fR attname [type_class\fB] )\fR

\fBcreate\fR \fBindex\fR index-name
	\fBon\fR classname [\fBusing\fR am-name]
	\fB(\fR funcname \fB(\fR attname\-1 { , attname\-i } \fB)\fR type_class \fB)\fR
.fi
.SH DESCRIPTION
This command constructs an index called
.IR index-name.
.PP
.IR Am-name
is the name of the access method which is used for the index.
The default access method is btree.
.PP
In the first syntax shown above, the key field for the index is
specified as an attribute name and an associated
.IR "operator class" .
An operator class is used to specify the operators to be used for a
particular index.  For example, a btree index on four-byte integers
would use the
.IR int4_ops
class; this operator class includes comparison functions for four-byte
integers.
The default operator class is the appropriate operator class for
that field type.
.PP
In the second syntax shown above, an index can be defined on the
result of a user-defined function
.IR funcname
applied to one or more attributes of a single class.  These
.IR "functional indices"
are primarily useful in two situations.  First, functional indices can
be used to simulate multikey indices.  That is, the user can define a
new base type (a simple combination of, say, \*(lqoid\*(rq and
\*(lqint2\*(rq) and the associated functions and operators on this new
type such that the access method can use it.  Once this has been done,
the standard techniques for interfacing new types to access methods
(described in the Postgres user manual) can be applied.  Second,
functional indices can be used to obtain fast access to data based on
operators that would normally require some transformation to be
applied to the base data.  For example, say you have an attribute in
class \*(lqmyclass\*(rq called \*(lqpt\*(rq that consists of a 2D
point type.  Now, suppose that you would like to index this attribute
but you only have index operator classes for 2D polygon types.  You
can define an index on the point attribute using a function that you
write (call it \*(lqpoint_to_polygon\*(rq) and your existing polygon
operator class; after that, queries using existing polygon operators
that reference \*(lqpoint_to_polygon(myclass.pt)\*(rq on one side will
use the precomputed polygons stored in the functional index instead of
computing a polygon for each and every instance in \*(lqmyclass\*(rq
and then comparing it to the value on the other side of the operator.
Obviously, the decision to build a functional index represents a
tradeoff between space (for the index) and execution time.
.PP
Postgres provides btree, rtree and hash access methods for
secondary indices.  The btree access method is an implementation of
the Lehman-Yao high-concurrency btrees.  The rtree access method
implements standard rtrees using Guttman's quadratic split algorithm.
The hash access method is an implementation of Litwin's linear
hashing.  We mention the algorithms used solely to indicate that all
of these access methods are fully dynamic and do not have to be
optimized periodically (as is the case with, for example, static hash
access methods).
.PP
This list was generated from the Postgres system catalogs with the query:

.nf
SELECT am.amname AS acc_name,
       opc.opcname AS ops_name,
       opr.oprname AS ops_comp
FROM   pg_am am, pg_amop amop, pg_opclass opc, pg_operator opr
WHERE  amop.amopid = am.oid AND
       amop.amopclaid = opc.oid AND
       amop.amopopr = opr.oid
ORDER BY acc_name, ops_name, ops_comp;

acc_name|ops_name   |ops_comp
--------+-----------+--------
btree   |abstime_ops|<       
btree   |abstime_ops|<=      
btree   |abstime_ops|=       
btree   |abstime_ops|>       
btree   |abstime_ops|>=      
btree   |bpchar_ops |<       
btree   |bpchar_ops |<=      
btree   |bpchar_ops |=       
btree   |bpchar_ops |>       
btree   |bpchar_ops |>=      
btree   |char16_ops |<       
btree   |char16_ops |<=      
btree   |char16_ops |=       
btree   |char16_ops |>       
btree   |char16_ops |>=      
btree   |char2_ops  |<       
btree   |char2_ops  |<=      
btree   |char2_ops  |=       
btree   |char2_ops  |>       
btree   |char2_ops  |>=      
btree   |char4_ops  |<       
btree   |char4_ops  |<=      
btree   |char4_ops  |=       
btree   |char4_ops  |>       
btree   |char4_ops  |>=      
btree   |char8_ops  |<       
btree   |char8_ops  |<=      
btree   |char8_ops  |=       
btree   |char8_ops  |>       
btree   |char8_ops  |>=      
btree   |char_ops   |<       
btree   |char_ops   |<=      
btree   |char_ops   |=       
btree   |char_ops   |>       
btree   |char_ops   |>=      
btree   |date_ops   |<       
btree   |date_ops   |<=      
btree   |date_ops   |=       
btree   |date_ops   |>       
btree   |date_ops   |>=      
btree   |float4_ops |<       
btree   |float4_ops |<=      
btree   |float4_ops |=       
btree   |float4_ops |>       
btree   |float4_ops |>=      
btree   |float8_ops |<       
btree   |float8_ops |<=      
btree   |float8_ops |=       
btree   |float8_ops |>       
btree   |float8_ops |>=      
btree   |int24_ops  |<       
btree   |int24_ops  |<=      
btree   |int24_ops  |=       
btree   |int24_ops  |>       
btree   |int24_ops  |>=      
btree   |int2_ops   |<       
btree   |int2_ops   |<=      
btree   |int2_ops   |=       
btree   |int2_ops   |>       
btree   |int2_ops   |>=      
btree   |int42_ops  |<       
btree   |int42_ops  |<=      
btree   |int42_ops  |=       
btree   |int42_ops  |>       
btree   |int42_ops  |>=      
btree   |int4_ops   |<       
btree   |int4_ops   |<=      
btree   |int4_ops   |=       
btree   |int4_ops   |>       
btree   |int4_ops   |>=      
btree   |name_ops   |<       
btree   |name_ops   |<=      
btree   |name_ops   |=       
btree   |name_ops   |>       
btree   |name_ops   |>=      
btree   |oid_ops    |<       
btree   |oid_ops    |<=      
btree   |oid_ops    |=       
btree   |oid_ops    |>       
btree   |oid_ops    |>=      
btree   |oidint2_ops|<       
btree   |oidint2_ops|<=      
btree   |oidint2_ops|=       
btree   |oidint2_ops|>       
btree   |oidint2_ops|>=      
btree   |oidint4_ops|<       
btree   |oidint4_ops|<=      
btree   |oidint4_ops|=       
btree   |oidint4_ops|>       
btree   |oidint4_ops|>=      
btree   |oidname_ops|<       
btree   |oidname_ops|<=      
btree   |oidname_ops|=       
btree   |oidname_ops|>       
btree   |oidname_ops|>=      
btree   |text_ops   |<       
btree   |text_ops   |<=      
btree   |text_ops   |=       
btree   |text_ops   |>       
btree   |text_ops   |>=      
btree   |time_ops   |<       
btree   |time_ops   |<=      
btree   |time_ops   |=       
btree   |time_ops   |>       
btree   |time_ops   |>=      
btree   |varchar_ops|<       
btree   |varchar_ops|<=      
btree   |varchar_ops|=       
btree   |varchar_ops|>       
btree   |varchar_ops|>=      
hash    |bpchar_ops |=       
hash    |char16_ops |=       
hash    |char2_ops  |=       
hash    |char4_ops  |=       
hash    |char8_ops  |=       
hash    |char_ops   |=       
hash    |date_ops   |=       
hash    |float4_ops |=       
hash    |float8_ops |=       
hash    |int2_ops   |=       
hash    |int4_ops   |=       
hash    |name_ops   |=       
hash    |oid_ops    |=       
hash    |text_ops   |=       
hash    |time_ops   |=       
hash    |varchar_ops|=       
rtree   |bigbox_ops |&&      
rtree   |bigbox_ops |&<      
rtree   |bigbox_ops |&>      
rtree   |bigbox_ops |<<      
rtree   |bigbox_ops |>>      
rtree   |bigbox_ops |@       
rtree   |bigbox_ops |~       
rtree   |bigbox_ops |~=      
rtree   |box_ops    |&&      
rtree   |box_ops    |&<      
rtree   |box_ops    |&>      
rtree   |box_ops    |<<      
rtree   |box_ops    |>>      
rtree   |box_ops    |@       
rtree   |box_ops    |~       
rtree   |box_ops    |~=      
rtree   |poly_ops   |&&      
rtree   |poly_ops   |&<      
rtree   |poly_ops   |&>      
rtree   |poly_ops   |<<      
rtree   |poly_ops   |>>      
rtree   |poly_ops   |@       
rtree   |poly_ops   |~       
rtree   |poly_ops   |~=      

.fi
The
.IR int24_ops
operator class is useful for constructing indices on int2 data, and
doing comparisons against int4 data in query qualifications.
Similarly,
.IR int42_ops
support indices on int4 data that is to be compared against int2 data
in queries.
.PP
The operator classes
.IR oidint2_ops ,
.IR oidint4_ops ,
and
.IR oidchar16_ops
represent the use of 
.IR "functional indices"
to simulate multi-key indices.
.PP
The Postgres query optimizer will consider using btree indices in a scan
whenever an indexed attribute is involved in a comparison using one of:

.nf
<    <=    =    >=    >
.fi

Both box classes support indices on the \*(lqbox\*(rq datatype in
Postgres.  The difference between them is that
.IR bigbox_ops
scales box coordinates down, to avoid floating point exceptions from
doing multiplication, addition, and subtraction on very large
floating-point coordinates.  If the field on which your rectangles lie
is about 20,000 units square or larger, you should use
.IR bigbox_ops .
The
.IR poly_ops
operator class supports rtree indices on \*(lqpolygon\*(rq data.
.PP
The Postgres query optimizer will consider using an rtree index whenever
an indexed attribute is involved in a comparison using one of:

.nf
<<    &<    &>    >>    @    ~=    &&
.fi

The Postgres query optimizer will consider using a hash index whenever
an indexed attribute is involved in a comparison using the \fB=\fR operator.
.SH EXAMPLES
.nf
--
--Create a btree index on the emp class using the age attribute.
--
create index empindex on emp using btree (age int4_ops)
.fi
.nf
--
--Create a btree index on employee name.
--
create index empname
	on emp using btree (name char16_ops)
.fi
.nf
--
--Create an rtree index on the bounding rectangle of cities.
--
create index cityrect
	on city using rtree (boundbox box_ops)
.fi
.nf
--
--Create a rtree index on a point attribute such that we
--can efficiently use box operators on the result of the 
--conversion function.  Such a qualification might look 
--like "where point2box(points.pointloc) = boxes.box".
--
create index pointloc
	on points using rtree (point2box(location) box_ops)
.nf
