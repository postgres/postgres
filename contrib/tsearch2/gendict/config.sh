#!/bin/sh

usage () {
	echo Usage:
	echo $0 -n DICTNAME  \( [ -s [ -p PREFIX ] ] \| [ -c CFILES ] [ -h HFILES ] [ -i ] \) [ -v ] [ -d DIR ] [ -C COMMENT ]
	echo '    -v - be verbose'
	echo '    -d DIR - name of directory in PGSQL_SRL/contrib (default dict_DICTNAME)'
	echo '    -C COMMENT - dictionary comment' 
	echo Generate Snowball stemmer:
	echo $0 -n DICTNAME -s [ -p PREFIX ] [ -v ] [ -d DIR ] [ -C COMMENT ]
	echo '    -s - generate Snowball wrapper'
	echo "    -p - prefix of Snowball's function, (default DICTNAME)" 
	echo Generate template dictionary:
	echo $0 -n DICTNAME [ -c CFILES ] [ -h HFILES ] [ -i ] [ -v ] [ -d DIR ] [ -C COMMENT ]
	echo '    -c CFILES - source files, must be placed in contrib/tsearch2/gendict directory.'
	echo '                These files will be used in Makefile.'
	echo '    -h HFILES - header files, must be placed in contrib/tsearch2/gendict directory.'
	echo '                These files will be used in Makefile and subinclude.h'
	echo '    -i - dictionary has init method'
	exit 1;
}

dictname=
stemmode=no
verbose=no
cfile=
hfile=
dir= 
hasinit=no
comment=
prefix=

while getopts n:c:C:h:d:p:vis opt
do
	case "$opt" in
		v) verbose=yes;;
		s) stemmode=yes;;
		i) hasinit=yes;;
		n) dictname="$OPTARG";;
		c) cfile="$OPTARG";;
		h) hfile="$OPTARG";;
		d) dir="$OPTARG";;
		C) comment="$OPTARG";;
		p) prefix="$OPTARG";;
		\?) usage;;
	esac
done

