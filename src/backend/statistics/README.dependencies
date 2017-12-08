Soft functional dependencies
============================

Functional dependencies are a concept well described in relational theory,
particularly in the definition of normalization and "normal forms". Wikipedia
has a nice definition of a functional dependency [1]:

    In a given table, an attribute Y is said to have a functional dependency
    on a set of attributes X (written X -> Y) if and only if each X value is
    associated with precisely one Y value. For example, in an "Employee"
    table that includes the attributes "Employee ID" and "Employee Date of
    Birth", the functional dependency

        {Employee ID} -> {Employee Date of Birth}

    would hold. It follows from the previous two sentences that each
    {Employee ID} is associated with precisely one {Employee Date of Birth}.

    [1] https://en.wikipedia.org/wiki/Functional_dependency

In practical terms, functional dependencies mean that a value in one column
determines values in some other column. Consider for example this trivial
table with two integer columns:

    CREATE TABLE t (a INT, b INT)
        AS SELECT i, i/10 FROM generate_series(1,100000) s(i);

Clearly, knowledge of the value in column 'a' is sufficient to determine the
value in column 'b', as it's simply (a/10). A more practical example may be
addresses, where the knowledge of a ZIP code (usually) determines city. Larger
cities may have multiple ZIP codes, so the dependency can't be reversed.

Many datasets might be normalized not to contain such dependencies, but often
it's not practical for various reasons. In some cases, it's actually a conscious
design choice to model the dataset in a denormalized way, either because of
performance or to make querying easier.


Soft dependencies
-----------------

Real-world data sets often contain data errors, either because of data entry
mistakes (user mistyping the ZIP code) or perhaps issues in generating the
data (e.g. a ZIP code mistakenly assigned to two cities in different states).

A strict implementation would either ignore dependencies in such cases,
rendering the approach mostly useless even for slightly noisy data sets, or
result in sudden changes in behavior depending on minor differences between
samples provided to ANALYZE.

For this reason, extended statistics implement "soft" functional dependencies,
associating each functional dependency with a degree of validity (a number
between 0 and 1). This degree is then used to combine selectivities in a
smooth manner.


Mining dependencies (ANALYZE)
-----------------------------

The current algorithm is fairly simple - generate all possible functional
dependencies, and for each one count the number of rows consistent with it.
Then use the fraction of rows (supporting/total) as the degree.

To count the rows consistent with the dependency (a => b):

 (a) Sort the data lexicographically, i.e. first by 'a' then 'b'.

 (b) For each group of rows with the same 'a' value, count the number of
     distinct values in 'b'.

 (c) If there's a single distinct value in 'b', the rows are consistent with
     the functional dependency, otherwise they contradict it.


Clause reduction (planner/optimizer)
------------------------------------

Applying the functional dependencies is fairly simple: given a list of
equality clauses, we compute selectivities of each clause and then use the
degree to combine them using this formula

    P(a=?,b=?) = P(a=?) * (d + (1-d) * P(b=?))

Where 'd' is the degree of functional dependency (a => b).

With more than two equality clauses, this process happens recursively. For
example for (a,b,c) we first use (a,b => c) to break the computation into

    P(a=?,b=?,c=?) = P(a=?,b=?) * (e + (1-e) * P(c=?))

where 'e' is the degree of functional dependency (a,b => c); then we can
apply (a=>b) the same way on P(a=?,b=?).


Consistency of clauses
----------------------

Functional dependencies only express general dependencies between columns,
without referencing particular values. This assumes that the equality clauses
are in fact consistent with the functional dependency, i.e. that given a
dependency (a=>b), the value in (b=?) clause is the value determined by (a=?).
If that's not the case, the clauses are "inconsistent" with the functional
dependency and the result will be over-estimation.

This may happen, for example, when using conditions on the ZIP code and city
name with mismatching values (ZIP code for a different city), etc. In such a
case, the result set will be empty, but we'll estimate the selectivity using
the ZIP code condition.

In this case, the default estimation based on AVIA principle happens to work
better, but mostly by chance.

This issue is the price for the simplicity of functional dependencies. If the
application frequently constructs queries with clauses inconsistent with
functional dependencies present in the data, the best solution is not to
use functional dependencies, but one of the more complex types of statistics.
