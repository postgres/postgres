#!/bin/sh
# $Header: /cvsroot/pgsql/src/test/bench/Attic/create.sh,v 1.2 1997/04/17 13:48:49 scrappy Exp $
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
