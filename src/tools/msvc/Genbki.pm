#!/usr/bin/perl
#-------------------------------------------------------------------------
#
# Genbki.pm --
#    perl script which generates .bki files from specially formatted .h
#    files.  These .bki files are used to initialize the postgres template
#    database.
#
# Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/tools/msvc/Genbki.pm,v 1.3 2008/01/01 19:46:01 momjian Exp $
#
#-------------------------------------------------------------------------

package Genbki;

use strict;
use warnings;

use Exporter;
our (@ISA, @EXPORT_OK);
@ISA = qw(Exporter);
@EXPORT_OK = qw(genbki);

sub genbki
{
    my $version = shift;
    my $prefix = shift;

    $version =~ /^(\d+\.\d+)/ || die "Bad format verison $version\n";
    my $majorversion = $1;

    my $pgext = read_file("src/include/pg_config_manual.h");
    $pgext =~ /^#define\s+NAMEDATALEN\s+(\d+)$/mg
      || die "Could not read NAMEDATALEN from pg_config_manual.h\n";
    my $namedatalen = $1;

    my $pgauthid = read_file("src/include/catalog/pg_authid.h");
    $pgauthid =~ /^#define\s+BOOTSTRAP_SUPERUSERID\s+(\d+)$/mg
      || die "Could not read BOOTSTRAUP_SUPERUSERID from pg_authid.h\n";
    my $bootstrapsuperuserid = $1;

    my $pgnamespace = read_file("src/include/catalog/pg_namespace.h");
    $pgnamespace =~ /^#define\s+PG_CATALOG_NAMESPACE\s+(\d+)$/mg
      || die "Could not read PG_CATALOG_NAMESPACE from pg_namespace.h\n";
    my $pgcatalognamespace = $1;

    my $indata = "";

    while (@_)
    {
        my $f = shift;
        next unless $f;
        $indata .= read_file($f);
        $indata .= "\n";
    }

    # Strip C comments, from perl FAQ 4.27
    $indata =~ s{/\*.*?\*/}{}gs;

    $indata =~ s{;\s*$}{}gm;
    $indata =~ s{^\s+}{}gm;
    $indata =~ s{^Oid}{oid}gm;
    $indata =~ s{\(Oid}{(oid}gm;
    $indata =~ s{^NameData}{name}gm;
    $indata =~ s{\(NameData}{(name}g;
    $indata =~ s{^TransactionId}{xid}gm;
    $indata =~ s{\(TransactionId}{(xid}g;
    $indata =~ s{PGUID}{$bootstrapsuperuserid}g;
    $indata =~ s{NAMEDATALEN}{$namedatalen}g;
    $indata =~ s{PGNSP}{$pgcatalognamespace}g;

    #print $indata;

    my $bki = "";
    my $desc = "";
    my $shdesc = "";

    my $oid = 0;
    my $catalog = 0;
    my $reln_open = 0;
    my $bootstrap = "";
    my $shared_relation = "";
    my $without_oids = "";
    my $nc = 0;
    my $inside = 0;
    my @attr;
    my @types;

    foreach my $line (split /\n/, $indata)
    {
        if ($line =~ /^DATA\((.*)\)$/m)
        {
            my $data = $1;
            my @fields = split /\s+/,$data;
            if ($#fields >=4 && $fields[0] eq "insert" && $fields[1] eq "OID" && $fields[2] eq "=")
            {
                $oid = $fields[3];
            }
            else
            {
                $oid = 0;
            }
            $data =~ s/\s{2,}/ /g;
            $bki .= $data . "\n";
        }
        elsif ($line =~ /^DESCR\("(.*)"\)$/m)
        {
            if ($oid != 0)
            {
                $desc .= sprintf("%d\t%s\t0\t%s\n", $oid, $catalog, $1);
            }
        }
        elsif ($line =~ /^SHDESCR\("(.*)"\)$/m)
        {
            if ($oid != 0)
            {
                $shdesc .= sprintf("%d\t%s\t%s\n", $oid, $catalog, $1);
            }
        }
        elsif ($line =~ /^DECLARE_(UNIQUE_)?INDEX\((.*)\)$/m)
        {
            if ($reln_open)
            {
                $bki .= "close $catalog\n";
                $reln_open = 0;
            }
            my $u = $1?" unique":"";
            my @fields = split /,/,$2,3;
            $fields[2] =~ s/\s{2,}/ /g;
            $bki .= "declare$u index $fields[0] $fields[1] $fields[2]\n";
        }
        elsif ($line =~ /^DECLARE_TOAST\((.*)\)$/m)
        {
            if ($reln_open)
            {
                $bki .= "close $catalog\n";
                $reln_open = 0;
            }
            my @fields = split /,/,$1;
            $bki .= "declare toast $fields[1] $fields[2] on $fields[0]\n";
        }
        elsif ($line =~ /^BUILD_INDICES/)
        {
            $bki .= "build indices\n";
        }
        elsif ($line =~ /^CATALOG\((.*)\)(.*)$/m)
        {
            if ($reln_open)
            {
                $bki .= "close $catalog\n";
                $reln_open = 0;
            }
            my $rest = $2;
            my @fields = split /,/,$1;
            $catalog = $fields[0];
            $oid = $fields[1];
            $bootstrap=$shared_relation=$without_oids="";
            if ($rest =~ /BKI_BOOTSTRAP/)
            {
                $bootstrap = "bootstrap ";
            }
            if ($rest =~ /BKI_SHARED_RELATION/)
            {
                $shared_relation = "shared_relation ";
            }
            if ($rest =~ /BKI_WITHOUT_OIDS/)
            {
                $without_oids = "without_oids ";
            }
            $nc++;
            $inside = 1;
            next;
        }
        if ($inside==1)
        {
            next if ($line =~ /{/);
            if ($line =~ /}/)
            {

                # Last line
                $bki .= "create $bootstrap$shared_relation$without_oids$catalog $oid\n (\n";
                my $first = 1;
                for (my $i = 0; $i <= $#attr; $i++)
                {
                    if ($first == 1)
                    {
                        $first = 0;
                    }
                    else
                    {
                        $bki .= ",\n";
                    }
                    $bki .= " " . $attr[$i] . " = " . $types[$i];
                }
                $bki .= "\n )\n";
                undef(@attr);
                undef(@types);
                $reln_open = 1;
                $inside = 0;
                if ($bootstrap eq "")
                {
                    $bki .= "open $catalog\n";
                }
                next;
            }

            # inside catalog definition, so keep sucking up attributes
            my @fields = split /\s+/,$line;
            if ($fields[1] =~ /(.*)\[.*\]/)
            { #Array attribute
                push @attr, $1;
                push @types, $fields[0] . '[]';
            }
            else
            {
                push @attr, $fields[1];
                push @types, $fields[0];
            }
            next;
        }
    }
    if ($reln_open == 1)
    {
        $bki .= "close $catalog\n";
    }

    open(O,">$prefix.bki") || die "Could not write $prefix.bki\n";
    print O "# PostgreSQL $majorversion\n";
    print O $bki;
    close(O);
    open(O,">$prefix.description") || die "Could not write $prefix.description\n";
    print O $desc;
    close(O);
    open(O,">$prefix.shdescription") || die "Could not write $prefix.shdescription\n";
    print O $shdesc;
    close(O);
}

sub read_file
{
    my $filename = shift;
    my $F;
    my $t = $/;

    undef $/;
    open($F, $filename) || die "Could not open file $filename\n";
    my $txt = <$F>;
    close($F);
    $/ = $t;

    return $txt;
}

1;
