#!/usr/bin/perl

#
# My2Pg: MySQL to PostgreSQL dump conversion utility
#
# (c) 2000,2001 Maxim Rudensky	<fonin@ziet.zhitomir.ua>
# (c) 2000 Valentine Danilchuk	<valdan@ziet.zhitomir.ua>
# All right reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 3. All advertising materials mentioning features or use of this software
#    must display the following acknowledgement:
# This product includes software developed by the Max Rudensky
# and its contributors.
# 4. Neither the name of the author nor the names of its contributors
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
# 
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
#
# $My2pg: my2pg.pl,v 1.27 2001/12/06 19:32:20 fonin Exp $
# $Id: my2pg.pl,v 1.10 2003/01/07 22:18:43 momjian Exp $

#
# $Log: my2pg.pl,v $
# Revision 1.10  2003/01/07 22:18:43  momjian
# Upgrade to my2pg 1.9
#
# Revision 1.27  2002/07/16 14:54:07  fonin
# Bugfix - didn't quote the fields inside PRIMARY KEY with -d option.
# Fix by Milan P. Stanic <mps@rns-nis.co.yu>.
#
# Revision 1.26  2002/07/14 10:30:27  fonin
# Bugfix - MySQL keywords inside data (INSERT INTO sentence) were replaced
# with Postgres keywords and therefore messed up the data.
#
# Revision 1.25  2002/07/05 09:20:25  fonin
# - fixed data that contains two consecutive timestamps - thanks to
#   Ben Darnell <bdarnell@google.com>
# - word 'default' was converted to upper case inside the data - fixed.
#   Thanks to Madsen Wikholm <madsen@iki.fi>
#
# Revision 1.24  2002/04/20 14:15:43  fonin
# Patch by Felipe Nievinski <fnievinski@terra.com.br>.
# A table I was re-creating had a composite primary key, and I was using
# the -d switch to maintain the table and column names
# adding double quotes around them.
#
# The SQL code generated was something like this:
#
# CREATE TABLE "rinav" (
#    "UnidadeAtendimento" INT8 DEFAULT '0' NOT NULL,
#    "NumeroRinav" INT8 DEFAULT '0' NOT NULL,
# -- ...
#    PRIMARY KEY ("UnidadeAtendimento"," NumeroRinav")
# );
#
# Please note the space inside the second column name string in the PK
# definition. Because of this PostgreSQL was not able to create the table.
#
# FIXED.
#
# Revision 1.23  2002/02/07 22:13:52  fonin
# Bugfix by Hans-Juergen Schoenig <hs@cybertec.at>: additional space after
# FLOAT8 is required.
#
# Revision 1.22  2001/12/06 19:32:20  fonin
# Patch: On line 594 where you check for UNIQUE, I believe the regex should try
# and match 'UNIQUE KEY'. Otherwise it outputs no unique indexes for the
# postgres dump.
# Thanks to Brad Hilton <bhilton@vpop.net>
#
# Revision 1.21  2001/08/25 18:55:28  fonin
# Incorporated changes from Yunliang Yu <yu@math.duke.edu>:
# - By default table & column names are not quoted; use the new
#   "-d" option if you want to,
# - Use conditional substitutions to speed up and preserve
#   the data integrity.
# Fixes by Max:
# - timestamps conversion fix. Shouldn't break now matching binary data and
# strings.
#
# Revision 1.21  2001/07/23 03:04:39  yu
# Updates & fixes by Yunliang Yu <yu@math.duke.edu>
# . By default table & column names are not quoted; use the new
#   "-d" option if you want to,
# . Use conditional substitutions to speed up and preserve
#   the data integrity.
#
# Revision 1.20  2001/07/05 12:45:05  fonin
# Timestamp conversion enhancement from Joakim Lemström <jocke@bytewize.com>
#
# Revision 1.19  2001/05/07 19:36:38  fonin
# Fixed a bug in quoting  PRIMARY KEYs, KEYs and UNIQUE indexes with more than 2 columns. Thanks to Jeff Waugh <jaw@ic.net>.
#
# Revision 1.18  2001/03/06 22:25:40  fonin
# Documentation up2dating.
#
# Revision 1.17  2001/03/04 13:01:50  fonin
# Fixes to make work it right with MySQL 3.23 dumps. Tested on mysqldump 8.11.
# Also, AUTO_INCREMENT->SERIAL fields no more have DEFAULT and NOT NULL 
# definitions.
#
# Revision 1.16  2001/02/02 08:15:34  fonin
# Sequences should be created BEFORE creating any objects \nthat depends on it.
#
# Revision 1.15  2001/01/30 10:13:36  fonin
# Re-released under BSD-like license.
#
# Revision 1.14  2000/12/18 20:55:13  fonin
# Better -n implementation.
#
# Revision 1.13  2000/12/18 15:26:33  fonin
# Added command-line options. -n forces *CHAR DEFAULT '' NOT NULL to be 
# converted to *CHAR NULL.
# AUTO_INCREMENT fields converted not in SERIAL but in 
# INT* NOT NULL DEFAULT nextval('seqname').
# Documentation refreshed.
# Dump enclosed in single transaction from now.
#
# Revision 1.12  2000/12/14 20:57:15  fonin
# Doublequotation bug fixed (in CREATE INDEX ON TABLE (field1,field2))
#
# Revision 1.10  2000/11/27 14:18:22  fonin
# Fixed bug - occasionaly was broken CREATE SEQUENCE generation
#
# Revision 1.8  2000/11/24 15:24:16  fonin
# TIMESTAMP fix: MySQL output YYYYMMDDmmhhss to YYYYMMDD mmhhss
#
# Revision 1.7  2000/11/22 23:04:41  fonin
# TIMESTAMP field fix. Better doublequoting. Splitting output dump
# into 2 transactions - create/load/indexing first, sequence setvals then. 
# Added POD documentation.
#
#

