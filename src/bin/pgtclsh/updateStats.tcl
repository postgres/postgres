#
# updateStats 
#   updates the statistic of number of distinct attribute values
#  (this should really be done by the vacuum command)
#   this is kind of brute force and slow, but it works
#  since we use SELECT DISTINCT to calculate the number of distinct values
# and that does a sort, you need to have plenty of disk space for the 
# intermediate sort files.
# 
# - jolly 6/8/95

#
# update_attnvals
#   takes in a table and updates the attnvals columns for the attributes
# of that table
#
#  conn is the database connection
#  rel is the table name 
proc update_attnvals {conn rel} {
    
   # first, get the oid of the rel
    set res [pg_exec $conn "SELECT oid FROM pg_class where relname = '$rel'"]
    if { [pg_result $res -numTuples] == "0"} {
	puts stderr "update_attnvals: Relation named $rel was not found"
	return
    }
    set oid [pg_result $res -getTuple 0]
    pg_result $res -clear

    # use this query to find the names of the attributes
    set res [pg_exec $conn "SELECT * FROM $rel WHERE 'f'::bool"]
    set attrNames [pg_result $res -attributes]

    puts "attrNames = $attrNames"
    foreach att $attrNames {
	# find how many distinct values there are for this attribute
	# this may fail if the user-defined type doesn't have 
	# comparison operators defined
	set res2 [pg_exec $conn "SELECT DISTINCT $att FROM $rel"]
	set NVALS($att) [pg_result $res2 -numTuples]
	puts "NVALS($att) is $NVALS($att)"
	pg_result $res2 -clear
    }
    pg_result $res -clear

    # now, update the pg_attribute table
    foreach att $attrNames {
	# first find the oid of the row to change
	set res [pg_exec $conn "SELECT oid FROM pg_attribute a WHERE a.attname = '$att' and a.attrelid = '$oid'"]
	set attoid [pg_result $res -getTuple 0]
	set res2 [pg_exec $conn "UPDATE pg_attribute SET attnvals = $NVALS($att) where pg_attribute.oid = '$attoid'::oid"]
    }
}

# updateStats
#    takes in a database name
# and updates the attnval stat for all the user-defined tables
# in the database
proc updateStats { dbName } {
    # datnames is the list to be result
    set conn [pg_connect $dbName]
    set res [pg_exec $conn "SELECT relname FROM pg_class WHERE relkind = 'r' and relname !~ '^pg_'"]
    set ntups [pg_result $res -numTuples]
    for {set i 0} {$i < $ntups} {incr i} {
	set rel [pg_result $res -getTuple $i]
	puts "updating attnvals stats on table $rel"
	update_attnvals $conn $rel
    }
    pg_disconnect $conn
}

