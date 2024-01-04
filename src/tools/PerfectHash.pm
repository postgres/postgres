#----------------------------------------------------------------------
#
# PerfectHash.pm
#    Perl module that constructs minimal perfect hash functions
#
# This code constructs a minimal perfect hash function for the given
# set of keys, using an algorithm described in
# "An optimal algorithm for generating minimal perfect hash functions"
# by Czech, Havas and Majewski in Information Processing Letters,
# 43(5):256-264, October 1992.
# This implementation is loosely based on NetBSD's "nbperf",
# which was written by Joerg Sonnenberger.
#
# The resulting hash function is perfect in the sense that if the presented
# key is one of the original set, it will return the key's index in the set
# (in range 0..N-1).  However, the caller must still verify the match,
# as false positives are possible.  Also, the hash function may return
# values that are out of range (negative or >= N), due to summing unrelated
# hashtable entries.  This indicates that the presented key is definitely
# not in the set.
#
#
# Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/tools/PerfectHash.pm
#
#----------------------------------------------------------------------

package PerfectHash;

use strict;
use warnings FATAL => 'all';


# At runtime, we'll compute two simple hash functions of the input key,
# and use them to index into a mapping table.  The hash functions are just
# multiply-and-add in uint32 arithmetic, with different multipliers and
# initial seeds.  All the complexity in this module is concerned with
# selecting hash parameters that will work and building the mapping table.

# We support making case-insensitive hash functions, though this only
# works for a strict-ASCII interpretation of case insensitivity,
# ie, A-Z maps onto a-z and nothing else.
my $case_fold = 0;


#
# Construct a C function implementing a perfect hash for the given keys.
# The C function definition is returned as a string.
#
# The keys should be passed as an array reference.  They can be any set
# of Perl strings; it is caller's responsibility that there not be any
# duplicates.  (Note that the "strings" can be binary data, but hashing
# e.g. OIDs has endianness hazards that callers must overcome.)
#
# The name to use for the function is specified as the second argument.
# It will be a global function by default, but the caller may prepend
# "static " to the result string if it wants a static function.
#
# Additional options can be specified as keyword-style arguments:
#
# case_fold => bool
# If specified as true, the hash function is case-insensitive, for the
# limited idea of case-insensitivity explained above.
#
# fixed_key_length => N
# If specified, all keys are assumed to have length N bytes, and the
# hash function signature will be just "int f(const void *key)"
# rather than "int f(const void *key, size_t keylen)".
#
sub generate_hash_function
{
	my ($keys_ref, $funcname, %options) = @_;

	# It's not worth passing this around as a parameter; just use a global.
	$case_fold = $options{case_fold} || 0;

	# Try different hash function parameters until we find a set that works
	# for these keys.  The multipliers are chosen to be primes that are cheap
	# to calculate via shift-and-add, so don't change them without care.
	# (Commonly, random seeds are tried, but we want reproducible results
	# from this program so we don't do that.)
	my $hash_mult1 = 257;
	my $hash_mult2;
	my $hash_seed1;
	my $hash_seed2;
	my @subresult;
  FIND_PARAMS:
	for ($hash_seed1 = 0; $hash_seed1 < 10; $hash_seed1++)
	{

		for ($hash_seed2 = 0; $hash_seed2 < 10; $hash_seed2++)
		{
			foreach (17, 31, 127, 8191)
			{
				$hash_mult2 = $_;    # "foreach $hash_mult2" doesn't work
				@subresult = _construct_hash_table(
					$keys_ref, $hash_mult1, $hash_mult2,
					$hash_seed1, $hash_seed2);
				last FIND_PARAMS if @subresult;
			}
		}
	}

	# Choke if we couldn't find a workable set of parameters.
	die "failed to generate perfect hash" if !@subresult;

	# Extract info from _construct_hash_table's result array.
	my $elemtype = $subresult[0];
	my @hashtab = @{ $subresult[1] };
	my $nhash = scalar(@hashtab);

	# OK, construct the hash function definition including the hash table.
	my $f = '';
	$f .= sprintf "int\n";
	if (defined $options{fixed_key_length})
	{
		$f .= sprintf "%s(const void *key)\n{\n", $funcname;
	}
	else
	{
		$f .= sprintf "%s(const void *key, size_t keylen)\n{\n", $funcname;
	}
	$f .= sprintf "\tstatic const %s h[%d] = {\n\t\t", $elemtype, $nhash;
	for (my $i = 0; $i < $nhash; $i++)
	{
		# Hash element.
		$f .= sprintf "%d", $hashtab[$i];
		next if ($i == $nhash - 1);

		# Optional indentation and newline, with eight items per line.
		$f .= sprintf ",%s",
		  ($i % 8 == 7 ? "\n\t\t" : ' ' x (6 - length($hashtab[$i])));
	}
	$f .= sprintf "\n" if ($nhash % 8 != 0);
	$f .= sprintf "\t};\n\n";
	$f .= sprintf "\tconst unsigned char *k = (const unsigned char *) key;\n";
	$f .= sprintf "\tsize_t\t\tkeylen = %d;\n", $options{fixed_key_length}
	  if (defined $options{fixed_key_length});
	$f .= sprintf "\tuint32\t\ta = %d;\n", $hash_seed1;
	$f .= sprintf "\tuint32\t\tb = %d;\n\n", $hash_seed2;
	$f .= sprintf "\twhile (keylen--)\n\t{\n";
	$f .= sprintf "\t\tunsigned char c = *k++";
	$f .= sprintf " | 0x20" if $case_fold;                 # see comment below
	$f .= sprintf ";\n\n";
	$f .= sprintf "\t\ta = a * %d + c;\n", $hash_mult1;
	$f .= sprintf "\t\tb = b * %d + c;\n", $hash_mult2;
	$f .= sprintf "\t}\n";
	$f .= sprintf "\treturn h[a %% %d] + h[b %% %d];\n", $nhash, $nhash;
	$f .= sprintf "}\n";

	return $f;
}


