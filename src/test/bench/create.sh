#!/bin/sh
# $PostgreSQL: pgsql/src/test/bench/create.sh,v 1.3 2003/11/29 19:52:14 pgsql Exp $
# 
if [ ! -d $1 ]; then
	echo " you must specify a valid data directory "
	exit
fi
if [ -d ./obj ]; then
	cd ./obj
fi

echo =============== destroying old bench database... =================
echo "drop database bench" | postgres -D${1} template1 > /dev/null

echo =============== creating new bench database... =================
echo "create database bench" | postgres -D${1} template1 > /dev/null
if [ $? -ne 0 ]; then
	echo createdb failed
	exit 1
fi

postgres -D${1} -Q bench < create.sql > /dev/null
if [ $? -ne 0 ]; then
	echo initial database load failed
	exit 1
fi

exit 0
