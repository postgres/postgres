#! /bin/sh

# This script runs the Postgres regression tests with all useful combinations
# of the backend options that disable various query plan types.  If the
# results are not all the same, it may indicate a bug in a particular
# plan type, or perhaps just a regression test whose results aren't fully
# determinate (eg, due to lack of an ORDER BY keyword).
#
# Run this in the src/test/regress directory, after doing the usual setup
# for a regular regression test, ie, "make clean all" (you should be ready
# to do "make runtest").
#
# The backend option switches that we use here are:
#	-fs	disable sequential scans
#	-fi	disable index scans
#	-fn	disable nestloop joins
#	-fm	disable merge joins
#	-fh	disable hash joins
# Only mergejoin and hashjoin are really guaranteed to turn off; the others
# just bias the optimizer's cost calculations heavily against that choice.
# There's no point in trying to turn off both scan types or all three join
# types simultaneously; ergo, we have 3*7 = 21 interesting combinations.
#
# Note that this will take *more than* 21 times longer than a regular
# regression test, since we are preventing the system from using the most
# efficient available query plans!  Have patience.


# Select make to use --- default gmake, can be overridden by env var
MAKE="${MAKE:-gmake}"

# If PGOPTIONS is already defined, we'll add the -f switches to it.
PGOPTIONS="${PGOPTIONS:-}"

mkdir planregress

PGOPTIONS="$PGOPTIONS                     " $MAKE runtest
mv -f regression.out planregress/out.normal
mv -f regression.diffs planregress/diffs.normal
PGOPTIONS="$PGOPTIONS                  -fh" $MAKE runtest
mv -f regression.out planregress/out.h
mv -f regression.diffs planregress/diffs.h
PGOPTIONS="$PGOPTIONS              -fm    " $MAKE runtest
mv -f regression.out planregress/out.m
mv -f regression.diffs planregress/diffs.m
PGOPTIONS="$PGOPTIONS              -fm -fh" $MAKE runtest
mv -f regression.out planregress/out.mh
mv -f regression.diffs planregress/diffs.mh
PGOPTIONS="$PGOPTIONS          -fn        " $MAKE runtest
mv -f regression.out planregress/out.n
mv -f regression.diffs planregress/diffs.n
PGOPTIONS="$PGOPTIONS          -fn     -fh" $MAKE runtest
mv -f regression.out planregress/out.nh
mv -f regression.diffs planregress/diffs.nh
PGOPTIONS="$PGOPTIONS          -fn -fm    " $MAKE runtest
mv -f regression.out planregress/out.nm
mv -f regression.diffs planregress/diffs.nm
PGOPTIONS="$PGOPTIONS      -fi            " $MAKE runtest
mv -f regression.out planregress/out.i
mv -f regression.diffs planregress/diffs.i
PGOPTIONS="$PGOPTIONS      -fi         -fh" $MAKE runtest
mv -f regression.out planregress/out.ih
mv -f regression.diffs planregress/diffs.ih
PGOPTIONS="$PGOPTIONS      -fi     -fm    " $MAKE runtest
mv -f regression.out planregress/out.im
mv -f regression.diffs planregress/diffs.im
PGOPTIONS="$PGOPTIONS      -fi     -fm -fh" $MAKE runtest
mv -f regression.out planregress/out.imh
mv -f regression.diffs planregress/diffs.imh
PGOPTIONS="$PGOPTIONS      -fi -fn        " $MAKE runtest
mv -f regression.out planregress/out.in
mv -f regression.diffs planregress/diffs.in
PGOPTIONS="$PGOPTIONS      -fi -fn     -fh" $MAKE runtest
mv -f regression.out planregress/out.inh
mv -f regression.diffsregression.planregress/inh
PGOPTIONS="$PGOPTIONS      -fi -fn -fm    " $MAKE runtest
mv -f regression.out planregress/out.inm
mv -f regression.diffs planregress/diffs.inm
PGOPTIONS="$PGOPTIONS  -fs                " $MAKE runtest
mv -f regression.out planregress/out.s
mv -f regression.diffs planregress/diffs.s
PGOPTIONS="$PGOPTIONS  -fs             -fh" $MAKE runtest
mv -f regression.out planregress/out.sh
mv -f regression.diffs planregress/diffs.sh
PGOPTIONS="$PGOPTIONS  -fs         -fm    " $MAKE runtest
mv -f regression.out planregress/out.sm
mv -f regression.diffs planregress/diffs.sm
PGOPTIONS="$PGOPTIONS  -fs         -fm -fh" $MAKE runtest
mv -f regression.out planregress/out.smh
mv -f regression.diffs planregress/diffs.smh
PGOPTIONS="$PGOPTIONS  -fs     -fn        " $MAKE runtest
mv -f regression.out planregress/out.sn
mv -f regression.diffs planregress/diffs.sn
PGOPTIONS="$PGOPTIONS  -fs     -fn     -fh" $MAKE runtest
mv -f regression.out planregress/out.snh
mv -f regression.diffs planregress/diffs.snh
PGOPTIONS="$PGOPTIONS  -fs     -fn -fm    " $MAKE runtest
mv -f regression.out planregress/out.snm
mv -f regression.diffs planregress/diffs.snm

exit 0
