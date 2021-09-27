Extended statistics
===================

When estimating various quantities (e.g. condition selectivities) the default
approach relies on the assumption of independence. In practice that's often
not true, resulting in estimation errors.

Extended statistics track different types of dependencies between the columns,
hopefully improving the estimates and producing better plans.


Types of statistics
-------------------

There are currently several kinds of extended statistics:

    (a) ndistinct coefficients

    (b) soft functional dependencies (README.dependencies)

    (c) MCV lists (README.mcv)


Compatible clause types
-----------------------

Each type of statistics may be used to estimate some subset of clause types.

    (a) functional dependencies - equality clauses (AND), possibly IS NULL

    (b) MCV lists - equality and inequality clauses (AND, OR, NOT), IS [NOT] NULL

Currently, only OpExprs in the form Var op Const, or Const op Var are
supported, however it's feasible to expand the code later to also estimate the
selectivities on clauses such as Var op Var.


Complex clauses
---------------

We also support estimating more complex clauses - essentially AND/OR clauses
with (Var op Const) as leaves, as long as all the referenced attributes are
covered by a single statistics object.

For example this condition

    (a=1) AND ((b=2) OR ((c=3) AND (d=4)))

may be estimated using statistics on (a,b,c,d). If we only have statistics on
(b,c,d) we may estimate the second part, and estimate (a=1) using simple stats.

If we only have statistics on (a,b,c) we can't apply it at all at this point,
but it's worth pointing out clauselist_selectivity() works recursively and when
handling the second part (the OR-clause), we'll be able to apply the statistics.

Note: The multi-statistics estimation patch also makes it possible to pass some
clauses as 'conditions' into the deeper parts of the expression tree.


Selectivity estimation
----------------------

Throughout the planner clauselist_selectivity() still remains in charge of
most selectivity estimate requests. clauselist_selectivity() can be instructed
to try to make use of any extended statistics on the given RelOptInfo, which
it will do if:

    (a) An actual valid RelOptInfo was given. Join relations are passed in as
        NULL, therefore are invalid.

    (b) The relation given actually has any extended statistics defined which
        are actually built.

When the above conditions are met, clauselist_selectivity() first attempts to
pass the clause list off to the extended statistics selectivity estimation
function. This function may not find any clauses which it can perform any
estimations on. In such cases, these clauses are simply ignored. When actual
estimation work is performed in these functions they're expected to mark which
clauses they've performed estimations for so that any other function
performing estimations knows which clauses are to be skipped.

Size of sample in ANALYZE
-------------------------

When performing ANALYZE, the number of rows to sample is determined as

    (300 * statistics_target)

That works reasonably well for statistics on individual columns, but perhaps
it's not enough for extended statistics. Papers analyzing estimation errors
all use samples proportional to the table (usually finding that 1-3% of the
table is enough to build accurate stats).

The requested accuracy (number of MCV items or histogram bins) should also
be considered when determining the sample size, and in extended statistics
those are not necessarily limited by statistics_target.

This however merits further discussion, because collecting the sample is quite
expensive and increasing it further would make ANALYZE even more painful.
Judging by the experiments with the current implementation, the fixed size
seems to work reasonably well for now, so we leave this as future work.
