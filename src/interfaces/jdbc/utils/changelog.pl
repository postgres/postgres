#!/bin/perl

while(<>) {
    chomp();
    s/\t+/ /g;
    if(substr($_,0,3) eq ' - ') {
	print "<ul>" if !$inlist;
	$inlist=1;
	print "<li>".substr($_,3)."\n";
    } else {
	if($_ eq "" || $_ eq " ") {
	    print "</ul>" if $inlist;
	    $inlist=0;
	    print "<br>\n";
	} elsif(substr($_,0,1) eq " ") {
	    print $_;
	} else {
	    print "</ul>" if $inlist;
	    $inlist=0;
	    print "<h4>".$_."</h4>\n";
	}
    }
}