[ ${#dictname} -eq 0 ] && usage

dictname=`echo $dictname | tr '[:upper:]' '[:lower:]'`

if [ $stemmode = "yes" ] ; then 
	[ ${#prefix} -eq 0 ] && prefix=$dictname
	hasinit=yes
	cfile="stem.c"
	hfile="stem.h"
fi 

[ ${#dir}   -eq 0 ] && dir="dict_$dictname"

if [ ${#comment} -eq 0 ]; then
	comment=null
else
	comment="'$comment'"
fi

ofile=
for f in $cfile
do
	f=` echo $f | sed 's#c$#o#'`
	ofile="$ofile $f"
done

if [ $stemmode = "yes" ] ; then
	ofile="$ofile dict_snowball.o"
else
	ofile="$ofile dict_tmpl.o"
fi

if [ $verbose = "yes" ]; then
	echo Dictname: "'"$dictname"'"
	echo Snowball stemmer: $stemmode
	echo Has init method: $hasinit
	[ $stemmode = "yes" ] && echo Function prefix: $prefix 
	echo Source files: $cfile
	echo Header files: $hfile
	echo Object files: $ofile
	echo Comment: $comment
	echo Directory: ../../$dir
fi


[ $verbose = "yes" ] && echo -n 'Build directory...  '
if [ ! -d ../../$dir ]; then
	if ! mkdir ../../$dir ; then 
		echo "Can't create directory ../../$dir"
		exit 1
	fi 
fi
[ $verbose = "yes" ] && echo ok


[ $verbose = "yes" ] && echo -n 'Build Makefile...  '
sed s#CFG_DIR#$dir# < Makefile.IN | sed s#CFG_MODNAME#$dictname# | sed "s#CFG_OFILE#$ofile#" > ../../$dir/Makefile.tmp
if [ $stemmode = "yes" ] ; then
	sed "s#^PG_CPPFLAGS.*\$#PG_CPPFLAGS = -I../tsearch2/snowball -I../tsearch2#" < ../../$dir/Makefile.tmp >  ../../$dir/Makefile 
else
	sed "s#^PG_CPPFLAGS.*\$#PG_CPPFLAGS = -I../tsearch2#" < ../../$dir/Makefile.tmp >  ../../$dir/Makefile 
fi
rm ../../$dir/Makefile.tmp
[ $verbose = "yes" ] && echo ok


[ $verbose = "yes" ] && echo -n Build dict_$dictname'.sql.in...  '
if [ $hasinit = "yes" ]; then
	sed s#CFG_MODNAME#$dictname# < sql.IN | sed "s#CFG_COMMENT#$comment#" | sed s#^HASINIT## | sed 's#^NOINIT.*$##' > ../../$dir/dict_$dictname.sql.in.tmp
	if [ $stemmode = "yes" ] ; then
		sed s#^ISSNOWBALL## < ../../$dir/dict_$dictname.sql.in.tmp | sed s#^NOSNOWBALL.*\$## > ../../$dir/dict_$dictname.sql.in
	else
		sed s#^NOSNOWBALL## < ../../$dir/dict_$dictname.sql.in.tmp | sed s#^ISSNOWBALL.*\$## > ../../$dir/dict_$dictname.sql.in
	fi
	rm ../../$dir/dict_$dictname.sql.in.tmp	
else 
	sed s#CFG_MODNAME#$dictname# < sql.IN | sed "s#CFG_COMMENT#$comment#" | sed s#^NOINIT## | sed 's#^HASINIT.*$##' | sed s#^NOSNOWBALL## | sed s#^ISSNOWBALL.*\$## > ../../$dir/dict_$dictname.sql.in
fi
[ $verbose = "yes" ] && echo ok



if [ ${#cfile} -ne 0 ] || [ ${#hfile} -ne 0 ] ; then
	[ $verbose = "yes" ] && echo -n 'Copy source and header files...  '
	if [ ${#cfile} -ne 0 ] ; then
		if [ $stemmode = "yes" ] ; then
			for cfn in $cfile
			do
				sed s#../runtime/## < $cfn > ../../$dir/$cfn
			done
		else
			if ! cp $cfile ../../$dir ; then 
				echo "Can't cp all or one of files: $cfile"
				exit 1
			fi
		fi
	fi
	if [ ${#hfile} -ne 0 ] ; then 
		if ! cp $hfile ../../$dir ; then 
   			echo "Cant cp all or one of files: $hfile"
			exit 1
		fi
	fi
	[ $verbose = "yes" ] && echo ok
fi


[ $verbose = "yes" ] && echo -n 'Build sub-include header...  '
echo -n > ../../$dir/subinclude.h 
for i in $hfile
do
	echo "#include \"$i\"" >> ../../$dir/subinclude.h
done
[ $verbose = "yes" ] && echo ok


if  [ $stemmode = "yes" ] ; then 
	[ $verbose = "yes" ] && echo -n 'Build Snowball stemmer...  '
	sed s#CFG_MODNAME#$dictname#g < dict_snowball.c.IN | sed s#CFG_PREFIX#$prefix#g > ../../$dir/dict_snowball.c
else
	[ $verbose = "yes" ] && echo -n 'Build dictinonary...  '
	sed s#CFG_MODNAME#$dictname#g < dict_tmpl.c.IN > ../../$dir/dict_tmpl.c.tmp
	if [ $hasinit = "yes" ]; then
		sed s#^HASINIT## <  ../../$dir/dict_tmpl.c.tmp | sed 's#^NOINIT.*$##' > ../../$dir/dict_tmpl.c
	else 
		sed s#^HASINIT.*\$## <  ../../$dir/dict_tmpl.c.tmp | sed 's#^NOINIT##' > ../../$dir/dict_tmpl.c
	fi
	rm ../../$dir/dict_tmpl.c.tmp
fi 
[ $verbose = "yes" ] && echo ok


[ $verbose = "yes" ] && echo -n "Build README.$dictname...  "
if  [ $stemmode = "yes" ] ; then
	echo "Autogenerated Snowball's wrapper for $prefix" > ../../$dir/README.$dictname
else
	echo "Autogenerated template for $dictname" > ../../$dir/README.$dictname
fi
[ $verbose = "yes" ] && echo ok

echo All is done

