test_predtest is a module for checking the correctness of the optimizer's
predicate-proof logic, in src/backend/optimizer/util/predtest.c.

The module provides a function that allows direct application of
predtest.c's exposed functions, predicate_implied_by() and
predicate_refuted_by(), to arbitrary boolean expressions, with direct
inspection of the results.  This could be done indirectly by checking
planner results, but it can be difficult to construct end-to-end test
cases that prove that the expected results were obtained.

In general, the use of this function is like
	select * from test_predtest('query string')
where the query string must be a SELECT returning two boolean
columns, for example

	select * from test_predtest($$
	select x, not x
	from (values (false), (true), (null)) as v(x)
	$$);

The function parses and plans the given query, and then applies the
predtest.c code to the two boolean expressions in the SELECT list, to see
if the first expression can be proven or refuted by the second.  It also
executes the query, and checks the resulting rows to see whether any
claimed implication or refutation relationship actually holds.  If the
query is designed to exercise the expressions on a full set of possible
input values, as in the example above, then this provides a mechanical
cross-check as to whether the proof code has given a correct answer.
