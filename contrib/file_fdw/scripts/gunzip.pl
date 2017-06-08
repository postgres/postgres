#!/usr/bin/perl

# Decompress the gzipped file at the path specified by the ARGV[0]
# Usage: gunzip.pl /path/to/compressed/file.gz

use strict;

use IO::Uncompress::Gunzip qw(gunzip $GunzipError) ;

gunzip $ARGV[0] => '-' or die "could not decompress: GunzipError\n";
