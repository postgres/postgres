.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/explain.l,v 1.7 1998/01/07 15:40:17 momjian Exp $
.TH EXPLAIN SQL 06/12/97 PostgreSQL PostgreSQL
.SH NAME
explain \(em explains statement execution details
.SH SYNOPSIS
.nf
\fBexplain [verbose]\fR query
.fi
.SH DESCRIPTION
This command outputs details about the supplied query.  The default
output is the computed query cost.  \f2verbose\f1 displays the full query
plan and cost.

.SH EXAMPLES
In the examples, the table has a single column of float4.
\fBcost\fR is the cost of scanning a base/join relation,
\fBsize\fR is the expected number of tuples from a scan,
\fBwidth\fR is the length of a tuple.

.nf
tgl=> explain select a from test\g
NOTICE:QUERY PLAN:

Seq Scan on test  (cost=0.00 size=0 width=4)

EXPLAIN
tgl=> explain verbose select sum(a) from test;
NOTICE:QUERY PLAN:

{AGG :cost 0 :size 0 :width 0 :state <> :qptargetlist
 ({TLE :resdom {RESDOM :resno 1 :restype 700 :reslen 4 :resname "sum"
   :reskey 0 :reskeyop 0 :resjunk 0}
  :expr {AGGREG :aggname "sum" :basetype 700 :aggtype 700 :aggno 0
 :target {VAR :varno 1 :varattno 1 :vartype 700 :varnoold 1 :varoattno 1}}})
 :qpqual <> :lefttree {SEQSCAN :cost 0 :size 0 :width 4 :state <>
  :qptargetlist ({TLE :resdom {RESDOM :resno 1 :restype 700 :reslen 4
   :resname "null" :reskey 0 :reskeyop 0 :resjunk 0}
  :expr {VAR :varno 1 :varattno 1 :vartype 700 :varnoold 1 :varoattno 1}})
 :qpqual <> :lefttree <> :righttree <> :scanrelid 1} :righttree <> :numagg 1 }

Aggregate  (cost=0.00 size=0 width=0)
  ->   Seq Scan on test  (cost=0.00 size=0 width=4)
.fi

The Postgres optimizer has chosen to use a sequential scan to retrieve rows from
this table. Indices will used by the optimizer
after tables grow large enough to warrant the access
overhead; typically this might happen when tables have a few hundred rows.

.SH "SEE ALSO"
delete(l),
insert(l),
select(l).

.SH BUGS

.PP
The query cost and plan can be affected by running vacuum.

