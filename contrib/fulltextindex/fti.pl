#!/usr/bin/perl
#
# This script substracts all substrings out of a specific column in a table
# and generates output that can be loaded into a new table with the
# psql '\copy' command. The new table should have the following structure:
#
#	create table tab (
#		string text,
#		id oid
#	);
#
# Note that you cannot use 'copy' (the SQL-command) directly, because
# there's no '\.' included at the end of the output.
#
# The output can be fed through the UNIX commands 'uniq' and 'sort'
# to generate the smallest and sorted output to populate the fti-table.
#
# Example:
#
# 	fti.pl -u -d mydb -t mytable -c mycolumn -f myfile
#	sort -o myoutfile myfile
#	uniq myoutfile sorted-file
#
# 	psql -u mydb
#
#		\copy my_fti_table from myfile
#
#  		create index fti_idx on my_fti_table (string,id);
#
#		create function fti() returns opaque as
#			'/path/to/fti/file/fti.so'
#		language 'newC';
#
#		create trigger my_fti_trigger after update or insert or delete
#			on mytable
#				for each row execute procedure fti(my_fti_table, mycolumn);
#
# Make sure you have an index on mytable(oid) to be able to do somewhat
# efficient substring searches.

#use lib '/usr/local/pgsql/lib/perl5/';
use lib '/mnt/web/guide/postgres/lib/perl5/site_perl';
use Pg;
use Getopt::Std;

$PGRES_EMPTY_QUERY    = 0 ;
$PGRES_COMMAND_OK     = 1 ;
$PGRES_TUPLES_OK      = 2 ;
$PGRES_COPY_OUT       = 3 ;
$PGRES_COPY_IN        = 4 ;
$PGRES_BAD_RESPONSE   = 5 ;
$PGRES_NONFATAL_ERROR = 6 ;
$PGRES_FATAL_ERROR    = 7 ;

$[ = 0; # make sure string offsets start at 0

sub break_up {
	my $string = pop @_;

	@strings = split(/\W+/, $string);
	@subs = ();

	foreach $s (@strings) {
		$len = length($s);
		next if ($len < 4);

		$lpos = $len-1;
		while ($lpos >= 3) {
			$fpos = $lpos - 3;
			while ($fpos >= 0) {
				$sub = substr($s, $fpos, $lpos - $fpos + 1);
				push(@subs, $sub);
				$fpos = $fpos - 1;
			}
			$lpos = $lpos - 1;
		}
	}

	return @subs;
}

sub connect_db {
	my $dbname = shift @_;
	my $user   = shift @_;
	my $passwd = shift @_;

	if (!defined($dbname) || $dbname eq "") {
		return 1;
	}
	$connect_string = "dbname=$dbname";

	if ($user ne "") {
		if ($passwd eq "") {
			return 0;
		}
		$connect_string = "$connect_string user=$user password=$passwd ".
		  "authtype=password";
	}
	
	$PG_CONN = PQconnectdb($connect_string);

	if (PQstatus($PG_CONN)) {
		print STDERR "Couldn't make connection with database!\n";
		print STDERR PQerrorMessage($PG_CONN), "\n";
		return 0;
	}

	return 1;
}

sub quit_prog {
	close(OUT);
	unlink $opt_f;
	if (defined($PG_CONN)) {
		PQfinish($PG_CONN);
	}
	exit 1;
}

sub get_username {
	print "Username: ";
	chop($n = <STDIN>);

    return $n;;
}

sub get_password {
	print "Password: ";

	system("stty -echo < /dev/tty");
	chop($pwd = <STDIN>);
	print "\n";
	system("stty echo < /dev/tty");

	return $pwd;
}

sub main {
	getopts('d:t:c:f:u');

	if (!$opt_d || !$opt_t || !$opt_c || !$opt_f) {
		print STDERR "usage: $0 [-u] -d database -t table -c column ".
		  "-f output-file\n";
		return 1;
	}

	if (defined($opt_u)) {
		$uname = get_username();
		$pwd   = get_password();
	} else {
		$uname = "";
		$pwd   = "";
	}

	$SIG{'INT'} = 'quit_prog';
	if (!connect_db($opt_d, $uname, $pwd)) {
		print STDERR "Connecting to database failed!\n";
		return 1;
	}

	if (!open(OUT, ">$opt_f")) {
		print STDERR "Couldnt' open file '$opt_f' for output!\n";
		return 1;
	}

	PQexec($PG_CONN, "begin");

	$query = "declare C cursor for select $opt_c, oid from $opt_t";
	$res = PQexec($PG_CONN, $query);
	if (!$res || (PQresultStatus($res) != $PGRES_COMMAND_OK)) {
		print STDERR "Error declaring cursor!\n";
		print STDERR PQerrorMessage($PG_CONN), "\n";
		PQfinish($PG_CONN);
		return 1;
	}
	PQclear($res);

	$query = "fetch in C";
	while (($res = PQexec($PG_CONN, $query)) &&
		   (PQresultStatus($res) == $PGRES_TUPLES_OK) &&
		   (PQntuples($res) == 1)) {
		$col = PQgetvalue($res, 0, 0);
		$oid = PQgetvalue($res, 0, 1);

		@subs = break_up($col);
		foreach $i (@subs) {
			print OUT "$i\t$oid\n";
		}
	}

	if (!$res || (PQresultStatus($res) != PGRES_TUPLES_OK)) {
		print STDERR "Error retrieving data from backend!\n";
		print STDERR PQerrorMEssage($PG_CONN), "\n";
		PQfinish($PG_CONN);
		return 1;
	}

	PQclear($res);
	PQfinish($PG_CONN);

	return 0;
}

exit main();
