#! /usr/bin/perl
#
# Copyright (c) 2001-2003, PostgreSQL Global Development Group
#
# $Id: UCS_to_GBK.pl,v 1.2 2003/08/04 23:59:39 tgl Exp $
#
#
# Generate UTF-8 <--> GBK code conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain CP936.TXT from 
# the organization's ftp site.
#
# CP936.TXT format:
#		 GBK code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)

require "ucs2utf.pl";

# first generate UTF-8 --> GBK table

$in_file = "CP936.TXT";

open( FILE, $in_file ) || die( "cannot open $in_file" );

while( <FILE> ){
	chop;
	if( /^#/ ){
		next;
	}
	( $c, $u, $rest ) = split;
	$ucs = hex($u);
	$code = hex($c);
	if( $code >= 0x80 && $ucs >= 0x0080 ){
		$utf = &ucs2utf($ucs);
		if( $array{ $utf } ne "" ){
			printf STDERR "Warning: duplicate unicode: %04x\n",$ucs;
			next;
		}
		$count++;

		$array{ $utf } = $code;
	}
}
close( FILE );

#
# first, generate UTF8 --> WIN949 table
#

$file = "utf8_to_gbk.map";
open( FILE, "> $file" ) || die( "cannot open $file" );
print FILE "static pg_utf_to_local ULmapGBK[ $count ] = {\n";

for $index ( sort {$a <=> $b} keys( %array ) ){
	$code = $array{ $index };
	$count--;
	if( $count == 0 ){
		printf FILE "  {0x%04x, 0x%04x}\n", $index, $code;
	} else {
		printf FILE "  {0x%04x, 0x%04x},\n", $index, $code;
	}
}

print FILE "};\n";
close(FILE);

#
# then generate WIN936 --> UTF8 table
#
reset 'array';

open( FILE, $in_file ) || die( "cannot open $in_file" );

while( <FILE> ){
	chop;
	if( /^#/ ){
		next;
	}
	( $c, $u, $rest ) = split;
	$ucs = hex($u);
	$code = hex($c);
	if( $code >= 0x80 && $ucs >= 0x0080 ){
		$utf = &ucs2utf($ucs);
		if( $array{ $code } ne "" ){
			printf STDERR "Warning: duplicate code: %04x\n",$ucs;
			next;
		}
		$count++;

		$array{ $code } = $utf;
	}
}
close( FILE );

$file = "gbk_to_utf8.map";
open( FILE, "> $file" ) || die( "cannot open $file" );
print FILE "static pg_local_to_utf LUmapGBK[ $count ] = {\n";
for $index ( sort {$a <=> $b} keys( %array ) ){
	$utf = $array{ $index };
	$count--;
	if( $count == 0 ){
		printf FILE "  {0x%04x, 0x%04x}\n", $index, $utf;
	} else {
		printf FILE "  {0x%04x, 0x%04x},\n", $index, $utf;
	}
}

print FILE "};\n";
close(FILE);
