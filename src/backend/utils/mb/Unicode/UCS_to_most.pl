#! /usr/bin/perl
#
# Copyright (c) 2001-2006, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/backend/utils/mb/Unicode/UCS_to_most.pl,v 1.2 2006/03/05 15:58:47 momjian Exp $
#
# Generate UTF-8 <--> character code conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain the map files from the organization's ftp site.
# ftp://www.unicode.org/Public/MAPPINGS/
# We assume the file include three tab-separated columns:
#		 source character set code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)

require "ucs2utf.pl";

%filename = (
	'WIN866' => 'CP866.TXT',
	'WIN874' => 'CP874.TXT',
	'WIN1250' => 'CP1250.TXT',
	'WIN1251' => 'CP1251.TXT',
	'WIN1252' => 'CP1252.TXT',
	'WIN1253' => 'CP1253.TXT',
	'WIN1254' => 'CP1254.TXT',
	'WIN1255' => 'CP1255.TXT',
	'WIN1256' => 'CP1256.TXT',
	'WIN1257' => 'CP1257.TXT',
	'WIN1258' => 'CP1258.TXT',
	'ISO8859_2' => '8859-2.TXT',
	'ISO8859_3' => '8859-3.TXT',
	'ISO8859_4' => '8859-4.TXT',
	'ISO8859_5' => '8859-5.TXT',
	'ISO8859_6' => '8859-6.TXT',
	'ISO8859_7' => '8859-7.TXT',
	'ISO8859_8' => '8859-8.TXT',
	'ISO8859_9' => '8859-9.TXT',
	'ISO8859_10' => '8859-10.TXT',
	'ISO8859_13' => '8859-13.TXT',
	'ISO8859_14' => '8859-14.TXT',
	'ISO8859_15' => '8859-15.TXT',
	'ISO8859_16' => '8859-16.TXT',
	'KOI8R' => 'KOI8-R.TXT',
	'GBK' => 'CP936.TXT',
	'UHC' => 'CP949.TXT',
	'JOHAB' => 'JOHAB.TXT',
	'BIG5' => 'BIG5.TXT',
);

@charsets = keys(filename);
foreach $charset (@charsets) {

#
# first, generate UTF8-> charset table
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
		if( $code >= 0x80 && $ucs >= 0x0080){
			$utf = &ucs2utf($ucs);
			if( $array{ $utf } ne "" ){
				printf STDERR "Warning: duplicate UTF8: %04x\n",$ucs;
				next;
			}
			$count++;
			$array{ $utf } = $code;
		}
	}
    close( FILE );

	$file = lc("utf8_to_${charset}.map");
    open( FILE, "> $file" ) || die( "cannot open $file" );
	print FILE "static pg_utf_to_local ULmap${charset}[ $count ] = {\n";

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
# then generate character set code ->UTF8 table
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
		if($code >= 0x80 && $ucs >= 0x0080){
			$utf = &ucs2utf($ucs);
			if( $array{ $code } ne "" ){
				printf STDERR "Warning: duplicate UTF8: %04x\n",$ucs;
				next;
			}
			$count++;
			$array{ $code } = $utf;
		}
	}
    close( FILE );

	$file = lc("${charset}_to_utf8.map");
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
