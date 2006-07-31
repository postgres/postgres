#! /bin/sh
#-------------------------------------------------------------------------
#
# genbki.sh--
#    shell script which generates .bki files from specially formatted .h
#    files.  These .bki files are used to initialize the postgres template
#    database.
#
# Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/catalog/genbki.sh,v 1.40 2006/07/31 01:16:36 tgl Exp $
#
# NOTES
#    non-essential whitespace is removed from the generated file.
#    if this is ever a problem, then the sed script at the very
#    end can be changed into another awk script or something smarter.
#
#-------------------------------------------------------------------------

: ${AWK='awk'}

CMDNAME=`basename $0`

INCLUDE_DIRS=
OUTPUT_PREFIX=
INFILES=
major_version=

#
# Process command line switches.
#
while [ $# -gt 0 ]
do
    case $1 in
        -I)
            INCLUDE_DIRS="$INCLUDE_DIRS $2"
            shift;;
        -I*)
            arg=`echo $1 | sed -e 's/^-I//'`
            INCLUDE_DIRS="$INCLUDE_DIRS $arg"
            ;;
        -o)
            OUTPUT_PREFIX="$2"
            shift;;
        -o*)
            OUTPUT_PREFIX=`echo $1 | sed -e 's/^-o//'`
            ;;
        --set-version=*)
            arg=`expr x"$1" : x"--set-version=\(.*\)"`
            major_version=`expr x"$arg" : x'\([0-9][0-9]*\.[0-9][0-9]*\)'`
            ;;
        --help)
            echo "$CMDNAME generates system catalog bootstrapping files."
            echo
            echo "Usage:"
            echo "  $CMDNAME [ -I dir ] --set-version=VERSION -o prefix files..."
            echo
            echo "Options:"
            echo "  -I  path to postgres_ext.h and pg_config_manual.h files"
            echo "  -o  prefix of output files"
            echo "  --set-version  PostgreSQL version number for initdb cross-check"
            echo
            echo "The environment variable AWK determines which Awk program"
            echo "to use. The default is \`awk'."
            echo
            echo "Report bugs to <pgsql-bugs@postgresql.org>."
            exit 0
            ;;
        -*)
            echo "$CMDNAME: invalid option: $1"
            exit 1
            ;;
        *)
            INFILES="$INFILES $1"
            ;;
    esac
    shift
done

if [ x"$INFILES" = x"" ] ; then
    echo "$CMDNAME: no input files" 1>&2
    exit 1
fi

if [ x"$OUTPUT_PREFIX" = x"" ] ; then
    echo "$CMDNAME: no output prefix specified" 1>&2
    exit 1
fi

if [ x"$INCLUDE_DIRS" = x"" ] ; then
    echo "$CMDNAME: path to include directory unknown" 1>&2
    exit 1
fi

if [ x"$major_version" = x"" ] ; then
    echo "$CMDNAME: invalid or no version number specified" 1>&2
    exit 1
fi


TMPFILE="genbkitmp$$.c"

trap "rm -f $TMPFILE ${OUTPUT_PREFIX}.bki.$$ ${OUTPUT_PREFIX}.description.$$ ${OUTPUT_PREFIX}.shdescription.$$" 0 1 2 3 15


# Get NAMEDATALEN from postgres_ext.h
for dir in $INCLUDE_DIRS; do
    if [ -f "$dir/postgres_ext.h" ]; then
        NAMEDATALEN=`grep '^#define[ 	]*NAMEDATALEN' $dir/postgres_ext.h | $AWK '{ print $3 }'`
        break
    fi
done

# Get BOOTSTRAP_SUPERUSERID from catalog/pg_authid.h
for dir in $INCLUDE_DIRS; do
    if [ -f "$dir/catalog/pg_authid.h" ]; then
        BOOTSTRAP_SUPERUSERID=`grep '^#define[ 	]*BOOTSTRAP_SUPERUSERID' $dir/catalog/pg_authid.h | $AWK '{ print $3 }'`
        break
    fi
done

# Get PG_CATALOG_NAMESPACE from catalog/pg_namespace.h
for dir in $INCLUDE_DIRS; do
    if [ -f "$dir/catalog/pg_namespace.h" ]; then
        PG_CATALOG_NAMESPACE=`grep '^#define[ 	]*PG_CATALOG_NAMESPACE' $dir/catalog/pg_namespace.h | $AWK '{ print $3 }'`
        break
    fi
done

touch ${OUTPUT_PREFIX}.description.$$
touch ${OUTPUT_PREFIX}.shdescription.$$

