#!/bin/sh
# $Header: /cvsroot/pgsql/src/test/bench/Attic/create.sh,v 1.1.1.1 1996/07/09 06:22:21 scrappy Exp $
# 
if [ -d ./obj ]; then
	cd ./obj
fi

echo =============== destroying old bench database... =================
echo "drop database bench" | postgres template1 > /dev/null

echo =============== creating new bench database... =================
echo "create database bench" | postgres template1 > /dev/null
if [ $? -ne 0 ]; then
	echo createdb failed
	exit 1
fi

postgres -Q bench < create.sql > /dev/null
if [ $? -ne 0 ]; then
	echo initial database load failed
	exit 1
fi

exit 0