use Getopt::Std;

my %opts;		# command line options
my $chareg='';		# CHAR conversion regexps
my $dq=''; # double quote

# parse command line
getopts('nhd',\%opts);

# output syntax
if($opts{h} ne '') {
    usage();
    exit;
}

# convert CHAR types from NOT NULL DEFAULT '' to NULL
if($opts{n} ne '') {
    $chareg='\s*?(default\s*?\'\')*?\s*?not\s*?null';
}
# want double quotes
if($opts{d} ne '') {
    $dq='"';
}

$|=1;

print("------------------------------------------------------------------");
print("\n-- My2Pg 1.27 translated dump");
print("\n--");
print("\n------------------------------------------------------------------");

print("\n\nBEGIN;\n\n\n");

my %index;		# contains array of CREATE INDEX for each table
my %seq;		# contains CREATE SEQUENCE for each table
my %primary;		# contains primary (eg SERIAL) fields for each table
my %identifier;		# contains generated by this program identifiers
my $j=-1;		# current position in $index{table}
my @check;		# CHECK constraint for current

# generating full path to libtypes.c
my $libtypesource='libtypes.c';
my $libtypename=`pwd`;
chomp($libtypename);
$libtypename.='/libtypes.so';

# push header to libtypes.c
open(LIBTYPES,">$libtypesource");
print LIBTYPES "/******************************************************";
print LIBTYPES "\n * My2Pg 1.27 \translated dump";
print LIBTYPES "\n * User types definitions";
print LIBTYPES "\n ******************************************************/";
print LIBTYPES "\n\n#include <postgres.h>\n";
print LIBTYPES "\n#define ADD_COMMA if(strcmp(result,\"\")!=0) strcat(result,\",\")\n";

# reading STDIN...
my $tabledef=0; # we are outside a table definition
while (<>) {

	if(!$tabledef && /^CREATE TABLE \S+/i){
		$tabledef=1;
	} elsif($tabledef && /^\) type=\w*;/i){ # /^\w/i
		$tabledef=0;
	}
	
