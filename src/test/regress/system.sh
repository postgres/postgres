#!/bin/sh
echo -n "string to use for system specific expected/* files: "
../../../config/config.guess |awk -F\- '{ split($3,a,/[0-9]/); printf"%s-%s", $1, a[1] }'
echo ""
