#!/bin/sh

while read line
do
	echo "trying $line"
	./uri-regress "$line"
	echo ""
done < "${SRCDIR}/${SUBDIR}"/regress.in >regress.out 2>&1

if diff -c "${SRCDIR}/${SUBDIR}/"expected.out regress.out >regress.diff; then
	echo "========================================"
	echo "All tests passed"
	exit 0
else
	echo "========================================"
	echo "FAILED: the test result differs from the expected output"
	echo
	echo "Review the difference in ${SUBDIR}/regress.diff"
	echo "========================================"
	exit 1
fi
