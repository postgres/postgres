#! /usr/bin/perl
#
# Copyright (c) 2001-2003, PostgreSQL Global Development Group
#
# $Id: UCS_to_cyrillic.pl,v 1.3 2003/08/04 23:59:39 tgl Exp $
#
# Generate UTF-8 <--> ISO8859 code conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain "8859-[2-5].TXT" from the organization's ftp site.
# We assume the file include three tab-separated columns:
#		 ISO/IEC 8859 code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)

require "ucs2utf.pl";
%filename = ('KOI8R'=>'koi8-r.txt',
             'WIN1251'=>'cp1251.txt',
             'ALT'=>'cp866.txt');
@charsets = ('KOI8R','ALT','WIN1251');
foreach $charset (@charsets) {

#
# first, generate UTF8->ISO8859 table
#
    $in_file = $filename{$charset};

    open( FILE, $in_file ) || die( "cannot open $in_file" );

	reset 'array';

    while( <FILE> ){
		chop;
		if( /^#/ ){
			next;
		}
		( $c, $u, $rest ) = split;
		$ucs = hex($u);
		$code = hex($c);
		if( $code >= 0x80){
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

	$file = "utf8_to_${charset}.map";
    open( FILE, "> $file" ) || die( "cannot open $file" );
	print FILE "static pg_utf_to_local ULmap_${charset}[ $count ] = {\n";

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
# then generate ISO885->UTF8 table
#
    open( FILE, $in_file ) || die( "cannot open $in_file" );

	reset 'array';

    while( <FILE> ){
		chop;
		if( /^#/ ){
			next;
		}
		( $c, $u, $rest ) = split;
		$ucs = hex($u);
		$code = hex($c);
		if($code >= 0x80){
			$utf = &ucs2utf($ucs);
			if( $array{ $utf } ne "" ){
				printf STDERR "Warning: duplicate unicode: %04x\n",$ucs;
				next;
			}
			$count++;
			$array{ $code } = $utf;
		}
	}
    close( FILE );

	$file = "${charset}_to_utf8.map";
    open( FILE, "> $file" ) || die( "cannot open $file" );
	print FILE "static pg_local_to_utf LUmap${charset}[ $count ] = {\n";
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
}