# Comments start with -- in SQL
    if(/^#/) {# !/insert into.*\(.*#.*\)/i, in mysqldump output
	s/#/--/;
    }

  if($tabledef){##################################
# Convert numeric types
    s/tinyint\(\d+\)/INT2/i;
    s/smallint\(\d+\)/INT2/i;
    s/mediumint\(\d+\)/INT4/i;
    s/bigint\(\d+\)/INT8/i;
    s/int\(\d+\)/INT4/i;
    s/float(\(\d+,\d*\))/DECIMAL$1/i;
    s/double precision/FLOAT8 /i;
    s/([\W])double(\(\d+,\d*\))/$1DECIMAL$2/i;
    s/([\W])double[\W]/$1FLOAT8 /i;
    s/([\W])real[\W]/$1FLOAT8 /i;
    s/([\W])real(\(\d+,\d*\))/$1DECIMAL$2/i;
    
# Convert string types
    s/\w*blob$chareg/text/i;
    s/mediumtext$chareg/text/i;
    s/tinytext$chareg/text/i;
    s/\stext\s+not\s+null/ TEXT DEFAULT '' NOT NULL/i;
    s/(.*?char\(.*?\))$chareg/$1/i;

# Old and New are reserved words in Postgres    
    s/^(\s+)Old /${1}MyOld /;
    s/^(\s+)New /${1}MyNew /;

# Convert DATE types
    s/datetime/TIMESTAMP/;
    s/timestamp\(\d+\)/TIMESTAMP/i;
    s/ date / DATE /i;
    s/,(\d{4})(\d{2})(\d{2}),/,'$1-$2-$3 00:00:00',/g;

# small hack - convert "default" to uppercase, because below we 
# enclose all lowercase words in double quotes
    if(!/^INSERT/) {
	s/default/DEFAULT/;
    }

# Change all AUTO_INCREMENT fields to SERIAL ones with a pre-defined sequence
    if(/([\w\d]+)\sint.*auto_increment/i) {
	$tmpseq=new_name("$table_name"."_"."$+"."_SEQ",28);
	$seq{$table_name}=$tmpseq;
	$primary{$table_name}=$+;
	s/(int.*?) .*AUTO_INCREMENT/$1 DEFAULT nextval\('$tmpseq'\)/i;
	#s/(int.*?)DEFAULT\s*?'.*?'(.*?)AUTO_INCREMENT/$1$2DEFAULT nextval\('$tmpseq'\)/i;
    }

# convert UNSIGNED to CHECK constraints
    if(/^\s+?([\w\d_]+).*?unsigned/i) {
	$check.=",\n  CHECK ($dq$1$dq>=0)";
    }
    s/unsigned//i;

# Limited ENUM support - little heuristic
    s/enum\('N','Y'\)/BOOL/i;
    s/enum\('Y','N'\)/BOOL/i;
# ENUM support
    if(/^\s+?([\w\d_]+).*?enum\((.*?)\)/i) {
	my $enumlist=$2;
	my @item;
	$item[0]='';
	while($enumlist=~s/'([\d\w_]+)'//i) {
	    $item[++$#item]=$1;
	}
# forming identifier name
	$typename=new_name('enum_'.$table_name.'_'.$item[1],28);
#	$typename=lc('enum_'.$table_name.'_'.$item[1]);
# creating input type function
	my $func_in="
int2* $typename"."_in (char *str) {
    int2* result;

    if(str==NULL)
	return NULL;
    
    result=(int2*)palloc(sizeof(int2));
    *result=-1;";
	for(my $i=0;$i<=$#item;$i++) {
	    $func_in.="
    if(strcmp(str,\"$item[$i]\")==0) {
	*result=$i;
    }";
	}
	$func_in.="
    if(*result == -1) {
	elog(ERROR,\"$typename"."_in: incorrect input value\");
	return NULL;
    }
    return (result);
}\n";
	$types.="\n---";
	$types.="\n--- Types for table ".uc($table_name);
	$types.="\n---\n";
	print LIBTYPES "\n/*";
	print LIBTYPES "\n * Types for table ".uc($table_name);
	print LIBTYPES "\n */\n";

	$types.="\nCREATE FUNCTION $typename"."_in (opaque)
	RETURNS $typename
	AS '$libtypename'
	LANGUAGE 'c'
	WITH (ISCACHABLE);\n";

# creating output function
	my $func_out="
char* $typename"."_out (int2 *outvalue) {
    char* result;

    if(outvalue==NULL)
	return NULL;

    result=(char*)palloc(10);
    switch (*outvalue) {";
	for(my $i=0;$i<=$#item;$i++) {
	    $func_out.="
	case $i:
	    strcpy(result,\"$item[$i]\");
	    break;";
	}
	$func_out.="
	default	 :
	    elog(ERROR,\"$typename"."_out: incorrect stored value\");
	    return NULL;
	    break;
    }
    return result;
}\n";
	$func_out.="\nbool $typename"."_eq(int2* a, int2* b) {
    return (*a==*b);
}

bool $typename"."_ne(int2* a, int2* b) {
    return (*a!=*b);
}

bool $typename"."_lt(int2* a, int2* b) {
    return (*a<*b);
}

bool $typename"."_le(int2* a, int2* b) {
    return (*a<=*b);
}

bool $typename"."_gt(int2* a, int2* b) {
    return (*a>*b);
}

bool $typename"."_ge(int2* a, int2* b) {
    return (*a>=*b);
}\n";

	$types.="\nCREATE FUNCTION $typename"."_out (opaque)
	RETURNS opaque
	AS '$libtypename'
	LANGUAGE 'c'
	WITH (ISCACHABLE);\n";

	$types.="\nCREATE TYPE $typename (
	internallength = 2,
	input = $typename\_in,
	output = $typename\_out
);\n";

	$types.="\nCREATE FUNCTION $typename"."_eq ($typename,$typename)
	RETURNS bool
	AS '$libtypename'
	LANGUAGE 'c';

CREATE FUNCTION $typename"."_lt ($typename,$typename)
	RETURNS bool
	AS '$libtypename'
	LANGUAGE 'c';

CREATE FUNCTION $typename"."_le ($typename,$typename)
	RETURNS bool
	AS '$libtypename'
	LANGUAGE 'c';

CREATE FUNCTION $typename"."_gt ($typename,$typename)
	RETURNS bool
	AS '$libtypename'
	LANGUAGE 'c';

CREATE FUNCTION $typename"."_ge ($typename,$typename)
	RETURNS bool
	AS '$libtypename'
	LANGUAGE 'c';

CREATE FUNCTION $typename"."_ne ($typename,$typename)
	RETURNS bool
	AS '$libtypename'
	LANGUAGE 'c';

CREATE OPERATOR < (
	leftarg = $typename,
	rightarg = $typename,
--	negator = >=,
	procedure = $typename"."_lt
);

CREATE OPERATOR <= (
	leftarg = $typename,
	rightarg = $typename,
--	negator = >,
	procedure = $typename"."_le
);

CREATE OPERATOR = (
	leftarg = $typename,
	rightarg = $typename,
	commutator = =,
--	negator = <>,
	procedure = $typename"."_eq
);

CREATE OPERATOR >= (
	leftarg = $typename,
	rightarg = $typename,
	negator = <,
	procedure = $typename"."_ge
);

CREATE OPERATOR > (
	leftarg = $typename,
	rightarg = $typename,
	negator = <=,
	procedure = $typename"."_gt
);

CREATE OPERATOR <> (
	leftarg = $typename,
	rightarg = $typename,
	negator = =,
	procedure = $typename"."_ne
);\n";

	print LIBTYPES $func_in;
	print LIBTYPES $func_out;
	s/enum\(.*?\)/$typename/i;
    }

# SET support
    if(/^\s+?([\w\d_]+).*?set\((.*?)\)/i) {
	my $setlist=$2;
	my @item;
	$item[0]='';
	my $maxlen=0;	# maximal string length
	while($setlist=~s/'([\d\w_]+)'//i) {
	    $item[++$#item]=$1;
	    $maxlen+=length($item[$#item])+1;
	}
	$maxlen+=1;
	my $typesize=int($#item/8);
	if($typesize<2) {
	    $typesize=2;
	}
	$internalsize=$typesize;
	$typesize='int'.$typesize;
#	$typename=lc('set_'.$table_name.'_'.$item[1]);
	$typename=new_name('set_'.$table_name.'_'.$item[1],28);
# creating input type function
	my $func_in="
$typesize* $typename"."_in (char *str) {
    $typesize* result;
    char* token;

    if(str==NULL)
	return NULL;

    result=($typesize*)palloc(sizeof($typesize));
    *result=0;
    if(strcmp(str,\"\")==0)
	return result;
    for(token=strtok(str,\",\");token!=NULL;token=strtok(NULL,\",\")) {";
	for(my $i=0,my $j=1;$i<=$#item;$i++,$j*=2) {
	    $func_in.="
	if(strcmp(token,\"$item[$i]\")==0) {
	    *result|=$j;
	    continue;
	}";
	}
	$func_in.="
    }

    if(*result == 0) {
	elog(ERROR,\"$typename"."_in: incorrect input value\");
	return NULL;
    }
    return (result);

}\n";
	$types.="\n---";
	$types.="\n--- Types for table ".uc($table_name);
	$types.="\n---\n";
	print LIBTYPES "\n/*";
	print LIBTYPES "\n * Types for table ".uc($table_name);
	print LIBTYPES "\n */\n";

	$types.="\nCREATE FUNCTION $typename"."_in (opaque)
	RETURNS $typename
	AS '$libtypename'
	LANGUAGE 'c';\n";

# creating output function
	my $func_out="
char* $typename"."_out ($typesize *outvalue) {
    char* result;
    int i;

    if(outvalue==NULL)
	return NULL;

    result=(char*)palloc($maxlen);
    strcpy(result,\"\");
    for(i=1;i<=2 << (sizeof(int2)*8);i*=2) {
	switch (*outvalue & i) {";
	for(my $i=0,$j=1;$i<=$#item;$i++,$j*=2) {
	    $func_out.="
	case $j:";
	    if($item[$i] ne '') {
		$func_out.="ADD_COMMA;";
	    }
	    $func_out.="strcat(result,\"$item[$i]\");
	    break;";
	}
	$func_out.="
	default	 :
	    break;
	}
    }
    
    return result;
}\n";
	$func_out.="\nbool $typename"."_eq($typesize* a, $typesize* b) {
    return (*a==*b);
}

$typesize find_in_set($typesize *a, $typesize *b) {
    int i;
    
    for(i=1;i<=sizeof($typesize)*8;i*=2) {
	if(*a & *b) {
	    return 1;
	}
    }
    return 0;
}

\n";

	$types.="\nCREATE FUNCTION $typename"."_out (opaque)
	RETURNS opaque
	AS '$libtypename'
	LANGUAGE 'c';\n";

	$types.="\nCREATE TYPE $typename (
	internallength = $internalsize,
	input = $typename\_in,
	output = $typename\_out
);\n";

	$types.="\nCREATE FUNCTION $typename"."_eq ($typename,$typename)
	RETURNS bool
	AS '$libtypename'
	LANGUAGE 'c';

CREATE FUNCTION find_in_set ($typename,$typename)
	RETURNS bool
	AS '$libtypename'
	LANGUAGE 'c';

CREATE OPERATOR = (
	leftarg = $typename,
	rightarg = $typename,
	commutator = =,
	procedure = $typename"."_eq
);

CREATE OPERATOR <> (
	leftarg = $typename,
	rightarg = $typename,
	commutator = <>,
	negator = =,
	procedure = $typename"."_eq
);

\n";

	print LIBTYPES $func_in;
	print LIBTYPES $func_out;
	s/set\(.*?\)/$typename/i;
    }

# Change multy-field keys to multi-field indices
# MySQL Dump usually ends the CREATE TABLE statement like this:
# CREATE TABLE bids (
#   ...
#   PRIMARY KEY (bids_id),
#   KEY offer_id (offer_id,user_id,the_time),
#   KEY bid_value (bid_value)
# );
# We want to replace this with smth like
# CREATE TABLE bids (
#   ...
#   PRIMARY KEY (bids_id),
# );
#   CREATE INDEX offer_id ON bids (offer_id,user_id,the_time);
#   CREATE INDEX bid_value ON bids (bid_value);
    if (s/CREATE TABLE (.*) /CREATE TABLE $dq$1$dq /i) {
	if($oldtable ne $table_name) {
	    $oldtable=$table_name;
	    $j=-1;
	    $check='';

	    if($seq{$table_name} ne '') {
		print "\n\n--";
		print "\n-- Sequences for table ".uc($table_name);
		print "\n--\n";
		print "\nCREATE SEQUENCE ".$seq{$table_name}.";\n\n";
	    }

	    print $types;
	    $types='';
	    $dump=~s/,\n\).*;/\n\);/gmi;
# removing table options after closing bracket:
# ) TYPE=ISAM PACK_KEYS=1;
	    $dump=~s/\n\).*/\n\);/gmi;
	    print $dump;
	    $dump='';
	}
	$table_name=$1;
    }

# output CHECK constraints instead UNSIGNED modifiers
    if(/PRIMARY KEY\s+\((.*)\)/i) {
	my $tmpfld=$1;
	$tmpfld=~s/,/","/g if $dq;
	$tmpfld=~s/ //g;
	s/PRIMARY KEY\s+(\(.*\))/PRIMARY KEY \($dq$tmpfld$dq\)/i;
	s/(PRIMARY KEY \(.*\)).*/$1$check\n/i;
    }
    
    if(/^\s*KEY ([\w\d_]+)\s*\((.*)\).*/i) {
	my $tmpfld=$2; my $ky=$1;
	$tmpfld=~s/\s*,\s*/","/g if $dq;
	$index{$table_name}[++$j]="CREATE INDEX ${ky}_$table_name\_index ON $dq$table_name$dq ($dq$tmpfld$dq);";
    }
    if(/^\s*UNIQUE.*?([\w\d_]+)\s*\((.*)\).*/i) {
	my $tmpfld=$2; my $ky=$1;
	$tmpfld=~s/,/","/g if $dq;
	$index{$table_name}[++$j]="CREATE UNIQUE INDEX ${ky}_$table_name\_index ON $dq$table_name$dq ($dq$tmpfld$dq);";
    }
    s/^\s*UNIQUE (.+).*(\(.*\)).*\n//i;
    s/^\s*KEY (.+).*(\(.*\)).*\n//i;
    
    if($dq && !/^\s*(PRIMARY KEY|UNIQUE |KEY |CREATE TABLE |\);)/i){
    	s/\s([A-Za-z_\d]+)\s/ $dq$+$dq /;
    }
  } ####if($tabledef)###############################

    if($dq && !s/INSERT INTO\s+?(.*?)\s+?/INSERT INTO $dq$1$dq /i) {
# Quote lowercase identifiers in double quotes
	#while(!/^--/ && s/\s([A-Za-z_\d]+[a-z][A-Za-z_\d]*)\s/ $dq$+$dq /) {;}
    }

# Fix timestamps
    s/'0000-00-00/'0001-01-01/g;
# may work wrong !!!
    s/([,(])00000000000000(?=[,)])/$1'00010101 000000'/g;
    s/([,(])(\d{8})(\d{6})(?=[,)])/$1'$2 $3'/g;
    s/([,(])(\d{4})(\d{2})(\d{2})(?=[,)])/$1'$2-$3-$4 00:00:00'/g;
#<Hackzone> ---------------------------------------------------
#</Hackzone> --------------------------------------------------
    $dump.=$_;
}

if($seq{$table_name} ne '') {
    print "\n\n--";
    print "\n-- Sequences for table ".uc($table_name);
    print "\n--\n";
    print "\nCREATE SEQUENCE ".$seq{$table_name}.";\n\n";
}
print $types;
$dump=~s/,\n\).*;/\n\);/gmi;
$dump=~s/\n\).*/\n\);/gmi;
print $dump;

# Output indices for tables
while(my($table,$ind)=each(%index)) {
    print "\n\n--";
    print "\n-- Indexes for table ".uc($table);
    print "\n--\n";
    for(my $i=0;$i<=$#{$ind};$i++) {
	print "\n$ind->[$i]";
    }

}

while(my($table,$s)=each(%seq)) {
    print "\n\n--";
    print "\n-- Sequences for table ".uc($table);
    print "\n--\n";

    # setting SERIAL sequence values right    
    if($primary{$table} ne '') {
	print "\nSELECT SETVAL('".$seq{$table}."',(select case when max($dq".$primary{$table}."$dq)>0 then max($dq".$primary{$table}."$dq)+1 else 1 end from $dq$table$dq));";
    }
}

print("\n\nCOMMIT;\n");
close(LIBTYPES);

open(MAKE,">Makefile");
print MAKE "#
# My2Pg \$Revision: 1.10 $ \translated dump
# Makefile
#

all: libtypes.so

libtypes.o: libtypes.c
	gcc -c -fPIC -g -O libtypes.c
libtypes.so: libtypes.o
	ld -Bshareable -o libtypes.so libtypes.o";
close(MAKE);

#
# Function generates unique identifier
# Args   : template name, max length
# Globals: %identifier
#
sub new_name() {
    my $name=lc(shift @_);
    my $len=shift @_;

# truncate long names
    if(length($name)>$len) {
	$name=~s/(.{$len}).*/$1/i;
    }

# find reserved identifiers
    if($identifier{$name}!=1) {
    	$identifier{$name}=1;
	return $name;
    }
    else {
	for(my $i=1,my $tmpname=$name.$i;$identifier{$tmpname}!=1;) {
	    $tmpname=$name.$i
	}
	$identifier{$tmpname}=1;
	return $tmpname;
    }

    die "Error during unique identifier generation :-(";
}

sub usage() {
print <<EOF
my2pg - MySQL to PostgreSQL database dump converter

Copyright (c) 2000 Max Rudensky		<fonin\@ziet.zhitomir.ua>
Copyright (c) 2000 Valentine Danilchuk	<valdan\@ziet.zhitomir.ua>

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
code source for license details.

SYNTAX:
    my2pg [-hnd]

OPTIONS:
    h		- this help
    n		- convert *CHAR NOT NULL DEFAULT '' types to *CHAR NULL
    d       - double quotes around table and column names
EOF
;
}


=head1 NAME

my2pg - MySQL -> PostgreSQL dump conversion utility.

=head1 SYNTAX

	mysqldump db | ./my2pg.pl [-n] > pgsqldump.sql
	vi libtypes.c
	make
	psql database < pgsqldump.txt
where

=over 4

=item B<pgsqldump.sql>

- file suitable for loading into PostgreSQL.

=item B<libtypes.c>

- C source for emulated MySQL types (ENUM, SET) generated by B<my2pg>

=back

=head1 OVERVIEW

B<my2pg> utility attempts to convert MySQL database dump to Postgres's one.
B<my2pg> performs such conversions:

=over 4

=item Type conversion.

It tries to find proper Postgres 
type for each column.
Unknown types are silently pushing to output dump;
ENUM and SET types implemented via user types 
(C source for such types can be found in 
B<libtypes.c> file);

=item Identifiers double-quotation.

All column and table 
names should be enclosed to double-quotes to prevent 
interferension with reserved words;

=item Converting

AUTO_INCREMENT fields to SERIAL. Actually, creating the sequence and 
setting default value to nextval('seq'), well, you know :)