# Calculate a hash function as the run-time code will do.
#
# If we are making a case-insensitive hash function, we implement that
# by OR'ing 0x20 into each byte of the key.  This correctly transforms
# upper-case ASCII into lower-case ASCII, while not changing digits or
# dollar signs.  (It does change '_', as well as other characters not
# likely to appear in keywords; this has little effect on the hash's
# ability to discriminate keywords.)
sub _calc_hash
{
	my ($key, $mult, $seed) = @_;

	my $result = $seed;
	for my $c (split //, $key)
	{
		my $cn = ord($c);
		$cn |= 0x20 if $case_fold;
		$result = ($result * $mult + $cn) % 4294967296;
	}
	return $result;
}


# Attempt to construct a mapping table for a minimal perfect hash function
# for the given keys, using the specified hash parameters.
#
# Returns an array containing the mapping table element type name as the
# first element, and a ref to an array of the table values as the second.
#
# Returns an empty array on failure; then caller should choose different
# hash parameter(s) and try again.
sub _construct_hash_table
{
	my ($keys_ref, $hash_mult1, $hash_mult2, $hash_seed1, $hash_seed2) = @_;
	my @keys = @{$keys_ref};

	# This algorithm is based on a graph whose edges correspond to the
	# keys and whose vertices correspond to entries of the mapping table.
	# A key's edge links the two vertices whose indexes are the outputs of
	# the two hash functions for that key.  For K keys, the mapping
	# table must have at least 2*K+1 entries, guaranteeing that there's at
	# least one unused entry.  (In principle, larger mapping tables make it
	# easier to find a workable hash and increase the number of inputs that
	# can be rejected due to touching unused hashtable entries.  In practice,
	# neither effect seems strong enough to justify using a larger table.)
	my $nedges = scalar @keys;       # number of edges
	my $nverts = 2 * $nedges + 1;    # number of vertices

	# However, it would be very bad if $nverts were exactly equal to either
	# $hash_mult1 or $hash_mult2: effectively, that hash function would be
	# sensitive to only the last byte of each key.  Cases where $nverts is a
	# multiple of either multiplier likewise lose information.  (But $nverts
	# can't actually divide them, if they've been intelligently chosen as
	# primes.)  We can avoid such problems by adjusting the table size.
	while ($nverts % $hash_mult1 == 0
		|| $nverts % $hash_mult2 == 0)
	{
		$nverts++;
	}

	# Initialize the array of edges.
	my @E = ();
	foreach my $kw (@keys)
	{
		# Calculate hashes for this key.
		# The hashes are immediately reduced modulo the mapping table size.
		my $hash1 = _calc_hash($kw, $hash_mult1, $hash_seed1) % $nverts;
		my $hash2 = _calc_hash($kw, $hash_mult2, $hash_seed2) % $nverts;

		# If the two hashes are the same for any key, we have to fail
		# since this edge would itself form a cycle in the graph.
		return () if $hash1 == $hash2;

		# Add the edge for this key.
		push @E, { left => $hash1, right => $hash2 };
	}

	# Initialize the array of vertices, giving them all empty lists
	# of associated edges.  (The lists will be hashes of edge numbers.)
	my @V = ();
	for (my $v = 0; $v < $nverts; $v++)
	{
		push @V, { edges => {} };
	}

	# Insert each edge in the lists of edges connected to its vertices.
	for (my $e = 0; $e < $nedges; $e++)
	{
		my $v = $E[$e]{left};
		$V[$v]{edges}->{$e} = 1;

		$v = $E[$e]{right};
		$V[$v]{edges}->{$e} = 1;
	}

	# Now we attempt to prove the graph acyclic.
	# A cycle-free graph is either empty or has some vertex of degree 1.
	# Removing the edge attached to that vertex doesn't change this property,
	# so doing that repeatedly will reduce the size of the graph.
	# If the graph is empty at the end of the process, it was acyclic.
	# We track the order of edge removal so that the next phase can process
	# them in reverse order of removal.
	my @output_order = ();

	# Consider each vertex as a possible starting point for edge-removal.
	for (my $startv = 0; $startv < $nverts; $startv++)
	{
		my $v = $startv;

		# If vertex v is of degree 1 (i.e. exactly 1 edge connects to it),
		# remove that edge, and then consider the edge's other vertex to see
		# if it is now of degree 1.  The inner loop repeats until reaching a
		# vertex not of degree 1.
		while (scalar(keys(%{ $V[$v]{edges} })) == 1)
		{
			# Unlink its only edge.
			my $e = (keys(%{ $V[$v]{edges} }))[0];
			delete($V[$v]{edges}->{$e});

			# Unlink the edge from its other vertex, too.
			my $v2 = $E[$e]{left};
			$v2 = $E[$e]{right} if ($v2 == $v);
			delete($V[$v2]{edges}->{$e});

			# Push e onto the front of the output-order list.
			unshift @output_order, $e;

			# Consider v2 on next iteration of inner loop.
			$v = $v2;
		}
	}

	# We succeeded only if all edges were removed from the graph.
	return () if (scalar(@output_order) != $nedges);

	# OK, build the hash table of size $nverts.
	my @hashtab = (0) x $nverts;
	# We need a "visited" flag array in this step, too.
	my @visited = (0) x $nverts;

	# The goal is that for any key, the sum of the hash table entries for
	# its first and second hash values is the desired output (i.e., the key
	# number).  By assigning hash table values in the selected edge order,
	# we can guarantee that that's true.  This works because the edge first
	# removed from the graph (and hence last to be visited here) must have
	# at least one vertex it shared with no other edge; hence it will have at
	# least one vertex (hashtable entry) still unvisited when we reach it here,
	# and we can assign that unvisited entry a value that makes the sum come
	# out as we wish.  By induction, the same holds for all the other edges.
	foreach my $e (@output_order)
	{
		my $l = $E[$e]{left};
		my $r = $E[$e]{right};
		if (!$visited[$l])
		{
			# $hashtab[$r] might be zero, or some previously assigned value.
			$hashtab[$l] = $e - $hashtab[$r];
		}
		else
		{
			die "oops, doubly used hashtab entry" if $visited[$r];
			# $hashtab[$l] might be zero, or some previously assigned value.
			$hashtab[$r] = $e - $hashtab[$l];
		}
		# Now freeze both of these hashtab entries.
		$visited[$l] = 1;
		$visited[$r] = 1;
	}

	# Detect range of values needed in hash table.
	my $hmin = $nedges;
	my $hmax = 0;
	for (my $v = 0; $v < $nverts; $v++)
	{
		$hmin = $hashtab[$v] if $hashtab[$v] < $hmin;
		$hmax = $hashtab[$v] if $hashtab[$v] > $hmax;
	}

	# Choose width of hashtable entries.  In addition to the actual values,
	# we need to be able to store a flag for unused entries, and we wish to
	# have the property that adding any other entry value to the flag gives
	# an out-of-range result (>= $nedges).
	my $elemtype;
	my $unused_flag;

	if (   $hmin >= -0x7F
		&& $hmax <= 0x7F
		&& $hmin + 0x7F >= $nedges)
	{
		# int8 will work
		$elemtype = 'int8';
		$unused_flag = 0x7F;
	}
	elsif ($hmin >= -0x7FFF
		&& $hmax <= 0x7FFF
		&& $hmin + 0x7FFF >= $nedges)
	{
		# int16 will work
		$elemtype = 'int16';
		$unused_flag = 0x7FFF;
	}
	elsif ($hmin >= -0x7FFFFFFF
		&& $hmax <= 0x7FFFFFFF
		&& $hmin + 0x3FFFFFFF >= $nedges)
	{
		# int32 will work
		$elemtype = 'int32';
		$unused_flag = 0x3FFFFFFF;
	}
	else
	{
		die "hash table values too wide";
	}

	# Set any unvisited hashtable entries to $unused_flag.
	for (my $v = 0; $v < $nverts; $v++)
	{
		$hashtab[$v] = $unused_flag if !$visited[$v];
	}

	return ($elemtype, \@hashtab);
}

1;
