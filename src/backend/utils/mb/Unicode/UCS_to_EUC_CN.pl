#! /usr/bin/perl
#
# Copyright (c) 2001-2003, PostgreSQL Global Development Group
#
# $Id: UCS_to_EUC_CN.pl,v 1.3 2003/08/04 23:59:39 tgl Exp $
#
# Generate UTF-8 <--> EUC_CN code conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain GB2312.TXT from 
# the organization's ftp site.
#
# GB2312.TXT format:
#		 GB2312 code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)

require "ucs2utf.pl";

# first generate UTF-8 --> EUC_CN table

$in_file = "GB2312.TXT";

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

		$array{ $utf } = ($code | 0x8080);
	}
}
close( FILE );

#
# first, generate UTF8 --> EUC_CN table
#

$file = "utf8_to_euc_cn.map";
open( FILE, "> $file" ) || die( "cannot open $file" );
print FILE "static pg_utf_to_local ULmapEUC_CN[ $count ] = {\n";

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
# then generate EUC_JP --> UTF8 table
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

		$code |= 0x8080;
		$array{ $code } = $utf;
	}
}
close( FILE );

$file = "euc_cn_to_utf8.map";
open( FILE, "> $file" ) || die( "cannot open $file" );
print FILE "static pg_local_to_utf LUmapEUC_CN[ $count ] = {\n";
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