=item Converting

KEY(field) to CREATE INDEX i_field on table (field);

=item The same

for UNIQUE keys;

=item Indices

are creating AFTER rows insertion (to speed up the load);

=item Translates '#'

MySQL comments to ANSI SQL '--'

=back

It encloses dump in transaction block to prevent single errors 
during data load.

=head1 COMMAND-LINE OPTIONS

My2pg takes the following command-line options:

=over 2

=item -n

Convert *CHAR DEFAULT '' NOT NULL types to *CHAR NULL.
Postgres can't load empty '' strings in NOT NULL fields.

=item -d

Add double quotes around table and column names

=item -h

Show usage banner.

=back

=head1 SIDE EFFECTS

=over 4

=item creates

file B<libtypes.c> in current directory 
overwriting existed file without any checks;

=item the same

for Makefile.

=back

=head1 BUGS

This program is still beta. Testers wanted.
Known bugs are:

=over 4

=item Poor doublequotation.

All identifiers such as table and column names should be enclosed in double 
quotes. Program can't handle upper-case identifiers, 
like DBA. Lower-case identifiers are OK.

=item SET type emulation is not full. LIKE operation on 

SETs, raw integer input values should be implemented

=item B<Makefile> generated during output is 
platform-dependent and surely works only on 
Linux/gcc (FreeBSD/gcc probably works as well - not tested)

=item Generated B<libtypes.c> contain line

	#include <postgres.h>

This file may be located not in standard compiler 
include path, you need to check it before compiling.

=back

=head1 AUTHORS

B<(c) 2000 Maxim V. Rudensky	 (fonin@ziet.zhitomir.ua)>
B<(c) 2000 Valentine V. Danilchuk (valdan@ziet.zhitomir.ua)>

=head1 CREDITS

Jeff Waugh <jaw@ic.net>
Joakim Lemström <jocke@bytewize.com> || <buddyh19@hotmail.com>
Yunliang Yu <yu@math.duke.edu>
Brad Hilton <bhilton@vpop.net>

=head1 LICENSE

B<BSD>

=cut