# ----------------
# 	Strip comments and other trash from .h
#
#	Put multi-line start/end comments on a separate line
#
#	Rename datatypes that have different names in .h files than in SQL
#
#	Substitute values of configuration constants
# ----------------
#
cat $INFILES | \
sed -e 's;/\*.*\*/;;g' \
    -e 's;/\*;\
/*\
;g' \
    -e 's;\*/;\
*/\
;g' | # we must run a new sed here to see the newlines we added
sed -e "s/;[ 	]*$//g" \
    -e "s/^[ 	]*//" \
    -e "s/[ 	]Oid/ oid/g" \
    -e "s/^Oid/oid/g" \
    -e "s/(Oid/(oid/g" \
    -e "s/[ 	]NameData/ name/g" \
    -e "s/^NameData/name/g" \
    -e "s/(NameData/(name/g" \
    -e "s/[ 	]TransactionId/ xid/g" \
    -e "s/^TransactionId/xid/g" \
    -e "s/(TransactionId/(xid/g" \
    -e "s/PGUID/$BOOTSTRAP_SUPERUSERID/g" \
    -e "s/NAMEDATALEN/$NAMEDATALEN/g" \
    -e "s/PGNSP/$PG_CATALOG_NAMESPACE/g" \
| $AWK '
# ----------------
#	now use awk to process remaining .h file..
#
#	nc is the number of catalogs
#	inside is a variable set to 1 when we are scanning the
#	   contents of a catalog definition.
#	reln_open is a flag indicating when we are processing DATA lines.
#	   (i.e. have a relation open and need to close it)
#	oid is the most recently seen oid, or 0 if none in the last DATA line.
# ----------------
BEGIN {
	inside = 0;
	bootstrap = "";
	shared_relation = "";
	without_oids = "";
	nc = 0;
	reln_open = 0;
	comment_level = 0;
	oid = 0;
}

# ----------------
# Anything in a /* .. */ block should be ignored.
# Blank lines also go.
# Note that any /* */ comment on a line by itself was removed from the line
# by the sed above.
# ----------------
/^\/\*/           { comment_level += 1; next; }
/^\*\//           { comment_level -= 1; next; }
comment_level > 0 { next; }

/^[ 	]*$/      { next; }

