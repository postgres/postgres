# $Id: ouiparse.awk,v 1.1 2000/08/23 06:02:23 thomas Exp $
#
# ouiparse.awk
# Author: Lawrence E. Rosenman <ler@lerctr.org>
# Original Date: 30 July 2000 (in this form).
# This AWK script takes the IEEE's oui.txt file and creates insert
# statements to populate a SQL table with the following attributes:
# create table oui (
#        oui macaddr primary key,
#        manufacturer text);
# the table name is set by setting the AWK variable TABLE
# 
# we translate the character apostrophe (') to space inside the company name
# to avoid SQL errors.
#
# match ONLY lines that begin with 2 hex numbers, -, and another hex number

BEGIN {
	TABLE="macoui";
	printf "DELETE FROM %s;",TABLE;
	printf "BEGIN TRANSACTION;";
	nrec=0;
}

END {
#	if (nrec > 0)
	printf "COMMIT TRANSACTION;";
}

/^[0-9a-fA-F][0-9a-fA-F]-[0-9a-fA-F]/ { 
#	if (nrec >= 100) {
#		printf "COMMIT TRANSACTION;";
#		printf "BEGIN TRANSACTION;";
#		nrec=0;
#	} else {
#		nrec++;
#	}
	# Get the OUI
	OUI=$1;
	# Skip the (hex) tag to get to Company Name
	Company=$3;
	# make the OUI look like a macaddr
	gsub("-",":",OUI);
	OUI=OUI ":00:00:00"
	# Pick up the rest of the company name
	for (i=4;i<=NF;i++)
		Company=Company " " $i;
	# Modify any apostrophes (') to avoid grief below.
	gsub("'","''",Company);
	# Print out for the 'C' structure in mac.c
	printf "INSERT INTO %s (addr, name) VALUES (trunc(macaddr \'%s\'),\'%s\');\n",
		TABLE,OUI,Company;
}
