# -*- perl -*-
# RServ.pm
# Vadim Mikheev, (c) 2000, PostgreSQL Inc.

package RServ;

require Exporter;
@ISA = qw(Exporter);
@EXPORT = qw(PrepareSnapshot ApplySnapshot GetSyncID SyncSyncID CleanLog);
@EXPORT_OK = qw();

use Pg;

$debug = 0;
$quiet = 1;

my %Mtables = ();
my %Stables = ();

sub PrepareSnapshot
{
	my ($conn, $outf, $server) = @_; # (@_[0], @_[1], @_[2]);

	my $result = $conn->exec("BEGIN");
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}
	$result = $conn->exec("set transaction isolation level serializable");
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	# MAP oid --> tabname, keyname
	$result = $conn->exec("select pgc.oid, pgc.relname, pga.attname" . 
						  " from _RSERV_TABLES_ rt, pg_class pgc, pg_attribute pga" . 
						  " where pgc.oid = rt.reloid and pga.attrelid = rt.reloid" .
						  " and pga.attnum = rt.key");
	if ($result->resultStatus ne PGRES_TUPLES_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	my @row;
	while (@row = $result->fetchrow)
	{
	#	printf "$row[0], $row[1], $row[2]\n";
		push @{$Mtables{$row[0]}}, $row[1], $row[2];
	}

	# Read last succeeded sync
	$sql = "select syncid, synctime, minid, maxid, active from _RSERV_SYNC_" . 
		" where server = $server and syncid = (select max(syncid) from" . 
			" _RSERV_SYNC_ where server = $server and status > 0)";

	printf "$sql\n" if $debug;

	$result = $conn->exec($sql);
	if ($result->resultStatus ne PGRES_TUPLES_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	my @lastsync = $result->fetchrow;

	my $sinfo = "";
	if ($lastsync[3] ne '')	# sync info
	{
		$sinfo = "and (l.logid >= $lastsync[3]";
		$sinfo .= " or l.logid in ($lastsync[4])" if $lastsync[4] ne '';
		$sinfo .= ")";
	}

	my $havedeal = 0;

	# DELETED rows
	$sql = "select l.reloid, l.key from _RSERV_LOG_ l" .
		" where l.deleted = 1 $sinfo order by l.reloid";

	printf "$sql\n" if $debug;

	$result = $conn->exec($sql);
	if ($result->resultStatus ne PGRES_TUPLES_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	$lastoid = '';
	while (@row = $result->fetchrow)
	{
		next unless exists $Mtables{$row[0]};
		if ($lastoid != $row[0])
		{
			if ($lastoid eq '')
			{
				my $syncid = GetSYNCID($conn, $outf);
				return($syncid) if $syncid < 0;
				$havedeal = 1;
			}
			else
			{
				printf $outf "\\.\n";
			}
			printf $outf "-- DELETE $Mtables{$row[0]}[0]\n";
			$lastoid = $row[0];
		}
		if (! defined $row[1])
		{
			print STDERR "NULL key\n" unless ($quiet);
			$conn->exec("ROLLBACK");
			return(-2);
		}
		printf $outf "%s\n", OutputValue($row[1]);
	}
	printf $outf "\\.\n" if $lastoid ne '';

	# UPDATED rows

	my ($taboid, $tabname, $tabkey);
	foreach $taboid (keys %Mtables)
	{
		($tabname, $tabkey) = @{$Mtables{$taboid}};
		my $oidkey = ($tabkey eq 'oid') ? "_$tabname.oid," : '';
		$sql = sprintf "select $oidkey _$tabname.* from _RSERV_LOG_ l," .
			" $tabname _$tabname where l.reloid = $taboid and l.deleted = 0 $sinfo" .
				" and l.key = _$tabname.${tabkey}::text";

		printf "$sql\n" if $debug;

		$result = $conn->exec($sql);
		if ($result->resultStatus ne PGRES_TUPLES_OK)
		{
			printf $outf "-- ERROR\n" if $havedeal;
			print STDERR $conn->errorMessage unless ($quiet);
			$conn->exec("ROLLBACK");
			return(-1);
		}
		next if $result->ntuples <= 0;
		if (! $havedeal)
		{
			my $syncid = GetSYNCID($conn, $outf);
			return($syncid) if $syncid < 0;
			$havedeal = 1;
		}
		printf $outf "-- UPDATE $tabname\n";
		while (@row = $result->fetchrow)
		{
			for ($i = 0; $i <= $#row; $i++)
			{
				printf $outf "	" if $i;
				printf $outf "%s", OutputValue($row[$i]);
			}
			printf $outf "\n";
		}
		printf $outf "\\.\n";
	}

	unless ($havedeal)
	{
		$conn->exec("ROLLBACK");
		return(0);
	}

	# Remember this snapshot info
	$result = $conn->exec("select _rserv_sync_($server)");
	if ($result->resultStatus ne PGRES_TUPLES_OK)
	{
		printf $outf "-- ERROR\n";
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	$result = $conn->exec("COMMIT");
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		printf $outf "-- ERROR\n";
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}
	printf $outf "-- OK\n";

	return(1);

}

sub OutputValue
{
	my ($val) = @_; # @_[0];

	return("\\N") unless defined $val;

	$val =~ s/\\/\\\\/g;
	$val =~ s/	/\\011/g;
	$val =~ s/\n/\\012/g;
	$val =~ s/\'/\\047/g;

	return($val);
}

# Get syncid for new snapshot
sub GetSYNCID
{
	my ($conn, $outf) = @_; # (@_[0], @_[1]);

	my $result = $conn->exec("select nextval('_rserv_sync_seq_')");
	if ($result->resultStatus ne PGRES_TUPLES_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	my @row = $result->fetchrow;

	printf $outf "-- SYNCID $row[0]\n";
	return($row[0]);
}


sub CleanLog
{
	my ($conn, $howold) = @_; # (@_[0], @_[1]);

	my $result = $conn->exec("BEGIN");
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	my $sql = "select rs.maxid, rs.active from _RSERV_SYNC_ rs" .
		" where rs.syncid = (select max(rs2.syncid) from _RSERV_SYNC_ rs2" .
			" where rs2.server = rs.server and rs2.status > 0) order by rs.maxid";

	printf "$sql\n" if $debug;

	$result = $conn->exec($sql);
	if ($result->resultStatus ne PGRES_TUPLES_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		return(-1);
	}
	my $maxid = '';
	my %active = ();
	while (my @row = $result->fetchrow)
	{
		$maxid = $row[0] if $maxid eq '';
		last if $row[0] > $maxid;
		my @ids = split(/[ 	]+,[ 	]+/, $row[1]);
		foreach $aid (@ids)
		{
			$active{$aid} = 1 unless exists $active{$aid};
		}
	}
	if ($maxid eq '')
	{
		print STDERR "No Sync IDs\n" unless ($quiet);
		return(0);
	}
	my $alist = join(',', keys %active);
	my $sinfo = "logid < $maxid";
	$sinfo .= " and logid not in ($alist)" if $alist ne '';
	
	$sql = "delete from _RSERV_LOG_ where " . 
		"logtime < now() - '$howold second'::interval and $sinfo";

	printf "$sql\n" if $debug;

	$result = $conn->exec($sql);
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}
	$maxid = $result->cmdTuples;

	$result = $conn->exec("COMMIT");
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	return($maxid);
}

sub ApplySnapshot
{
	my ($conn, $inpf) = @_; # (@_[0], @_[1]);

	my $result = $conn->exec("BEGIN");
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	$result = $conn->exec("SET CONSTRAINTS ALL DEFERRED");
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	# MAP name --> oid, keyname, keynum
	my $sql = "select pgc.oid, pgc.relname, pga.attname, rt.key" . 
		" from _RSERV_SLAVE_TABLES_ rt, pg_class pgc, pg_attribute pga" . 
			" where pgc.oid = rt.reloid and pga.attrelid = rt.reloid" .
				" and pga.attnum = rt.key";
	$result = $conn->exec($sql);
	if ($result->resultStatus ne PGRES_TUPLES_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	while (@row = $result->fetchrow)
	{
	#	printf "	%s	%s\n", $row[1], $row[0];
		push @{$Stables{$row[1]}}, $row[0], $row[2], $row[3];
	}

	my $ok = 0;
	my $syncid = '';
	while(<$inpf>)
	{
		$_ =~ s/\n//;
		my ($cmt, $cmd, $prm) = split (/[ 	]+/, $_, 3);
		if ($cmt ne '--')
		{
			printf STDERR "Invalid format\n" unless ($quiet);
			$conn->exec("ROLLBACK");
			return(-2);
		}
		if ($cmd eq 'DELETE')
		{
			if ($syncid eq '')
			{
				printf STDERR "Sync ID unspecified\n" unless ($quiet);
				$conn->exec("ROLLBACK");
				return(-2);
			}
			$result = DoDelete($conn, $inpf, $prm);
			if ($result)
			{
				$conn->exec("ROLLBACK");
				return($result);
			}
		}
		elsif ($cmd eq 'UPDATE')
		{
			if ($syncid eq '')
			{
				printf STDERR "Sync ID unspecified\n" unless ($quiet);
				$conn->exec("ROLLBACK");
				return(-2);
			}
			$result = DoUpdate($conn, $inpf, $prm);
			if ($result)
			{
				$conn->exec("ROLLBACK");
				return($result);
			}
		}
		elsif ($cmd eq 'SYNCID')
		{
			if ($syncid ne '')
			{
				printf STDERR "Second Sync ID ?!\n" unless ($quiet);
				$conn->exec("ROLLBACK");
				return(-2);
			}
			if ($prm !~ /^\d+$/)
			{
				printf STDERR "Invalid Sync ID $prm\n" unless ($quiet);
				$conn->exec("ROLLBACK");
				return(-2);
			}
			$syncid = $prm;

			printf STDERR "Sync ID $syncid\n" unless ($quiet);

			$result = $conn->exec("select syncid, synctime from " . 
								  "_RSERV_SLAVE_SYNC_ where syncid = " . 
								  "(select max(syncid) from _RSERV_SLAVE_SYNC_)");
			if ($result->resultStatus ne PGRES_TUPLES_OK)
			{
				print STDERR $conn->errorMessage unless ($quiet);
				$conn->exec("ROLLBACK");
				return(-1);
			}
			my @row = $result->fetchrow;
			if (! defined $row[0])
			{
				$result = $conn->exec("insert into" .
									  " _RSERV_SLAVE_SYNC_(syncid, synctime) values ($syncid, now())");
			}
			elsif ($row[0] >= $prm)
			{
				printf STDERR "Sync-ed to ID $row[0] ($row[1])\n" unless ($quiet);
				$conn->exec("ROLLBACK");
				return(0);
			}
			else
			{
				$result = $conn->exec("update _RSERV_SLAVE_SYNC_" .
									  " set syncid = $syncid, synctime = now()");
			}
			if ($result->resultStatus ne PGRES_COMMAND_OK)
			{
				print STDERR $conn->errorMessage unless ($quiet);
				$conn->exec("ROLLBACK");
				return(-1);
			}
		}
		elsif ($cmd eq 'OK')
		{
			$ok = 1;
			last;
		}
		elsif ($cmd eq 'ERROR')
		{
			printf STDERR "ERROR signaled\n" unless ($quiet);
			$conn->exec("ROLLBACK");
			return(-2);
		}
		else
		{
			printf STDERR "Unknown command $cmd\n" unless ($quiet);
			$conn->exec("ROLLBACK");
			return(-2);
		}
	}

	if (! $ok)
	{
		printf STDERR "No OK flag in input\n" unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-2);
	}

	$result = $conn->exec("COMMIT");
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	return(1);
}

sub DoDelete
{
	my ($conn, $inpf, $tabname) = @_; # (@_[0], @_[1], @_[2]);

	my $ok = 0;
	while(<$inpf>)
	{
		if ($_ !~ /\n$/)
		{
			printf STDERR "Invalid format\n" unless ($quiet);
			return(-2);
		}
		my $key = $_;
		$key =~ s/\n//;
		if ($key eq '\.')
		{
			$ok = 1;
			last;
		}

		my $sql = "delete from $tabname where $Stables{$tabname}->[1] = '$key'";

		printf "$sql\n" if $debug;

		my $result = $conn->exec($sql);
		if ($result->resultStatus ne PGRES_COMMAND_OK)
		{
			print STDERR $conn->errorMessage unless ($quiet);
			return(-1);
		}
	}

	if (! $ok)
	{
		printf STDERR "No end of input in DELETE section\n" unless ($quiet);
		return(-2);
	}

	return(0);
}


sub DoUpdate
{
	my ($conn, $inpf, $tabname) = @_; # (@_[0], @_[1], @_[2]);
	my $oidkey = ($Stables{$tabname}->[2] < 0) ? 1 : 0;

	my @CopyBuf = ();
	my $CBufLen = 0;
	my $CBufMax = 16 * 1024 * 1024;	# max size of buf for copy

	my $sql = "select attnum, attname from pg_attribute" .
		" where attrelid = $Stables{$tabname}->[0] and attnum > 0";

	my $result = $conn->exec($sql);
	if ($result->resultStatus ne PGRES_TUPLES_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		return(-1);
	}

	my @anames = ();
	while (@row = $result->fetchrow)
	{
		$anames[$row[0]] = $row[1];
	}

	my $istring;
	my $ok = 0;
	while(<$inpf>)
	{
		if ($_ !~ /\n$/)
		{
			printf STDERR "Invalid format\n" unless ($quiet);
			return(-2);
		}
		$istring = $_;
		$istring =~ s/\n//;
		if ($istring eq '\.')
		{
			$ok = 1;
			last;
		}
		my @vals = split(/	/, $istring);
		if ($oidkey)
		{
			if ($vals[0] !~ /^\d+$/ || $vals[0] <= 0)
			{
				printf STDERR "Invalid OID\n" unless ($quiet);
				return(-2);
			}
			$oidkey = $vals[0];
		}
		else
		{
			unshift @vals, '';
		}

		$sql = "update $tabname set ";
		my $ocnt = 0;
		for (my $i = 1; $i <= $#anames; $i++)
		{
			if ($vals[$i] eq '\N')
			{
				if ($i == $Stables{$tabname}->[2])
				{
					printf STDERR "NULL key\n" unless ($quiet);
					return(-2);
				}
				$vals[$i] = 'null';
			}
			else
			{
				$vals[$i] = "'" . $vals[$i] . "'";
				next if $i == $Stables{$tabname}->[2];
			}
			$ocnt++;
			$sql .= ', ' if $ocnt > 1;
			$sql .= "$anames[$i] = $vals[$i]";
		}
		if ($oidkey)
		{
			$sql .= " where $Stables{$tabname}->[1] = $oidkey";
		}
		else
		{
			$sql .= " where $Stables{$tabname}->[1] = $vals[$Stables{$tabname}->[2]]";
		}

		printf "$sql\n" if $debug;

		$result = $conn->exec($sql);
		if ($result->resultStatus ne PGRES_COMMAND_OK)
		{
			print STDERR $conn->errorMessage unless ($quiet);
			return(-1);
		}
		next if $result->cmdTuples == 1;	# updated

		if ($result->cmdTuples > 1)
		{
			printf STDERR "Duplicate keys\n" unless ($quiet);
			return(-2);
		}

		# no key - copy
		push @CopyBuf, "$istring\n";
		$CBufLen += length($istring);

		if ($CBufLen >= $CBufMax)
		{
			$result = DoCopy($conn, $tabname, $oidkey, \@CopyBuf);
			return($result) if $result;
			@CopyBuf = ();
			$CBufLen = 0;
		}
	}

	if (! $ok)
	{
		printf STDERR "No end of input in UPDATE section\n" unless ($quiet);
		return(-2);
	}

	if ($CBufLen)
	{
		$result = DoCopy($conn, $tabname, $oidkey, \@CopyBuf);
		return($result) if $result;
	}

	return(0);
}


sub DoCopy
{
	my ($conn, $tabname, $withoids, $CBuf) = @_; # (@_[0], @_[1], @_[2], @_[3]);

	my $sql = "COPY $tabname " . (($withoids) ? "WITH OIDS " : '') . 
"FROM STDIN";
	my $result = $conn->exec($sql);
	if ($result->resultStatus ne PGRES_COPY_IN)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		return(-1);
	}

	foreach $str (@{$CBuf})
	{
		$conn->putline($str);
	}

	$conn->putline("\\.\n");

	if ($conn->endcopy)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		return(-1);
	}

	return(0);

}


#
# Returns last SyncID applied on Slave
#
sub GetSyncID
{
	my ($conn) = @_; # (@_[0]);
	
	my $result = $conn->exec("select max(syncid) from _RSERV_SLAVE_SYNC_");
	if ($result->resultStatus ne PGRES_TUPLES_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		return(-1);
	}
	my @row = $result->fetchrow;
	return(undef) unless defined $row[0];	# null
	return($row[0]);
}

#
# Updates _RSERV_SYNC_ on Master with Slave SyncID
#
sub SyncSyncID
{
	my ($conn, $server, $syncid) = @_; # (@_[0], @_[1], @_[2]);
	
	my $result = $conn->exec("BEGIN");
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	$result = $conn->exec("select synctime, status from _RSERV_SYNC_" .
						  " where server = $server and syncid = $syncid" .
						  " for update");
	if ($result->resultStatus ne PGRES_TUPLES_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}
	my @row = $result->fetchrow;
	if (! defined $row[0])
	{
		printf STDERR "No SyncID $syncid found for server $server\n" unless ($quiet);
		$conn->exec("ROLLBACK");
		return(0);
	}
	if ($row[1] > 0)
	{
		printf STDERR "SyncID $syncid for server $server already updated\n" unless ($quiet);
		$conn->exec("ROLLBACK");
		return(0);
	}
	$result = $conn->exec("update _RSERV_SYNC_" .
						  " set synctime = now(), status = 1" .
						  " where server = $server and syncid = $syncid");
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}
	$result = $conn->exec("delete from _RSERV_SYNC_" .
						  " where server = $server and syncid < $syncid");
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	$result = $conn->exec("COMMIT");
	if ($result->resultStatus ne PGRES_COMMAND_OK)
	{
		print STDERR $conn->errorMessage unless ($quiet);
		$conn->exec("ROLLBACK");
		return(-1);
	}

	return(1);
}

1;