# ----------------
#	DATA() statements are basically passed right through after
#	stripping off the DATA( and the ) on the end.
#	Remember the OID for use by DESCR() and SHDESCR().
# ----------------
/^DATA\(/ {
	data = substr($0, 6, length($0) - 6);
	oid = 0;
	nf = split(data, datafields);
	if (nf >= 4 && datafields[1] == "insert" && datafields[2] == "OID" && datafields[3] == "=")
	{
		oid = datafields[4];
	}
	print data;
	next;
}

/^DESCR\(/ {
	if (oid != 0)
	{
		data = substr($0, 8, length($0) - 9);
		if (data != "")
			printf "%d\t%s\t0\t%s\n", oid, catalog, data >>descriptionfile;
	}
	next;
}

/^SHDESCR\(/ {
	if (oid != 0)
	{
		data = substr($0, 10, length($0) - 11);
		if (data != "")
			printf "%d\t%s\t%s\n", oid, catalog, data >>shdescriptionfile;
	}
	next;
}

/^DECLARE_INDEX\(/ {
# ----
#  end any prior catalog data insertions before starting a define index
# ----
	if (reln_open == 1) {
		print "close " catalog;
		reln_open = 0;
	}

	data = substr($0, 15, length($0) - 15);
	pos = index(data, ",");
	iname = substr(data, 1, pos-1);
	data = substr(data, pos+1, length(data)-pos);
	pos = index(data, ",");
	oid = substr(data, 1, pos-1);
	data = substr(data, pos+1, length(data)-pos);

	print "declare index " iname " " oid " " data
}

/^DECLARE_UNIQUE_INDEX\(/ {
# ----
#  end any prior catalog data insertions before starting a define unique index
# ----
	if (reln_open == 1) {
		print "close " catalog;
		reln_open = 0;
	}

	data = substr($0, 22, length($0) - 22);
	pos = index(data, ",");
	iname = substr(data, 1, pos-1);
	data = substr(data, pos+1, length(data)-pos);
	pos = index(data, ",");
	oid = substr(data, 1, pos-1);
	data = substr(data, pos+1, length(data)-pos);

	print "declare unique index " iname " " oid " " data
}

/^DECLARE_TOAST\(/ {
# ----
#  end any prior catalog data insertions before starting a define toast
# ----
	if (reln_open == 1) {
		print "close " catalog;
		reln_open = 0;
	}

	data = substr($0, 15, length($0) - 15);
	pos = index(data, ",");
	tname = substr(data, 1, pos-1);
	data = substr(data, pos+1, length(data)-pos);
	pos = index(data, ",");
	toastoid = substr(data, 1, pos-1);
	data = substr(data, pos+1, length(data)-pos);
	# previous commands already removed the trailing );
	indexoid = data;

	print "declare toast " toastoid " " indexoid " on " tname
}

/^BUILD_INDICES/	{ print "build indices"; }
	
# ----------------
#	CATALOG() definitions take some more work.
# ----------------
/^CATALOG\(/ { 
# ----
#  end any prior catalog data insertions before starting a new one..
# ----
	if (reln_open == 1) {
		print "close " catalog;
		reln_open = 0;
	}

# ----
#  get the name and properties of the new catalog
# ----
	pos = index($1,")");
	catalogandoid = substr($1,9,pos-9);
	pos = index(catalogandoid, ",");
	catalog = substr(catalogandoid, 1, pos-1);
	oid = substr(catalogandoid, pos+1, length(catalogandoid)-pos);

	if ($0 ~ /BKI_BOOTSTRAP/) {
		bootstrap = "bootstrap ";
	}
	if ($0 ~ /BKI_SHARED_RELATION/) {
		shared_relation = "shared_relation ";
	}
	if ($0 ~ /BKI_WITHOUT_OIDS/) {
		without_oids = "without_oids ";
	}

        i = 1;
	inside = 1;
        nc++;
	next;
}

# ----------------
#	process the columns of the catalog definition
#
#	attname[ x ] contains the attribute name for attribute x
#	atttype[ x ] contains the attribute type fot attribute x
# ----------------
inside == 1 {
# ----
#  ignore a leading brace line..
# ----
        if ($1 ~ /\{/)
		next;

# ----
#  if this is the last line, then output the bki catalog stuff.
# ----
	if ($1 ~ /}/) {
		print "create " bootstrap shared_relation without_oids catalog " " oid;
		print "\t(";

		for (j=1; j<i-1; j++) {
			print "\t " attname[ j ] " = " atttype[ j ] " ,";
		}
		print "\t " attname[ j ] " = " atttype[ j ] ;
		print "\t)";

		if (bootstrap == "") {
			print "open " catalog;
		}

		i = 1;
		reln_open = 1;
		inside = 0;
		bootstrap = "";
		shared_relation = "";
		without_oids = "";
		next;
	}

# ----
#  we are inside the catalog definition, so keep sucking up
#  attribute names and types
# ----
	if ($2 ~ /\[.*\]/) {			# array attribute
		idlen = index($2,"[") - 1;
		atttype[ i ] = $1 "[]";		# variable-length only..
		attname[ i ] = substr($2,1,idlen);
	} else {
		atttype[ i ] = $1;
		attname[ i ] = $2;
	}
	i++;
	next;
}

END {
	if (reln_open == 1) {
		print "close " catalog;
		reln_open = 0;
	}
}
' "descriptionfile=${OUTPUT_PREFIX}.description.$$" "shdescriptionfile=${OUTPUT_PREFIX}.shdescription.$$" > $TMPFILE || exit

echo "# PostgreSQL $major_version" >${OUTPUT_PREFIX}.bki.$$

sed -e '/^[ 	]*$/d' \
    -e 's/[ 	][ 	]*/ /g' $TMPFILE >>${OUTPUT_PREFIX}.bki.$$ || exit

#
# Sanity check: if one of the sed/awk/etc commands fails, we'll probably
# end up with a .bki file that is empty or just a few lines.  Cross-check
# that the files are of reasonable size.  The numbers here are arbitrary,
# but are much smaller than the actual expected sizes as of Postgres 7.2.
#
if [ `wc -c < ${OUTPUT_PREFIX}.bki.$$` -lt 100000 ]; then
    echo "$CMDNAME: something seems to be wrong with the .bki file" >&2
    exit 1
fi
if [ `wc -c < ${OUTPUT_PREFIX}.description.$$` -lt 10000 ]; then
    echo "$CMDNAME: something seems to be wrong with the .description file" >&2
    exit 1
fi
if [ `wc -c < ${OUTPUT_PREFIX}.shdescription.$$` -lt 10 ]; then
    echo "$CMDNAME: something seems to be wrong with the .shdescription file" >&2
    exit 1
fi

# Looks good, commit ...

mv ${OUTPUT_PREFIX}.bki.$$ ${OUTPUT_PREFIX}.bki || exit
mv ${OUTPUT_PREFIX}.description.$$ ${OUTPUT_PREFIX}.description || exit
mv ${OUTPUT_PREFIX}.shdescription.$$ ${OUTPUT_PREFIX}.shdescription || exit

exit 0
