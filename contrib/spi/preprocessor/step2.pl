#!/usr/bin/perl

##
##  MAIN
##
$table_name="";
$old_name="";
$references_table="";
$references_column="";
$is_create=0;



while ( <> ) 
{
	chop;
	$str=$_ ;

	if ($is_create == 1) {
		$table_name=$str;
	  	$is_create=2;
	}
	if ( $str =~ /^CREATE TABLE/ ){
		$is_create=1;
	}
	if ($is_create == 2) {
	if ($str =~ /^FOREIGN KEY/){
		($d1,$d2,$d3,$columns,$d4,$d5,$references_table,$d6) = split (/ /,$str,8);
		#printf "Table $table_name $columns $references_table\n";

		if ($table_name ne $old_name ){	
			printf "--\n-- Trigger for $table_name\n--\n\n";
		}

		foreach $i ( split(/,/ , $columns ) ){
			print "CREATE INDEX I_$table_name";
			print "_$i ON $table_name ( $i ) ;\n";
		}

		printf "\nCREATE TRIGGER T_P_$table_name";
		printf "_$references_table BEFORE INSERT OR UPDATE ON $table_name FOR EACH ROW\n" ;
		printf "EXECUTE PROCEDURE\n";
		printf "check_primary_key(";
		$val=0;
		foreach $i ( split(/,/ , $columns ) ){
			print "'$i',";
			$val=$val+1 ;
		}
		print "'$references_table',";
		
		$t=1;
		foreach $i ( split(/,/,$columns ) ){
			print "'$i'";
			if ( $t < $val ) {
				printf ",";
			}	
			$t=$t+1;
		}
		print " );\n\n";

		printf "CREATE TRIGGER T_F_D_$references_table";
		printf "_$table_name BEFORE DELETE ON $references_table FOR EACH ROW\n" ;
		printf "EXECUTE PROCEDURE\n";
		printf "check_foreign_key(1,'cascade',";
		$val=0;
		foreach $i ( split(/,/ , $columns ) ){
			print "'$i',";
			$val=$val+1 ;
		}
		print "'$table_name',";
		
		$t=1;
		foreach $i ( split(/,/,$columns ) ){
			print "'$i'";
			if ( $t < $val ) {
				printf ",";
			}	
			$t=$t+1;
		}
		print " );\n\n";

		printf "CREATE TRIGGER T_F_U_$references_table";
		printf "_$table_name AFTER UPDATE ON $references_table FOR EACH ROW\n" ;
		printf "EXECUTE PROCEDURE\n";
		printf "check_foreign_key(1,'cascade',";
		$val=0;
		foreach $i ( split(/,/ , $columns ) ){
			print "'$i',";
			$val=$val+1 ;
		}
		print "'$table_name',";
		
		$t=1;
		foreach $i ( split(/,/,$columns ) ){
			print "'$i'";
			if ( $t < $val ) {
				printf ",";
			}	
			$t=$t+1;
		}
		print " );\n\n";

		if ($table_name ne $old_name ){	
			printf "-- ********************************\n\n\n";
		}
		$old_name=$table_name ;




	 }
	 }
	 if ($str =~  /^\)\;/ ) {
	    $is_create = 0 ;
	 }

}






