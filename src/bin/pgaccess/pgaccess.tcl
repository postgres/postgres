#!/usr/bin/wish
#############################################################################
# Visual Tcl v1.10 Project
#

#################################
# GLOBAL VARIABLES
#
global activetab; 
global dbc; 
global dbname; 
global mw; 
global host; 
global newdbname; 
global newhost; 
global newpport; 
global pport; 
global pref; 
global qlvar; 
global sdbname; 
global tablist; 
global widget; 

#################################
# USER DEFINED PROCEDURES
#
proc init {argc argv} {
global dbc host pport tablist mw fldval activetab qlvar
foreach wid {Label Text Button Listbox Checkbutton Radiobutton} {
	option add *$wid.font  -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
}
set host localhost
set pport 5432
set dbc {}
set tablist [list Tables Queries Views Sequences Functions Reports Scripts]
set activetab {}
set mw(dirtyrec) 0
set mw(id_edited) {}
catch {unset qlvar}
set qlvar(yoffs) 360
set qlvar(xoffs) 50
set qlvar(reswidth) 150
set qlvar(resfields) {}
set qlvar(ressort) {}
set qlvar(rescriteria) {}
set qlvar(restables) {}
set qlvar(critedit) 0
set qlvar(links) {}
set qlvar(ntables) 0
set qlvar(newtablename) {}
}

init $argc $argv


proc add_new_field {} {
global fldname fldtype fldsize defaultval notnull
if {$fldname==""} {
    show_error "Enter a field name"
    focus .nt.e2
	return
}
if {$fldtype==""} {
    show_error "The field type is not specified!"
	return
}
if {(($fldtype=="varchar")||($fldtype=="char"))&&($fldsize=="")} {
    focus .nt.e3
    show_error "You must specify field size!"
	return
}
if {$fldsize==""} then {set sup ""} else {set sup "($fldsize)"}
if {[regexp $fldtype "varchar2char4char8char16textdatetime"]} {set supc "'"} else {set supc ""}
if {$defaultval==""} then {set sup2 ""} else {set sup2 " DEFAULT $supc$defaultval$supc"}
# Checking for field name collision
set inspos end
for {set i 0} {$i<[.nt.lb size]} {incr i} {
	set linie [.nt.lb get $i]
	if {$fldname==[lindex [split $linie] 0]} {
		if {[tk_messageBox -title Warning -message "There is another field with the same name!\n\nReplace it ?" -type yesno -default yes]=="no"} return
		.nt.lb delete $i
		set inspos $i
	}	 
  }
.nt.lb insert $inspos [format "%-17s%-14s%-16s" $fldname $fldtype$sup $sup2$notnull]
focus .nt.e2
set fldname {}
set fldsize {}
set defaultval {}
}

proc cmd_Delete {} {
global dbc activetab
if {$dbc==""} return;
set objtodelete [get_dwlb_Selection]
if {$objtodelete==""} return;
set temp {}
switch $activetab {
	Tables {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete table:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec noquiet "drop table $objtodelete"
			sql_exec quiet "delete from pga_layout where tablename='$objtodelete'"
			cmd_Tables
		}
	}
	Views {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete view:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec noquiet "drop view $objtodelete"
			sql_exec quiet "delete from pga_layout where tablename='$objtodelete'"
			cmd_Views
		}
	}
	Queries {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete query:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec quiet "delete from pga_queries where queryname='$objtodelete'"
			sql_exec quiet "delete from pga_layout where tablename='$objtodelete'"
			cmd_Queries
		}
	}
	Sequences {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete sequence:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			sql_exec quiet "drop sequence $objtodelete"
			cmd_Sequences
		}
	}
	Functions {
		if {[tk_messageBox -title "FINAL WARNING" -message "You are going to delete function:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
			delete_function $objtodelete
			cmd_Functions
		}
	}
}
if {$temp==""} return;
}

proc cmd_Design {} {
global dbc activetab tablename
if {$dbc==""} return;
if {[.dw.lb curselection]==""} return;
set tablename [.dw.lb get [.dw.lb curselection]]
switch $activetab {
    Queries {open_query design}
}
}

proc cmd_Functions {} {
global dbc
set maxim 0
set pgid 0
cursor_watch .dw
catch {
	pg_select $dbc "select proowner,count(*) from pg_proc group by proowner" rec {
		if {$rec(count)>$maxim} {
			set maxim $rec(count)
			set pgid $rec(proowner)
		}
	}
.dw.lb delete 0 end
catch {
	pg_select $dbc "select proname from pg_proc where prolang=14 and proowner<>$pgid order by proname" rec {
		.dw.lb insert end $rec(proname)
	}	
}
cursor_arrow .dw
}
}

proc cmd_Import_Export {how} {
global dbc ie_tablename ie_filename activetab
if {$dbc==""} return;
Window show .iew
set ie_tablename {}
set ie_filename {}
set ie_delimiter {}
if {$activetab=="Tables"} {
	set tn [get_dwlb_Selection]
	set ie_tablename $tn
	if {$tn!=""} {set ie_filename "$tn.txt"}
}
.iew.expbtn configure -text $how
}

proc cmd_Information {} {
global dbc tiw activetab indexlist
if {$dbc==""} return;
if {$activetab!="Tables"} return;
set tiw(tablename) [get_dwlb_Selection]
if {$tiw(tablename)==""} return;
Window show .tiw
.tiw.lb delete 0 end
.tiw.ilb delete 0 end
set tiw(isunique) {}
set tiw(isclustered) {}
set tiw(indexfields) {}
pg_select $dbc "select attnum,attname,typname,attlen,usename,pg_class.oid from pg_class,pg_user,pg_attribute,pg_type where (pg_class.relname='$tiw(tablename)') and (pg_class.oid=pg_attribute.attrelid) and (pg_class.relowner=pg_user.usesysid) and (pg_attribute.atttypid=pg_type.oid) order by attnum" rec {
    set fsize $rec(attlen)
    set ftype $rec(typname)
    if {$ftype=="varchar"} {
        incr fsize -4
    }
    if {$ftype=="bpchar"} {
    	incr fsize -4
    }
    if {$ftype=="text"} {
        set fsize ""
    }
    if {$rec(attnum)>0} {.tiw.lb insert end [format "%-32s %-14s %-4s" $rec(attname) $ftype $fsize]}
    set tiw(owner) $rec(usename)
    set tiw(tableoid) $rec(oid)
    set tiw(f$rec(attnum)) $rec(attname)
}
set tiw(indexlist) {}
pg_select $dbc "select oid,indexrelid from pg_index where (pg_class.relname='$tiw(tablename)') and (pg_class.oid=pg_index.indrelid)" rec {
    lappend tiw(indexlist) $rec(oid)
    pg_select $dbc "select relname from pg_class where oid=$rec(indexrelid)" rec1 {
        .tiw.ilb insert end $rec1(relname)
    }
}
}

proc cmd_New {} {
global dbc activetab queryname queryoid cbv funcpar funcname funcret
if {$dbc==""} return;
switch $activetab {
    Tables {Window show .nt; focus .nt.etabn}
    Queries {
            Window show .qb
			set queryoid 0
			set queryname {}
            set cbv 0
			.qb.cbv configure -state normal
        }
    Views {
			set queryoid 0
			set queryname {}
            Window show .qb
            set cbv 1
            .qb.cbv configure -state disabled
        }
    Sequences {
			Window show .sqf
			focus .sqf.e1
    	}
	Functions {
			Window show .fw
			set funcname {}
			set funcpar {}
			set funcret {}
			place .fw.okbtn -y 255
			.fw.okbtn configure -state normal
			.fw.okbtn configure -text Define
			.fw.text1 delete 1.0 end
			focus .fw.e1
		}
}
}

proc cmd_Open {} {
global dbc activetab
if {$dbc==""} return;
set objname [get_dwlb_Selection]
if {$objname==""} return;
switch $activetab {
    Tables {Window show .mw; load_table $objname}
    Queries {open_query view}
	Views {open_view}
	Sequences {open_sequence $objname}
	Functions {open_function $objname}
}
}

proc cmd_Preferences {} {
# Show
Window show .pw
}

proc cmd_Queries {} {
global dbc

.dw.lb delete 0 end
catch {
    pg_select $dbc "select * from pga_queries order by queryname" rec {
        .dw.lb insert end $rec(queryname)
    }
}
}

proc cmd_Rename {} {
global dbc oldobjname activetab
if {$dbc==""} return;
if {$activetab=="Views"} return;
if {$activetab=="Sequences"} return;
if {$activetab=="Functions"} return;
set temp [get_dwlb_Selection]
if {$temp==""} {
	tk_messageBox -title Warning -message "Please select first an object!"
	return;
}
set oldobjname $temp
Window show .rf
}

proc cmd_Reports {} {
global dbc
}

proc cmd_Scripts {} {
global dbc
}

proc cmd_Sequences {} {
global dbc

cursor_watch .dw
.dw.lb delete 0 end
catch {
    pg_select $dbc "select * from pg_class where (relname not like 'pg_%') and (relkind='S') order by relname" rec {
        .dw.lb insert end $rec(relname)
    }
}
cursor_arrow .dw
}

proc cmd_Tables {} {
global dbc

cursor_watch .dw
.dw.lb delete 0 end
catch {
    pg_select $dbc "select * from pg_class where (relname !~ '^pg_') and (relkind='r') and (not relhasrules) order by relname" rec {
        if {![regexp "^pga_" $rec(relname)]} {.dw.lb insert end $rec(relname)}
    }
}
cursor_arrow .dw
}

proc cmd_Vacuum {} {
global dbc dbname sdbname

if {$dbc==""} return;
cursor_watch .dw
set sdbname "vacuuming database $dbname ..."
update; update idletasks
set retval [catch {
        set pgres [pg_exec $dbc "vacuum;"]
        pg_result $pgres -clear
    } msg]
cursor_arrow .dw
set sdbname $dbname
if {$retval} {
    show_error $msg
}
}

proc cmd_Views {} {
global dbc

cursor_watch .dw
.dw.lb delete 0 end
catch {
    pg_select $dbc "select * from pg_class where (relname !~ '^pg_') and (relkind='r') and (relhasrules) order by relname" rec {
        .dw.lb insert end $rec(relname)
    }
}
cursor_arrow .dw
}

proc mw_show_record {row} {
global mw msg
set mw(errorsavingnew) 0
if {$mw(newrec_fields)!=""} {
	if {$row!=$mw(last_rownum)} {
		if {![mw_save_new_record]} {
                    set mw(errorsavingnew) 1
                    return
                }
	}
}
set y1 [lindex $mw(rowy) $row]
set y2 [lindex $mw(rowy) [expr $row+1]]
if {$y2==""} {set y2 [expr $y1+14]}
.mw.c dtag hili hili
.mw.c addtag hili withtag r$row
# Making a rectangle arround the record
set x 3
foreach wi $mw(colwidth) {incr x [expr $wi+2]}
.mw.c delete crtrec
.mw.c create rectangle [expr -1-$mw(leftoffset)] $y1 [expr $x-$mw(leftoffset)] $y2 -fill #EEEEEE -outline {} -tags {q crtrec}
.mw.c lower crtrec
}

proc cursor_arrow {w} {
$w configure -cursor top_left_arrow
update idletasks
}

proc cursor_watch {w} {
$w configure -cursor watch
update idletasks
}

proc delete_function {objname} {
global dbc
pg_select $dbc "select * from pg_proc where proname='$objname'" rec {
	set funcpar $rec(proargtypes)
	set nrpar $rec(pronargs)
}
set lispar {}
for {set i 0} {$i<$nrpar} {incr i} {
	lappend lispar [get_pgtype [lindex $funcpar $i]]
}
set lispar [join $lispar ,]
sql_exec noquiet "drop function $objname ($lispar)"
}

proc mw_delete_record {} {
global dbc mw tablename
if {!$mw(updatable)} return;
if {![mw_exit_edit]} return;
set taglist [.mw.c gettags hili]
if {[llength $taglist]==0} return;
set rowtag [lindex $taglist [lsearch -regexp $taglist "^r"]]
set row [string range $rowtag 1 end]
set oid [lindex $mw(keylist) $row]
if {[tk_messageBox -title "FINAL WARNING" -icon question -message "Delete current record ?" -type yesno -default no]=="no"} return
if {[sql_exec noquiet "delete from $tablename where oid=$oid"]} {
	.mw.c delete hili
}
}

proc drag_it {w x y} {
global draglocation
	set dlo ""
	catch { set dlo $draglocation(obj) }
    if {$dlo != ""} {
        set dx [expr $x - $draglocation(x)]
        set dy [expr $y - $draglocation(y)]
        $w move $dlo $dx $dy
        set draglocation(x) $x
        set draglocation(y) $y
    }
}

proc drag_start {w x y} {
global draglocation
catch {unset draglocation}
set object [$w find closest $x $y]
if {[lsearch [.mw.c gettags $object] movable]==-1} return;
.mw.c bind movable <Leave> {}
set draglocation(obj) $object
set draglocation(x) $x
set draglocation(y) $y
set draglocation(start) $x
}

proc drag_stop {w x y} {
global draglocation mw dbc
	set dlo ""
	catch { set dlo $draglocation(obj) }
    if {$dlo != ""} {
		.mw.c bind movable <Leave> {.mw configure -cursor top_left_arrow}
		.mw configure -cursor top_left_arrow
        set ctr [get_tag_info $draglocation(obj) v]
        set diff [expr $x-$draglocation(start)]
        if {$diff==0} return;
        set newcw {}
        for {set i 0} {$i<$mw(colcount)} {incr i} {
            if {$i==$ctr} {
                lappend newcw [expr [lindex $mw(colwidth) $i]+$diff]
            } else {
                lappend newcw [lindex $mw(colwidth) $i]
            }
        }
        set mw(colwidth) $newcw
		.mw.c itemconfigure c$ctr -width [expr [lindex $mw(colwidth) $ctr]-5]
        mw_draw_headers
		mw_draw_hgrid
		if {$mw(crtrow)!=""} {mw_show_record $mw(crtrow)}
        for {set i [expr $ctr+1]} {$i<$mw(colcount)} {incr i} {
            .mw.c move c$i $diff 0
        }
		cursor_watch .mw
        sql_exec quiet "update pga_layout set colwidth='$mw(colwidth)' where tablename='$mw(layout_name)'"
		cursor_arrow .mw
    }
}

proc mw_draw_headers {} {
global mw
.mw.c delete header
set posx [expr 5-$mw(leftoffset)]
for {set i 0} {$i<$mw(colcount)} {incr i} {
    set xf [expr $posx+[lindex $mw(colwidth) $i]]
    .mw.c create rectangle $posx 1 $xf 22 -fill #CCCCCC -outline "" -width 0 -tags header
    .mw.c create text [expr $posx+[lindex $mw(colwidth) $i]*1.0/2] 14 -text [lindex $mw(colnames) $i] -tags header -fill navy -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
    .mw.c create line $posx 22 [expr $xf-1] 22 -fill #AAAAAA -tags header
    .mw.c create line [expr $xf-1] 5 [expr $xf-1] 22 -fill #AAAAAA -tags header
    .mw.c create line [expr $xf+1] 5 [expr $xf+1] 22 -fill white -tags header
    .mw.c create line $xf -15000 $xf 15000 -fill #CCCCCC -tags [subst {header movable v$i}]
    set posx [expr $xf+2]
}
set mw(r_edge) $posx
.mw.c bind movable <Button-1> {drag_start %W %x %y}
.mw.c bind movable <B1-Motion> {drag_it %W %x %y}
.mw.c bind movable <ButtonRelease-1> {drag_stop %W %x %y}
.mw.c bind movable <Enter> {.mw configure -cursor left_side}
.mw.c bind movable <Leave> {.mw configure -cursor top_left_arrow}
}

proc mw_draw_new_record {} {
global mw pref
set posx 10
set posy [lindex $mw(rowy) $mw(last_rownum)]
if {$pref(tvfont)=="helv"} {
    set tvfont -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
} else {
    set tvfont -*-Clean-Medium-R-Normal-*-*-130-*-*-*-*-*
}
if {$mw(updatable)} {for {set j 0} {$j<$mw(colcount)} {incr j} {
	.mw.c create text $posx $posy -text * -tags [subst {r$mw(nrecs) c$j q new unt}]  -anchor nw -font $tvfont -width [expr [lindex $mw(colwidth) $j]-5]
    incr posx [expr [lindex $mw(colwidth) $j]+2]
  }
  incr posy 14
  lappend mw(rowy) $posy
  .mw.c create line [expr -$mw(leftoffset)] $posy [expr $mw(r_edge)-$mw(leftoffset)] $posy -fill gray -tags [subst {hgrid g$mw(nrecs)}]
}
}

proc draw_tabs {} {
global tablist activetab
set ypos 85
foreach tab $tablist {
    label .dw.tab$tab -borderwidth 1  -anchor w -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -relief raised -text $tab
    place .dw.tab$tab -x 10 -y $ypos -height 25 -width 82 -anchor nw -bordermode ignore
    lower .dw.tab$tab
    bind .dw.tab$tab <Button-1> {tab_click %W}
    incr ypos 25
}
set activetab ""
}

proc mw_edit_text {c k} {
global mw msg
set bbin [.mw.c bbox r$mw(row_edited)]
switch $k {
    BackSpace { set dp [expr [.mw.c index $mw(id_edited) insert]-1];if {$dp>=0} {.mw.c dchars $mw(id_edited) $dp $dp; set mw(dirtyrec) 1}}
    Home {.mw.c icursor $mw(id_edited) 0}
    End {.mw.c icursor $mw(id_edited) end}
    Left {.mw.c icursor $mw(id_edited) [expr [.mw.c index $mw(id_edited) insert]-1]}
    Delete {}
    Right {.mw.c icursor $mw(id_edited) [expr [.mw.c index $mw(id_edited) insert]+1]}
    Return {if {[mw_exit_edit]} {.mw.c focus {}}}
    Escape {set mw(dirtyrec) 0; .mw.c itemconfigure $mw(id_edited) -text $mw(text_initial_value); .mw.c focus {}}
    default {if {[string compare $c " "]>-1} {.mw.c insert $mw(id_edited) insert $c;set mw(dirtyrec) 1}}
}
set bbout [.mw.c bbox r$mw(row_edited)]
set dy [expr [lindex $bbout 3]-[lindex $bbin 3]]
if {$dy==0} return
set re $mw(row_edited)
.mw.c move g$re 0 $dy
for {set i [expr 1+$re]} {$i<=$mw(nrecs)} {incr i} {
	.mw.c move r$i 0 $dy
	.mw.c move g$i 0 $dy
	set rh [lindex $mw(rowy) $i]
	incr rh $dy
	set mw(rowy) [lreplace $mw(rowy) $i $i $rh]
}
mw_show_record $mw(row_edited)
# Delete is trapped by window interpreted as record delete
#    Delete {.mw.c dchars $mw(id_edited) insert insert; set mw(dirtyrec) 1}
}

proc get_dwlb_Selection {} {
set temp [.dw.lb curselection]
if {$temp==""} return "";
return [.dw.lb get $temp]
}

proc get_pgtype {oid} {
global dbc
set temp "unknown"
pg_select $dbc "select typname from pg_type where oid=$oid" rec {
	set temp $rec(typname)
}
return $temp
}

proc get_tag_info {itemid prefix} {
set taglist [.mw.c itemcget $itemid -tags]
set i [lsearch -glob $taglist $prefix*]
set thetag [lindex $taglist $i]
return [string range $thetag 1 end]
}

proc mw_exit_edit {} {
global mw dbc msg tablename
# User has edited the text ?
if {!$mw(dirtyrec)} {
	# No, unfocus text
    .mw.c focus {}
    # For restoring * to the new record position
	if {$mw(id_edited)!=""} {
	    if {[lsearch [.mw.c gettags $mw(id_edited)] new]!=-1} {
	        .mw.c itemconfigure $mw(id_edited) -text $mw(text_initial_value)
	    }
	}
    set mw(id_edited) {};set mw(text_initial_value) {}
    return 1
}
# Trimming the spaces
set fldval [string trim [.mw.c itemcget $mw(id_edited) -text]]
.mw.c itemconfigure $mw(id_edited) -text $fldval
if {[string compare $mw(text_initial_value) $fldval]==0} {
    set mw(dirtyrec) 0
    .mw.c focus {}
    set mw(id_edited) {};set mw(text_initial_value) {}
    return 1
}
cursor_watch .mw
set oid [lindex $mw(keylist) $mw(row_edited)]
set fld [lindex $mw(colnames) [get_tag_info $mw(id_edited) c]]
set fillcolor black
if {$mw(row_edited)==$mw(last_rownum)} {
	set fillcolor red
	set sfp [lsearch $mw(newrec_fields) $fld]
	if {$sfp>-1} {
		set mw(newrec_fields) [lreplace $mw(newrec_fields) $sfp $sfp]
		set mw(newrec_values) [lreplace $mw(newrec_values) $sfp $sfp]
	}			
	lappend mw(newrec_fields) $fld
	lappend mw(newrec_values) '$fldval'
	# Remove the untouched tag from the object
	.mw.c dtag $mw(id_edited) unt
        .mw.c itemconfigure $mw(id_edited) -fill red
	set retval 1
} else {
    set msg "Updating record ..."
    after 1000 {set msg ""}
    set retval [sql_exec noquiet "update $tablename set $fld='$fldval' where oid=$oid"]
}
cursor_arrow .mw
if {!$retval} {
    set msg ""
	focus .mw.c
    return 0
}
set mw(dirtyrec) 0
.mw.c focus {}
set mw(id_edited) {};set mw(text_initial_value) {}
return 1
}

proc mw_load_layout {tablename} {
global dbc msg mw
cursor_watch .mw
set mw(layout_name) $tablename
catch {unset mw(colcount) mw(colnames) mw(colwidth)}
set mw(layout_found) 0
set retval [catch {set pgres [pg_exec $dbc "select *,oid from pga_layout where tablename='$tablename' order by oid desc"]}]
if {$retval} {
    # Probably table pga_layout isn't yet defined
    sql_exec noquiet "create table pga_layout (tablename varchar(64),nrcols int2,colnames text,colwidth text)"
	sql_exec quiet "grant ALL on pga_layout to PUBLIC"
} else {
	set nrlay [pg_result $pgres -numTuples]
    if {$nrlay>=1} {
        set layoutinfo [pg_result $pgres -getTuple 0]
        set mw(colcount) [lindex $layoutinfo 1]
        set mw(colnames)  [lindex $layoutinfo 2]
        set mw(colwidth) [lindex $layoutinfo 3]
		set goodoid [lindex $layoutinfo 4]
        set mw(layout_found) 1
    }
    if {$nrlay>1} {
		show_error "Multiple ([pg_result $pgres -numTuples]) layout info found\n\nPlease report the bug!"
		sql_exec quiet "delete from pga_layout where (tablename='$tablename') and (oid<>$goodoid)"
    }
}
catch {pg_result $pgres -clear}
}

proc load_pref {} {
global pref
set retval [catch {set fid [open "~/.pgaccessrc" r]}]
if {$retval} {
    set pref(rows) 200
    set pref(tvfont) clean
    set pref(autoload) 1
    set pref(lastdb) {}
    set pref(lasthost) localhost
    set pref(lastport) 5432
} else {
    while {![eof $fid]} {
        set pair [gets $fid]
        set pref([lindex $pair 0]) [lindex $pair 1]
    }
    close $fid
}
}

proc load_table {objname} {
global mw sortfield filter tablename
set tablename $objname
mw_load_layout $objname
set mw(query) "select oid,$tablename.* from $objname"
set mw(updatable) 1
set mw(isaquery) 0
mw_select_records $mw(query)
wm title .mw "Table viewer : $objname"
}

proc mw_canvas_click {x y} {
global mw msg
if {![mw_exit_edit]} return
# Determining row
for {set row 0} {$row<$mw(nrecs)} {incr row} {
	if {[lindex $mw(rowy) $row]>$y} break
}
incr row -1
if {$y>[lindex $mw(rowy) $mw(last_rownum)]} {set row $mw(last_rownum)}
if {$row<0} return
set mw(row_edited) $row
set mw(crtrow) $row
mw_show_record $row
if {$mw(errorsavingnew)} return
# Determining column
set posx [expr -$mw(leftoffset)]
set col 0
foreach cw $mw(colwidth) {
    incr posx [expr $cw+2]
    if {$x<$posx} break
    incr col
}
set itlist [.mw.c find withtag r$row]
foreach item $itlist {
    if {[get_tag_info $item c]==$col} {
        mw_start_edit $item $x $y
        break
    }
}
}

proc mw_start_edit {id x y} {
global mw msg
if {!$mw(updatable)} return
set mw(id_edited) $id
set mw(dirtyrec) 0
set mw(text_initial_value) [.mw.c itemcget $id -text]
focus .mw.c
.mw.c focus $id
.mw.c icursor $id @$x,$y
if {$mw(row_edited)==$mw(nrecs)} {
    if {[.mw.c itemcget $id -text]=="*"} {
        .mw.c itemconfigure $id -text ""
        .mw.c icursor $id 0
    }
}
}

proc open_database {} {
global dbc host pport dbname sdbname newdbname newhost newpport pref
catch {cursor_watch .dbod}
if {[catch {set newdbc [pg_connect $newdbname -host $newhost -port $newpport]} msg]} {
    catch {cursor_arrow .dbod}
    show_error "Error connecting database\n$msg"
} else {
    catch {pg_disconnect $dbc}
    set dbc $newdbc
    set host $newhost
    set pport $newpport
    set dbname $newdbname
    set sdbname $dbname
    set pref(lastdb) $dbname
    set pref(lasthost) $host
    set pref(lastport) $pport
    save_pref
    catch {cursor_arrow .dbod; Window hide .dbod}
    tab_click .dw.tabTables
    set pgres [pg_exec $dbc "select relname from pg_class where relname='pga_queries'"]
    if {[pg_result $pgres -numTuples]==0} {
        pg_result $pgres -clear
        sql_exec quiet "create table pga_queries (queryname varchar(64),querytype char(1),querycommand text)"
		sql_exec quiet "grant ALL on pga_queries to PUBLIC"
    }
    catch { pg_result $pgres -clear }
}
}

proc open_function {objname} {
global dbc funcname funcpar funcret
Window show .fw
place .fw.okbtn -y 400
.fw.okbtn configure -state disabled
.fw.text1 delete 1.0 end
pg_select $dbc "select * from pg_proc where proname='$objname'" rec {
	set funcname $objname
	set temppar $rec(proargtypes)
	set funcret [get_pgtype $rec(prorettype)]
	set funcnrp $rec(pronargs)
	.fw.text1 insert end $rec(prosrc)
}
set funcpar {}
for {set i 0} {$i<$funcnrp} {incr i} {
	lappend funcpar [get_pgtype [lindex $temppar $i]]
}
set funcpar [join $funcpar ,]
}

proc open_query {how} {
global dbc queryname mw queryoid sortfield filter

if {[.dw.lb curselection]==""} return;
set queryname [.dw.lb get [.dw.lb curselection]]
if {[catch {set pgres [pg_exec $dbc "select querycommand,querytype,oid from pga_queries where queryname='$queryname'"]}]} then {
    show_error "Error retrieving query definition"
    return
}
if {[pg_result $pgres -numTuples]==0} {
    show_error "Query $queryname was not found!"
    pg_result $pgres -clear
    return
}
set tuple [pg_result $pgres -getTuple 0]
set qcmd [lindex $tuple 0]
set qtype [lindex $tuple 1]
set queryoid [lindex $tuple 2]
pg_result $pgres -clear
if {$how=="design"} {
    Window show .qb
    .qb.text1 delete 0.0 end
    .qb.text1 insert end $qcmd
} else {
    if {$qtype=="S"} then {
        Window show .mw
		wm title .mw "Query result: $queryname"
        mw_load_layout $queryname
        set mw(query) $qcmd
        set mw(updatable) 0
        set mw(isaquery) 1
        mw_select_records $qcmd
    } else {
        set answ [tk_messageBox -title Warning -type yesno -message "This query is an action query!\n\n$qcmd\n\nDo you want to execute it?"]
        if {$answ} {
            if {[sql_exec noquiet $qcmd]} {
                tk_messageBox -title Information -message "Your query has been executed without error!"
            }
        }
    }
}
}

proc open_sequence {objname} {
global dbc seq_name seq_inc seq_start seq_minval seq_maxval
Window show .sqf
set flag 1
pg_select $dbc "select * from $objname" rec {
	set flag 0
	set seq_name $objname
	set seq_inc $rec(increment_by)
	set seq_start $rec(last_value)
	.sqf.l3 configure -text "Last value"
	set seq_minval $rec(min_value)
	set seq_maxval $rec(max_value)
	.sqf.defbtn configure -state disabled
	place .sqf.defbtn -x 40 -y 300
}
if {$flag} {
	show_error "Sequence $objname not found!"
} else {
	for {set i 1} {$i<6} {incr i} {
		.sqf.e$i configure -state disabled
	}
	focus .sqf.closebtn
}
}

proc open_view {} {
global mw
set vn [get_dwlb_Selection]
if {$vn==""} return;
Window show .mw
set mw(query) "select * from $vn"
set mw(isaquery) 0
set mw(updatable) 0
mw_load_layout $vn
mw_select_records $mw(query)
}

proc mw_pan_left {} {
global mw
if {![mw_exit_edit]} return;
if {$mw(leftcol)==[expr $mw(colcount)-1]} return;
set diff [expr 2+[lindex $mw(colwidth) $mw(leftcol)]]
incr mw(leftcol)
incr mw(leftoffset) $diff
.mw.c move header -$diff 0
.mw.c move q -$diff 0
.mw.c move hgrid -$diff 0
}

proc mw_pan_right {} {
global mw
if {![mw_exit_edit]} return;
if {$mw(leftcol)==0} return;
incr mw(leftcol) -1
set diff [expr 2+[lindex $mw(colwidth) $mw(leftcol)]]
incr mw(leftoffset) -$diff
.mw.c move header $diff 0
.mw.c move q $diff 0
.mw.c move hgrid $diff 0
}

proc ql_add_new_table {} {
global qlvar dbc

if {$qlvar(newtablename)==""} return
set fldlist {}
cursor_watch .ql
pg_select $dbc "select attnum,attname from pg_class,pg_attribute where (pg_class.relname='$qlvar(newtablename)') and (pg_class.oid=pg_attribute.attrelid) and (attnum>0) order by attnum" rec {
        lappend fldlist $rec(attname)
}
cursor_arrow .ql
if {$fldlist==""} {
    show_error "Table $qlvar(newtablename) not found!"
    return
}
set qlvar(tablename$qlvar(ntables)) $qlvar(newtablename)
set qlvar(tablestruct$qlvar(ntables)) $fldlist
incr qlvar(ntables)
if {$qlvar(ntables)==1} {
   ql_draw_lizzard
} else {
   ql_draw_table [expr $qlvar(ntables)-1]
}
set qlvar(newtablename) {}
focus .ql.entt
}

proc ql_compute_sql {} {
global qlvar
set sqlcmd "select "
for {set i 0} {$i<[llength $qlvar(resfields)]} {incr i} {
    if {$sqlcmd!="select "} {set sqlcmd "$sqlcmd, "}
    set sqlcmd "$sqlcmd[lindex $qlvar(restables) $i].[lindex $qlvar(resfields) $i]"
}
set tables {}
for {set i 0} {$i<$qlvar(ntables)} {incr i} {
    set thename {}
    catch {set thename $qlvar(tablename$i)}
    if {$thename!=""} {lappend tables $qlvar(tablename$i)}
}
set sqlcmd "$sqlcmd from [join $tables ,] "
set sup1 {}
if {[llength $qlvar(links)]>0} {
    set sup1 "where "
    foreach link $qlvar(links) {
        if {$sup1!="where "} {set sup1 "$sup1 and "}
        set sup1 "$sup1 ([lindex $link 0].[lindex $link 1]=[lindex $link 2].[lindex $link 3])"
    }
}
for {set i 0} {$i<[llength $qlvar(resfields)]} {incr i} {
    set crit [lindex $qlvar(rescriteria) $i]
    if {$crit!=""} {
        if {$sup1==""} {set sup1 "where "}
        if {[string length $sup1]>6} {set sup1 "$sup1 and "}
        set sup1 "$sup1 ([lindex $qlvar(restables) $i].[lindex $qlvar(resfields) $i] $crit) "        
    }        
}
set sqlcmd "$sqlcmd $sup1"
set sup2 {}
for {set i 0} {$i<[llength $qlvar(ressort)]} {incr i} {
    set how [lindex $qlvar(ressort) $i]
    if {$how!="unsorted"} {
        if {$how=="Ascending"} {set how asc} else {set how desc}
        if {$sup2==""} {set sup2 " order by "} else {set sup2 "$sup2,"}
        set sup2 "$sup2 [lindex $qlvar(resfields) $i] $how "
    }
}
set sqlcmd "$sqlcmd $sup2"
set qlvar(sql) $sqlcmd
#tk_messageBox -message $sqlcmd
return $sqlcmd
}

proc ql_delete_object {} {
global qlvar
# Checking if there 
set obj [.ql.c find withtag hili]
if {$obj==""} return
# Is object a link ?
if {[ql_get_tag_info $obj link]=="s"} {
    if {[tk_messageBox -title WARNING -icon question -message "Remove link ?" -type yesno -default no]=="no"} return
    set linkid [ql_get_tag_info $obj lkid]
    set qlvar(links) [lreplace $qlvar(links) $linkid $linkid]
    .ql.c delete links
    ql_draw_links
}
# Is object a result field ?
if {[ql_get_tag_info $obj res]=="f"} {
    set col [ql_get_tag_info $obj col]
    if {$col==""} return
    if {[tk_messageBox -title WARNING -icon question -message "Remove field from result ?" -type yesno -default no]=="no"} return
    set qlvar(resfields) [lreplace $qlvar(resfields) $col $col]
    set qlvar(restables) [lreplace $qlvar(restables) $col $col]
    set qlvar(rescriteria) [lreplace $qlvar(rescriteria) $col $col]
    ql_draw_res_panel
    return
}
# Is object a table ?
set tablename [ql_get_tag_info $obj tab]
if {$tablename==""} return
if {[tk_messageBox -title WARNING -icon question -message "Remove table $tablename from query ?" -type yesno -default no]=="no"} return
for {set i [expr [llength $qlvar(restables)]-1]} {$i>=0} {incr i -1} {
    if {$tablename==[lindex $qlvar(restables) $i]} {
       set qlvar(resfields) [lreplace $qlvar(resfields) $i $i]
       set qlvar(restables) [lreplace $qlvar(restables) $i $i]
       set qlvar(rescriteria) [lreplace $qlvar(rescriteria) $i $i]
    }
}
for {set i [expr [llength $qlvar(links)]-1]} {$i>=0} {incr i -1} {
    set thelink [lindex $qlvar(links) $i]
    if {($tablename==[lindex $thelink 0]) || ($tablename==[lindex $thelink 2])} {
        set qlvar(links) [lreplace $qlvar(links) $i $i]
    }
}
for {set i 0} {$i<$qlvar(ntables)} {incr i} {
    set temp {}
    catch {set temp $qlvar(tablename$i)}
    if {$temp=="$tablename"} {
        unset qlvar(tablename$i)
        unset qlvar(tablestruct$i)
        break
    }
}
incr qlvar(ntables) -1
.ql.c delete tab$tablename
.ql.c delete links
ql_draw_links
ql_draw_res_panel
}

proc ql_dragit {w x y} {
global draginfo
if {"$draginfo(obj)" != ""} {
    set dx [expr $x - $draginfo(x)]
    set dy [expr $y - $draginfo(y)]
    if {$draginfo(is_a_table)} {
        set taglist [.ql.c gettags $draginfo(obj)]
        set tabletag [lindex $taglist [lsearch -regexp $taglist "^tab"]]
        $w move $tabletag $dx $dy
        ql_draw_links
    } else {
        $w move $draginfo(obj) $dx $dy
    }
    set draginfo(x) $x
    set draginfo(y) $y
}
}

proc ql_dragstart {w x y} {
global draginfo
catch {unset draginfo}
set draginfo(obj) [$w find closest $x $y]
if {[ql_get_tag_info $draginfo(obj) r]=="ect"} {
    # If it'a a rectangle, exit
    set draginfo(obj) {}
    return
}
.ql configure -cursor hand1
.ql.c raise $draginfo(obj)
set draginfo(table) 0
if {[ql_get_tag_info $draginfo(obj) table]=="header"} {
    set draginfo(is_a_table) 1
    .ql.c itemconfigure [.ql.c find withtag hili] -fill black
    .ql.c dtag [.ql.c find withtag hili] hili
    .ql.c addtag hili withtag $draginfo(obj)
    .ql.c itemconfigure hili -fill blue
} else {
    set draginfo(is_a_table) 0
}
set draginfo(x) $x
set draginfo(y) $y
set draginfo(sx) $x
set draginfo(sy) $y
}

proc ql_dragstop {x y} {
global draginfo qlvar
.ql configure -cursor top_left_arrow
set este {}
catch {set este $draginfo(obj)}
if {$este==""} return
# Re-establish the normal paint order so
# information won't be overlapped by table rectangles
# or link linkes
.ql.c lower $draginfo(obj)
.ql.c lower rect
.ql.c lower links
set qlvar(panstarted) 0
if {$draginfo(is_a_table)} {
    set draginfo(obj) {}
    .ql.c delete links
    ql_draw_links
    return
}
.ql.c move $draginfo(obj) [expr $draginfo(sx)-$x] [expr $draginfo(sy)-$y]
if {($y>$qlvar(yoffs)) && ($x>$qlvar(xoffs))} {
    # Drop position : inside the result panel
    # Compute the offset of the result panel due to panning
    set resoffset [expr [lindex [.ql.c bbox resmarker] 0]-$qlvar(xoffs)]
    set newfld [.ql.c itemcget $draginfo(obj) -text]
    set tabtag [ql_get_tag_info $draginfo(obj) tab]
    set col [expr int(($x-$qlvar(xoffs)-$resoffset)/$qlvar(reswidth))]
    set qlvar(resfields) [linsert $qlvar(resfields) $col $newfld]
    set qlvar(ressort) [linsert $qlvar(ressort) $col unsorted]
    set qlvar(rescriteria) [linsert $qlvar(rescriteria) $col {}]
    set qlvar(restables) [linsert $qlvar(restables) $col $tabtag]
    ql_draw_res_panel    
} else {
    # Drop position : in the table panel
    set droptarget [.ql.c find overlapping $x $y $x $y]
    set targettable {}
    foreach item $droptarget {
        set targettable [ql_get_tag_info $item tab]
        set targetfield [ql_get_tag_info $item f-]
        if {($targettable!="") && ($targetfield!="")} {
            set droptarget $item
            break
        }
    }
    # check if target object isn't a rectangle
    if {[ql_get_tag_info $droptarget rec]=="t"} {set targettable {}}
    if {$targettable!=""} {
        # Target has a table
        # See about originate table
        set sourcetable [ql_get_tag_info $draginfo(obj) tab]
        if {$sourcetable!=""} {
            # Source has also a tab .. tag
            set sourcefield [ql_get_tag_info $draginfo(obj) f-]
            if {$sourcetable!=$targettable} {
                lappend qlvar(links) [list $sourcetable $sourcefield $targettable $targetfield $draginfo(obj) $droptarget]
                ql_draw_links
            }
        }
    }
}
# Erase information about onbject beeing dragged
set draginfo(obj) {}
}

proc ql_draw_links {} {
global qlvar
.ql.c delete links
set i 0
foreach link $qlvar(links) {
    # Compute the source and destination right edge
    set sre [lindex [.ql.c bbox tab[lindex $link 0]] 2]
    set dre [lindex [.ql.c bbox tab[lindex $link 2]] 2]
    # Compute field bound boxes
    set sbbox [.ql.c bbox [lindex $link 4]]
    set dbbox [.ql.c bbox [lindex $link 5]]
    # Compute the auxiliary lines
    if {[lindex $sbbox 2] < [lindex $dbbox 0]} {
        # Source object is on the left of target object
        set x1 $sre
        set y1 [expr ([lindex $sbbox 1]+[lindex $sbbox 3])/2]
        .ql.c create line $x1 $y1 [expr $x1+10] $y1 -tags [subst {links lkid$i}] -width 3
        set x2 [lindex $dbbox 0]
        set y2 [expr ([lindex $dbbox 1]+[lindex $dbbox 3])/2]
        .ql.c create line [expr $x2-10] $y2 $x2 $y2 -tags {links} -width 3
        .ql.c create line [expr $x1+10] $y1 [expr $x2-10] $y2 -tags [subst {links lkid$i}] -width 2
    } else {
        # source object is on the right of target object
        set x1 [lindex $sbbox 0]
        set y1 [expr ([lindex $sbbox 1]+[lindex $sbbox 3])/2]
        .ql.c create line $x1 $y1 [expr $x1-10] $y1 -tags [subst {links lkid$i}] -width 3
        set x2 $dre
        set y2 [expr ([lindex $dbbox 1]+[lindex $dbbox 3])/2]
        .ql.c create line $x2 $y2 [expr $x2+10] $y2 -width 3 -tags [subst {links lkid$i}]
        .ql.c create line [expr $x1-10] $y1 [expr $x2+10] $y2 -tags [subst {links lkid$i}] -width 2
    }
    incr i
}
.ql.c lower links
.ql.c bind links <Button-1> {ql_link_click %x %y}
}

proc ql_draw_lizzard {} {
global qlvar
.ql.c delete all
set posx 20
for {set it 0} {$it<$qlvar(ntables)} {incr it} {
    ql_draw_table $it
}
.ql.c lower rect
.ql.c create line 0 $qlvar(yoffs) 10000 $qlvar(yoffs) -width 3
.ql.c create rectangle 0 $qlvar(yoffs) 10000 5000 -fill #FFFFFF
for {set i [expr 15+$qlvar(yoffs)]} {$i<500} {incr i 15} {
    .ql.c create line $qlvar(xoffs) $i 10000 $i -fill #CCCCCC -tags {resgrid}
}    
for {set i $qlvar(xoffs)} {$i<10000} {incr i $qlvar(reswidth)} {
    .ql.c create line $i [expr 1+$qlvar(yoffs)] $i 10000 -fill #cccccc -tags {resgrid}
}
# Make a marker for result panel offset calculations (due to panning)
.ql.c create line $qlvar(xoffs) $qlvar(yoffs) $qlvar(xoffs) 500 -tags {resmarker resgrid}
.ql.c create rectangle 0 $qlvar(yoffs) $qlvar(xoffs) 5000 -fill #EEEEEE -tags {reshdr}
.ql.c create text 5 [expr 1+$qlvar(yoffs)] -text Field: -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags {reshdr}
.ql.c create text 5 [expr 16+$qlvar(yoffs)] -text Table: -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags {reshdr}
.ql.c create text 5 [expr 31+$qlvar(yoffs)] -text Sort: -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags {reshdr}
.ql.c create text 5 [expr 46+$qlvar(yoffs)] -text Criteria: -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags {reshdr}
.ql.c bind mov <Button-1> {ql_dragstart %W %x %y}
.ql.c bind mov <B1-Motion> {ql_dragit %W %x %y}
bind .ql <ButtonRelease-1> {ql_dragstop %x %y}
bind .ql <Button-1> {qlc_click %x %y %W}
bind .ql <B1-Motion> {ql_pan %x %y}
bind .ql <Key-Delete> {ql_delete_object}
}

proc ql_draw_res_panel {} {
global qlvar
# Compute the offset of the result panel due to panning
set resoffset [expr [lindex [.ql.c bbox resmarker] 0]-$qlvar(xoffs)]
.ql.c delete resp
for {set i 0} {$i<[llength $qlvar(resfields)]} {incr i} {
    .ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)] [expr 1+$qlvar(yoffs)] -text [lindex $qlvar(resfields) $i] -anchor nw -tags [subst {resf resp col$i}] -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
    .ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)] [expr 16+$qlvar(yoffs)] -text [lindex $qlvar(restables) $i] -anchor nw -tags {resp rest} -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
    .ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)] [expr 31+$qlvar(yoffs)] -text [lindex $qlvar(ressort) $i] -anchor nw -tags {resp sort} -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
    if {[lindex $qlvar(rescriteria) $i]!=""} {
        .ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)]  [expr $qlvar(yoffs)+46+15*0] -anchor nw -text [lindex $qlvar(rescriteria) $i]  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -tags [subst {resp cr-c$i-r0}]
    }
}
.ql.c raise reshdr
.ql.c bind resf <Button-1> {ql_resfield_click %x %y}
.ql.c bind sort <Button-1> {ql_swap_sort %W %x %y}
}

proc ql_draw_table {it} {
global qlvar

set posy 10
set allbox [.ql.c bbox rect]
if {$allbox==""} {set posx 10} else {set posx [expr 20+[lindex $allbox 2]]}
set tablename $qlvar(tablename$it)
.ql.c create text $posx $posy -text $tablename -anchor nw -tags [subst {tab$tablename f-oid mov tableheader}] -font -Adobe-Helvetica-Bold-R-Normal-*-*-120-*-*-*-*-*
incr posy 16
foreach fld $qlvar(tablestruct$it) {
   .ql.c create text $posx $posy -text $fld -anchor nw -tags [subst {f-$fld tab$tablename mov}] -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
   incr posy 14
}
set reg [.ql.c bbox tab$tablename]
.ql.c create rectangle [lindex $reg 0] [lindex $reg 1] [lindex $reg 2] [lindex $reg 3] -fill #EEEEEE -tags [subst {rect tab$tablename}]
.ql.c create line [lindex $reg 0] [expr [lindex $reg 1]+15] [lindex $reg 2] [expr [lindex $reg 1]+15] -tags [subst {rect tab$tablename}]
.ql.c lower tab$tablename
.ql.c lower rect
}

proc ql_get_tag_info {obj prefix} {
set taglist [.ql.c gettags $obj]
set tagpos [lsearch -regexp $taglist "^$prefix"]
if {$tagpos==-1} {return ""}
set thattag [lindex $taglist $tagpos]
return [string range $thattag [string length $prefix] end]
}

proc ql_init {} {
global qlvar
catch {unset qlvar}
set qlvar(yoffs) 360
set qlvar(xoffs) 50
set qlvar(reswidth) 150
set qlvar(resfields) {}
set qlvar(ressort) {}
set qlvar(rescriteria) {}
set qlvar(restables) {}
set qlvar(critedit) 0
set qlvar(links) {}
set qlvar(ntables) 0
set qlvar(newtablename) {}
}

proc ql_link_click {x y} {
global qlvar

set obj [.ql.c find closest $x $y 1 links]
if {[ql_get_tag_info $obj link]!="s"} return
.ql.c itemconfigure [.ql.c find withtag hili] -fill black
.ql.c dtag [.ql.c find withtag hili] hili
.ql.c addtag hili withtag $obj
.ql.c itemconfigure $obj -fill blue
}

proc ql_pan {x y} {
global qlvar
set panstarted 0
catch {set panstarted $qlvar(panstarted) }
if {!$panstarted} return
set dx [expr $x-$qlvar(panstartx)]
set dy [expr $y-$qlvar(panstarty)]
set qlvar(panstartx) $x
set qlvar(panstarty) $y
if {$qlvar(panobject)=="tables"} {
    .ql.c move mov $dx $dy
    .ql.c move links $dx $dy
    .ql.c move rect $dx $dy
} else {
    .ql.c move resp $dx 0
    .ql.c move resgrid $dx 0
    .ql.c raise reshdr
}
}

proc ql_resfield_click {x y} {
global qlvar

set obj [.ql.c find closest $x $y]
if {[ql_get_tag_info $obj res]!="f"} return
.ql.c itemconfigure [.ql.c find withtag hili] -fill black
.ql.c dtag [.ql.c find withtag hili] hili
.ql.c addtag hili withtag $obj
.ql.c itemconfigure $obj -fill blue
}

proc ql_show_sql {} {
global qlvar

set sqlcmd [ql_compute_sql]
.ql.c delete sqlpage
.ql.c create rectangle 0 0 2000 [expr $qlvar(yoffs)-1] -fill #ffffff -tags {sqlpage}
.ql.c create text 10 10 -text $sqlcmd -anchor nw -width 550 -tags {sqlpage} -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
.ql.c bind sqlpage <Button-1> {.ql.c delete sqlpage}
}

proc ql_swap_sort {w x y} {
global qlvar
set obj [$w find closest $x $y]
set taglist [.ql.c gettags $obj]
if {[lsearch $taglist sort]==-1} return
set cum [.ql.c itemcget $obj -text]
if {$cum=="unsorted"} {
    set cum Ascending
} elseif {$cum=="Ascending"} {
    set cum Descending
} else {
    set cum unsorted
}
set col [expr int(($x-$qlvar(xoffs))/$qlvar(reswidth))]
set qlvar(ressort) [lreplace $qlvar(ressort) $col $col $cum]
.ql.c itemconfigure $obj -text $cum
}

proc qlc_click {x y w} {
global qlvar
set qlvar(panstarted) 0
if {$w==".ql.c"} {
    set canpan 1
    if {$y<$qlvar(yoffs)} {
        if {[llength [.ql.c find overlapping $x $y $x $y]]!=0} {set canpan 0}
            set qlvar(panobject) tables
    } else {
        set qlvar(panobject) result
    }
    if {$canpan} {
        .ql configure -cursor hand1
        set qlvar(panstartx) $x
        set qlvar(panstarty) $y
        set qlvar(panstarted) 1
    }
}
set isedit 0
catch {set isedit $qlvar(critedit)}
# Compute the offset of the result panel due to panning
set resoffset [expr [lindex [.ql.c bbox resmarker] 0]-$qlvar(xoffs)]
if {$isedit} {
    set qlvar(rescriteria) [lreplace $qlvar(rescriteria) $qlvar(critcol) $qlvar(critcol) $qlvar(critval)]
    .ql.c delete cr-c$qlvar(critcol)-r$qlvar(critrow)
    .ql.c create text [expr $resoffset+4+$qlvar(xoffs)+$qlvar(critcol)*$qlvar(reswidth)] [expr $qlvar(yoffs)+46+15*$qlvar(critrow)] -anchor nw -text $qlvar(critval) -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags [subst {resp cr-c$qlvar(critcol)-r$qlvar(critrow)}]
    set qlvar(critedit) 0
}
catch {destroy .ql.entc}
if {$y<[expr $qlvar(yoffs)+46]} return
if {$x<[expr $qlvar(xoffs)+5]} return
set col [expr int(($x-$qlvar(xoffs)-$resoffset)/$qlvar(reswidth))]
if {$col>=[llength $qlvar(resfields)]} return
set nx [expr $col*$qlvar(reswidth)+8+$qlvar(xoffs)+$resoffset]
set ny [expr $qlvar(yoffs)+76]
# Get the old criteria value
set qlvar(critval) [lindex $qlvar(rescriteria) $col]
entry .ql.entc -textvar qlvar(critval) -borderwidth 0 -background #FFFFFF -highlightthickness 0 -selectborderwidth 0  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
place .ql.entc -x $nx -y $ny -height 14
focus .ql.entc
bind .ql.entc <Button-1> {set qlvar(panstarted) 0}
set qlvar(critcol) $col
set qlvar(critrow) 0
set qlvar(critedit) 1
}

proc mw_save_new_record {} {
global dbc mw tablename msg
if {![mw_exit_edit]} {return 0}
if {$mw(newrec_fields)==""} {return 1}
set msg "Saving new record ..."
after 1000 {set msg ""}
set retval [catch {
	set sqlcmd "insert into $tablename ([join $mw(newrec_fields) ,]) values ([join $mw(newrec_values) ,])"
	set pgres [pg_exec $dbc $sqlcmd]
	} errmsg]
if {$retval} {
	show_error "Error inserting new record\n\n$errmsg"
	return 0
}
set oid [pg_result $pgres -oid]
lappend mw(keylist) $oid
pg_result $pgres -clear
# Get bounds of the last record
set lrbb [.mw.c bbox new]
lappend mw(rowy) [lindex $lrbb 3]
.mw.c itemconfigure new -fill black
.mw.c dtag q new
# Replace * from untouched new row elements with "  "
foreach item [.mw.c find withtag unt] {
	.mw.c itemconfigure $item -text "  "
}
.mw.c dtag q unt
incr mw(last_rownum)
incr mw(nrecs)
mw_draw_new_record
set mw(newrec_fields) {}
set mw(newrec_values) {}
return 1
}

proc save_pref {} {
global pref
catch {
    set fid [open "~/.pgaccessrc" w]
    foreach {opt val} [array get pref] { puts $fid "$opt $val" }
    close $fid
}
}

proc mw_scroll_window {par1 par2 args} {
global mw
if {![mw_exit_edit]} return;
if {$par1=="scroll"} {
    set newtop $mw(toprec)
    if {[lindex $args 0]=="units"} {
        incr newtop $par2
    } else {
        incr newtop [expr $par2*25]
        if {$newtop<0} {set newtop 0}
        if {$newtop>=[expr $mw(nrecs)-1]} {set newtop [expr $mw(nrecs)-1]}
    }
} else {
    set newtop [expr int($par2*$mw(nrecs))]
}
if {$newtop<0} return;
if {$newtop>=[expr $mw(nrecs)-1]} return;
set dy [expr [lindex $mw(rowy) $mw(toprec)]-[lindex $mw(rowy) $newtop]]
.mw.c move q 0 $dy
.mw.c move hgrid 0 $dy
set newrowy {}
foreach y $mw(rowy) {lappend newrowy [expr $y+$dy]}
set mw(rowy) $newrowy
set mw(toprec) $newtop
mw_set_scrollbar
}

proc mw_select_records {sql} {
global dbc field mw
global tablename msg pref
set mw(newrec_fields) {}
set mw(newrec_values) {}
if {![mw_exit_edit]} return;
.mw.c delete q
.mw.c delete header
.mw.c delete hgrid
.mw.c delete new
set mw(leftcol) 0
set mw(leftoffset) 0
set mw(crtrow) {}
set msg {}
set msg "Accessing data. Please wait ..."
cursor_watch .mw
set retval [catch {set pgres [pg_exec $dbc "BEGIN"]} errmsg]
if {!$retval} {
	pg_result $pgres -clear
	set retval [catch {set pgres [pg_exec $dbc "declare mycursor cursor for $sql"]} errmsg]
	if {!$retval} {
		pg_result $pgres -clear
		set retval [catch {set pgres [pg_exec $dbc "fetch $pref(rows) in mycursor"]} errmsg]
	}
}
#set retval [catch {set pgres [pg_exec $dbc $sql]} errmsg]
if {$retval} {
	sql_exec quiet "END"
	set msg {}
    cursor_arrow .mw
    show_error "Error executing SQL command\n\n$sql\n\nError message:$errmsg"
    set msg "Error executing : $sql"
    return
}
if {$mw(updatable)} then {set shift 1} else {set shift 0}
#
# checking at least the numer of fields
set attrlist [pg_result $pgres -lAttributes]
if {$mw(layout_found)} then {
    if {  ($mw(colcount) != [expr [llength $attrlist]-$shift]) ||
          ($mw(colcount) != [llength $mw(colnames)]) ||
          ($mw(colcount) != [llength $mw(colwidth)]) } then {
        # No. of columns don't match, something is wrong
		# tk_messageBox -title Information -message "Layout info changed !\nRescanning..."
        set mw(layout_found) 0
        sql_exec quiet "delete from pga_layout where tablename='$mw(layout_name)'"
    }
}
# Always take the col. names from the result
set mw(colcount) [llength $attrlist]
if {$mw(updatable)} then {incr mw(colcount) -1}
set mw(colnames) {}
# In defmw(colwidth) prepare mw(colwidth) (in case that not layout_found)
set defmw(colwidth) {}
for {set i 0} {$i<$mw(colcount)} {incr i} {
    lappend mw(colnames) [lindex [lindex $attrlist [expr $i+$shift]] 0]
    lappend defmw(colwidth) 150
}
if {!$mw(layout_found)} {
    set mw(colwidth) $defmw(colwidth)
    sql_exec quiet "insert into pga_layout values ('$mw(layout_name)',$mw(colcount),'$mw(colnames)','$mw(colwidth)')"
}
set mw(nrecs) [pg_result $pgres -numTuples]
if {$mw(nrecs)>$pref(rows)} {
	set msg "Only first $pref(rows) records from $mw(nrecs) have been loaded"
	set mw(nrecs) $pref(rows)
}
set tagoid {}
if {$pref(tvfont)=="helv"} {
    set tvfont -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
} else {
    set tvfont -*-Clean-Medium-R-Normal-*-*-130-*-*-*-*-*
}
# Computing column's left edge
set posx 10
for {set j 0} {$j<$mw(colcount)} {incr j} {
	set ledge($j) $posx
	incr posx [expr [lindex $mw(colwidth) $j]+2]
	set textwidth($j) [expr [lindex $mw(colwidth) $j]-5]
}
incr posx -6
set posy 24
mw_draw_headers
set mw(updatekey) oid
set mw(keylist) {}
set mw(rowy) {24}
set msg [time {for {set i 0} {$i<$mw(nrecs)} {incr i} {
    set curtup [pg_result $pgres -getTuple $i]
    if {$mw(updatable)} then {lappend mw(keylist) [lindex $curtup 0]}
    for {set j 0} {$j<$mw(colcount)} {incr j} {
        .mw.c create text $ledge($j) $posy -text [lindex $curtup [expr $j+$shift]] -tags [subst {r$i c$j q}] -anchor nw -font $tvfont -width $textwidth($j)
    }
	set bb [.mw.c bbox r$i]
	incr posy [expr [lindex $bb 3]-[lindex $bb 1]]
	lappend mw(rowy) $posy
	.mw.c create line 0 [lindex $bb 3] $posx [lindex $bb 3] -fill gray -tags [subst {hgrid g$i}]
	if {$i==25} {update; update idletasks}
}
}]
after 2000 set msg {}
set mw(last_rownum) $i
# Defining position for input data
mw_draw_new_record
pg_result $pgres -clear
#set msg {}
sql_exec quiet "END"
set mw(toprec) 0
mw_set_scrollbar
if {$mw(updatable)} then {
    .mw.c bind q <Key> {mw_edit_text %A %K}
} else {
	.mw.c bind q <Key> {}
}
set mw(dirtyrec) 0
#mw_draw_headers
.mw.c raise header
cursor_arrow .mw
}

proc mw_draw_hgrid {} {
global mw
.mw.c delete hgrid
set posx 10
for {set j 0} {$j<$mw(colcount)} {incr j} {
	set ledge($j) $posx
	incr posx [expr [lindex $mw(colwidth) $j]+2]
	set textwidth($j) [expr [lindex $mw(colwidth) $j]-5]
}
incr posx -6
for {set i 0} {$i<$mw(nrecs)} {incr i} {
	.mw.c create line [expr -$mw(leftoffset)] [lindex $mw(rowy) [expr $i+1]] [expr $posx-$mw(leftoffset)] [lindex $mw(rowy) [expr $i+1]] -fill gray -tags [subst {hgrid g$i}]
}
if {$mw(updatable)} {
	set i $mw(nrecs)
	.mw.c create line [expr -$mw(leftoffset)] [lindex $mw(rowy) [expr $i+1]] [expr $posx-$mw(leftoffset)] [lindex $mw(rowy) [expr $i+1]] -fill gray -tags [subst {hgrid g$i}]
}
}

proc mw_set_scrollbar {} {
global mw
if {$mw(nrecs)==0} return;
.mw.sb set [expr $mw(toprec)*1.0/$mw(nrecs)] [expr ($mw(toprec)+27.0)/$mw(nrecs)]
}

proc show_error {emsg} {
tk_messageBox -title Error -icon error -message $emsg
}

proc sql_exec {how cmd} {
global dbc
set retval [catch {set pgr [pg_exec $dbc $cmd]} errmsg]
if { $retval } {
    if {$how != "quiet"} {
        show_error "Error executing query\n\n$cmd\n\nPostgreSQL error message:\n$errmsg"
    }
    return 0
}
pg_result $pgr -clear
return 1
}

proc tab_click {w} {
global dbc tablist activetab
if {$dbc==""} return;
set curtab [$w cget -text]
#if {$activetab==$curtab} return;
.dw.btndesign configure -state disabled
if {$activetab!=""} {
    place .dw.tab$activetab -x 10
    .dw.tab$activetab configure -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
}
$w configure -font -Adobe-Helvetica-Bold-R-Normal-*-*-120-*-*-*-*-*
place $w -x 7
place .dw.lmask -x 80 -y [expr 86+25*[lsearch -exact $tablist $curtab]]
set activetab $curtab
# Tabs where button Design is enabled
if {[lsearch $activetab [list Queries]]!=-1} {
	.dw.btndesign configure -state normal
}
.dw.lb delete 0 end
cmd_$curtab
}

proc main {argc argv} {
global pref newdbname newpport newhost dbc
load libpgtcl.so
catch {draw_tabs}
load_pref
if {$pref(autoload) && ($pref(lastdb)!="")} {
    set newdbname $pref(lastdb)
    set newhost $pref(lasthost)
    set newpport $pref(lastport)
    open_database
}
wm protocol .dw WM_DELETE_WINDOW {
		catch {pg_disconnect $dbc}
		exit
   }
}

proc tiw_show_index {} {
global tiw dbc
set cs [.tiw.ilb curselection]
if {$cs==""} return
set idxname [.tiw.ilb get $cs]
pg_select $dbc "select pg_index.*,pg_class.oid from pg_index,pg_class where pg_class.relname='$idxname' and pg_class.oid=pg_index.indexrelid" rec {
    if {$rec(indisunique)=="t"} {
        set tiw(isunique) Yes
    } else {
        set tiw(isunique) No
    }
    if {$rec(indisclustered)=="t"} {
        set tiw(isclustered) Yes
    } else {
        set tiw(isclustered) No
    }
    set tiw(indexfields) {}
    foreach field $rec(indkey) {
        if {$field!=0} {
#            pg_select $dbc "select attname from pg_attribute where attrelid=$tiw(tableoid) and attnum=$field" rec1 {
#                set tiw(indexfields) "$tiw(indexfields) $rec1(attname)"
#            }
        set tiw(indexfields) "$tiw(indexfields) $tiw(f$field)"
        }

    }
}
set tiw(indexfields) [string trim $tiw(indexfields)]
}

proc Window {args} {
global vTcl
    set cmd [lindex $args 0]
    set name [lindex $args 1]
    set newname [lindex $args 2]
    set rest [lrange $args 3 end]
    if {$name == "" || $cmd == ""} {return}
    if {$newname == ""} {
        set newname $name
    }
    set exists [winfo exists $newname]
    switch $cmd {
        show {
            if {$exists == "1" && $name != "."} {wm deiconify $name; return}
            if {[info procs vTclWindow(pre)$name] != ""} {
                eval "vTclWindow(pre)$name $newname $rest"
            }
            if {[info procs vTclWindow$name] != ""} {
                eval "vTclWindow$name $newname $rest"
            }
            if {[info procs vTclWindow(post)$name] != ""} {
                eval "vTclWindow(post)$name $newname $rest"
            }
        }
        hide    { if $exists {wm withdraw $newname; return} }
        iconify { if $exists {wm iconify $newname; return} }
        destroy { if $exists {destroy $newname; return} }
    }
}

#################################
# VTCL GENERATED GUI PROCEDURES
#

proc vTclWindow. {base} {
    if {$base == ""} {
        set base .
    }
    ###################
    # CREATING WIDGETS
    ###################
    wm focusmodel $base passive
    wm geometry $base 1x1+0+0
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm withdraw $base
    wm title $base "vt.tcl"
    ###################
    # SETTING GEOMETRY
    ###################
}

proc vTclWindow.about {base} {
    if {$base == ""} {
        set base .about
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 471x177+168+243
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm title $base "About"
    label $base.l1 \
        -borderwidth 3 -font -Adobe-Helvetica-Bold-R-Normal-*-*-180-*-*-*-*-* \
        -relief ridge -text PgAccess 
    label $base.l2 \
        -relief groove \
        -text {A Tcl/Tk interface to
PostgreSQL
by Constantin Teodorescu} 
    label $base.l3 \
        -borderwidth 0 \
        -relief sunken -text {vers 0.61}
    label $base.l4 \
        -relief groove \
        -text {You will always get the latest version at:
http://ww.flex.ro/pgaccess

Suggestions : teo@flex.ro} 
    button $base.b1 \
        -borderwidth 1 -command {Window hide .about} \
        -padx 9 \
        -pady 3 -text Ok 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1 \
        -x 10 -y 10 -width 196 -height 103 -anchor nw -bordermode ignore 
    place $base.l2 \
        -x 10 -y 115 -width 198 -height 55 -anchor nw -bordermode ignore 
    place $base.l3 \
        -x 145 -y 80 -anchor nw -bordermode ignore 
    place $base.l4 \
        -x 215 -y 10 -width 246 -height 103 -anchor nw -bordermode ignore 
    place $base.b1 \
        -x 295 -y 130 -width 105 -height 28 -anchor nw -bordermode ignore 
}

proc vTclWindow.dbod {base} {
    if {$base == ""} {
        set base .dbod
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel \
        -cursor top_left_arrow 
    wm focusmodel $base passive
    wm geometry $base 282x128+353+310
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Open database"
    label $base.lhost \
        -borderwidth 0 \
        -relief raised -text Host 
    entry $base.ehost \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable newhost 
    label $base.lport \
        -borderwidth 0 \
        -relief raised -text Port 
    entry $base.epport \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable newpport 
    label $base.ldbname \
        -borderwidth 0 \
        -relief raised -text Database 
    entry $base.edbname \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable newdbname 
    button $base.opbtu \
        -borderwidth 1 -command open_database \
        -padx 9 -pady 3 -text Open 
    button $base.canbut \
        -borderwidth 1 -command {Window hide .dbod} \
        -padx 9 \
        -pady 3 -text Cancel 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.lhost \
        -x 35 -y 7 -anchor nw -bordermode ignore 
    place $base.ehost \
        -x 100 -y 5 -anchor nw -bordermode ignore 
    place $base.lport \
        -x 35 -y 32 -anchor nw -bordermode ignore 
    place $base.epport \
        -x 100 -y 30 -anchor nw -bordermode ignore 
    place $base.ldbname \
        -x 35 -y 57 -anchor nw -bordermode ignore 
    place $base.edbname \
        -x 100 -y 55 -anchor nw -bordermode ignore 
    place $base.opbtu \
        -x 70 -y 90 -width 60 -height 26 -anchor nw -bordermode ignore 
    place $base.canbut \
        -x 150 -y 90 -width 60 -height 26 -anchor nw -bordermode ignore 
}

proc vTclWindow.dw {base} {
    if {$base == ""} {
        set base .dw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel \
        -background #efefef 
    wm focusmodel $base passive
    wm geometry $base 322x355+93+104
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm deiconify $base
    wm title $base "PostgreSQL access"
    label $base.labframe \
        -relief raised 
    listbox $base.lb \
        -background #fefefe \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -highlightthickness 0 -selectborderwidth 0 \
        -yscrollcommand {.dw.sb set} 
    bind $base.lb <Double-Button-1> {
        cmd_Open
    }
    button $base.btnnew \
        -borderwidth 1 -command cmd_New \
        -padx 9 \
        -pady 3 -text New 
    button $base.btnopen \
        -borderwidth 1 -command cmd_Open \
        -padx 9 \
        -pady 3 -text Open 
    button $base.btndesign \
        -borderwidth 1 -command cmd_Design \
        -padx 9 \
        -pady 3 -state disabled -text Design 
    label $base.lmask \
        -borderwidth 0 \
        -relief raised -text {  } 
    label $base.label22 \
        -borderwidth 1 \
        -relief raised 
    menubutton $base.menubutton23 \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -menu .dw.menubutton23.01 -padx 4 -pady 3 -text Database 
    menu $base.menubutton23.01 \
        -borderwidth 1 -cursor {} \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -tearoff 0 
    $base.menubutton23.01 add command \
        \
        -command {set newhost $host
set newpport $pport
Window show .dbod
focus .dbod.edbname} \
        -label Open 
    $base.menubutton23.01 add command \
        \
        -command {.dw.lb delete 0 end
set dbc {}
set dbname {}
set sdbname {}} \
        -label Close 
    $base.menubutton23.01 add command \
        -command cmd_Vacuum -label Vacuum 
    $base.menubutton23.01 add separator
    $base.menubutton23.01 add command \
        -command {cmd_Import_Export Import} -label {Import table} 
    $base.menubutton23.01 add command \
        -command {cmd_Import_Export Export} -label {Export table} 
    $base.menubutton23.01 add separator
    $base.menubutton23.01 add command \
        -command cmd_Preferences -label Preferences 
    $base.menubutton23.01 add separator
    $base.menubutton23.01 add command \
        -command {catch {pg_disconnect $dbc}
save_pref
exit} -label Exit 
    label $base.lshost \
        -relief groove -text localhost -textvariable host 
    label $base.lsdbname \
        -anchor w -relief groove -textvariable sdbname 
    scrollbar $base.sb \
        -borderwidth 1 -command {.dw.lb yview} -orient vert 
    menubutton $base.mnob \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -menu .dw.mnob.m -padx 4 -pady 3 -text Object 
    menu $base.mnob.m \
        -borderwidth 1 -cursor {} \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -tearoff 0 
    $base.mnob.m add command \
        -command cmd_New -label New 
    $base.mnob.m add command \
        -command {cmd_Delete } -label Delete 
    $base.mnob.m add command \
        -command {cmd_Rename } -label Rename 
    $base.mnob.m add command \
        -command cmd_Information -label Information 
    menubutton $base.mhelp \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -menu .dw.mhelp.m -padx 4 -pady 3 -text Help 
    menu $base.mhelp.m \
        -borderwidth 1 -cursor {} \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -tearoff 0 
    $base.mhelp.m add command \
        -label Contents 
    $base.mhelp.m add command \
        -label PostgreSQL 
    $base.mhelp.m add separator
    $base.mhelp.m add command \
        -command {Window show .about} -label About 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.labframe \
        -x 80 -y 30 -width 236 -height 300 -anchor nw -bordermode ignore 
    place $base.lb \
        -x 90 -y 75 -width 205 -height 248 -anchor nw -bordermode ignore 
    place $base.btnnew \
        -x 90 -y 40 -width 60 -height 25 -anchor nw -bordermode ignore 
    place $base.btnopen \
        -x 165 -y 40 -width 60 -height 25 -anchor nw -bordermode ignore 
    place $base.btndesign \
        -x 235 -y 40 -width 60 -height 25 -anchor nw -bordermode ignore 
    place $base.lmask \
        -x 155 -y 45 -width 10 -height 23 -anchor nw -bordermode ignore 
    place $base.label22 \
        -x 0 -y 0 -width 396 -height 23 -anchor nw -bordermode ignore 
    place $base.menubutton23 \
        -x 0 -y 3 -width 63 -height 17 -anchor nw -bordermode ignore 
    place $base.lshost \
        -x 3 -y 335 -width 91 -height 20 -anchor nw -bordermode ignore 
    place $base.lsdbname \
        -x 95 -y 335 -width 223 -height 20 -anchor nw -bordermode ignore 
    place $base.sb \
        -x 295 -y 75 -width 18 -height 249 -anchor nw -bordermode ignore 
    place $base.mnob \
        -x 70 -y 2 -width 44 -height 19 -anchor nw -bordermode ignore 
    place $base.mhelp \
        -x 280 -y 1 -height 20 -anchor nw -bordermode ignore 
}

proc vTclWindow.fw {base} {
    if {$base == ""} {
        set base .fw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 306x288+298+290
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Function"
    label $base.l1 \
        -borderwidth 0 \
        -relief raised -text Name 
    entry $base.e1 \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable funcname 
    label $base.l2 \
        -borderwidth 0 \
        -relief raised -text Parameters 
    entry $base.e2 \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable funcpar 
    label $base.l3 \
        -borderwidth 0 \
        -relief raised -text Returns 
    entry $base.e3 \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable funcret 
    text $base.text1 \
        -background #fefefe -borderwidth 1 \
        -highlightthickness 1 -selectborderwidth 0 -wrap word 
    button $base.okbtn \
        -borderwidth 1 \
        -command {
			if {$funcname==""} {
				show_error "You must supply a name for this function!"
			} elseif {$funcret==""} {
				show_error "You must supply a return type!"
			} else {
				set funcbody [.fw.text1 get 1.0 end]
			    regsub -all "\n" $funcbody " " funcbody
				if {[sql_exec noquiet "create function $funcname ($funcpar) returns $funcret as '$funcbody' language 'sql'"]} {
					Window hide .fw
					tk_messageBox -title PostgreSQL -message "Function created!"
					tab_click .dw.tabFunctions
				}
								
			}
        } \
        -padx 9 \
        -pady 3 -state disabled -text Define 
    button $base.cancelbtn \
        -borderwidth 1 -command {Window hide .fw} \
        -padx 9 \
        -pady 3 -text Close 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1 \
        -x 15 -y 18 -anchor nw -bordermode ignore 
    place $base.e1 \
        -x 95 -y 15 -width 198 -height 22 -anchor nw -bordermode ignore 
    place $base.l2 \
        -x 15 -y 48 -anchor nw -bordermode ignore 
    place $base.e2 \
        -x 95 -y 45 -width 198 -height 22 -anchor nw -bordermode ignore 
    place $base.l3 \
        -x 15 -y 78 -anchor nw -bordermode ignore 
    place $base.e3 \
        -x 95 -y 75 -width 198 -height 22 -anchor nw -bordermode ignore 
    place $base.text1 \
        -x 15 -y 105 -width 275 -height 141 -anchor nw -bordermode ignore 
    place $base.okbtn \
        -x 90 -y 400 -anchor nw -bordermode ignore 
    place $base.cancelbtn \
        -x 160 -y 255 -anchor nw -bordermode ignore 
}

proc vTclWindow.iew {base} {
    if {$base == ""} {
        set base .iew
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 287x151+259+304
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Import-Export table"
    label $base.l1 \
        -borderwidth 0 \
        -relief raised -text {Table name} 
    entry $base.e1 \
        -background #fefefe -borderwidth 1 -textvariable ie_tablename 
    label $base.l2 \
        -borderwidth 0 \
        -relief raised -text {File name} 
    entry $base.e2 \
        -background #fefefe -borderwidth 1 -textvariable ie_filename 
    label $base.l3 \
        -borderwidth 0 \
        -relief raised -text {Field delimiter} 
    entry $base.e3 \
        -background #fefefe -borderwidth 1 -textvariable ie_delimiter 
    button $base.expbtn \
        -borderwidth 1 \
        -command {if {$ie_tablename==""} {
    show_error "You have to supply a table name!"
} elseif {$ie_filename==""} {
    show_error "You have to supply a external file name!"
} else {
	if {$ie_delimiter==""} {
		set sup ""
	} else {
		set sup " USING DELIMITERS '$ie_delimiter'"
	}
	if {[.iew.expbtn cget -text]=="Import"} {
		set oper "FROM"
	} else {
		set oper "TO"
	}
        if {$oicb} {
                set sup2 " WITH OIDS "
        } else {
                set sup2 ""
        }
	set sqlcmd "COPY $ie_tablename $sup2 $oper '$ie_filename'$sup"
        cursor_watch .iew
	if {[sql_exec noquiet $sqlcmd]} {
                cursor_arrow .iew
		tk_messageBox -title Information -message "Operation completed!"
		Window hide .iew
	}
        cursor_arrow .iew
}} \
        -padx 9 \
        -pady 3 -text Export 
    button $base.cancelbtn \
        -borderwidth 1 -command {Window hide .iew} \
        -padx 9 \
        -pady 3 -text Cancel 
    checkbutton $base.oicb \
        -borderwidth 1 \
        -text {with OIDs} -variable oicb 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1 \
        -x 25 -y 15 -anchor nw -bordermode ignore 
    place $base.e1 \
        -x 115 -y 10 -anchor nw -bordermode ignore 
    place $base.l2 \
        -x 25 -y 45 -anchor nw -bordermode ignore 
    place $base.e2 \
        -x 115 -y 40 -anchor nw -bordermode ignore 
    place $base.l3 \
        -x 25 -y 75 -height 18 -anchor nw -bordermode ignore 
    place $base.e3 \
        -x 115 -y 74 -width 33 -height 22 -anchor nw -bordermode ignore 
    place $base.expbtn \
        -x 60 -y 110 -anchor nw -bordermode ignore 
    place $base.cancelbtn \
        -x 155 -y 110 -anchor nw -bordermode ignore 
    place $base.oicb \
        -x 170 -y 75 -anchor nw -bordermode ignore 
}

proc vTclWindow.mw {base} {
    if {$base == ""} {
        set base .mw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel \
        -cursor top_left_arrow 
    wm focusmodel $base passive
    wm geometry $base 631x452+239+226
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Table browser"
    bind $base <Key-Delete> {
        mw_delete_record
    }
    label $base.hoslbl \
        -borderwidth 0 \
        -relief raised -text {Sort field} 
    button $base.fillbtn \
        -borderwidth 1 \
        -command {set nq $mw(query)
if {($mw(isaquery)) && ("$filter$sortfield"!="")} {
    show_error "Sorting and filtering not (yet) available from queries!\n\nPlease enter them in the query definition!"
	set sortfield {}
	set filter {}
} else {
    if {$filter!=""} {
        set nq "$mw(query) where ($filter)"
    } else {
        set nq $mw(query)
    }
    if {$sortfield!=""} {
        set nq "$nq order by $sortfield"
    }
}
if {[mw_save_new_record]} {mw_select_records $nq}
} \
        -padx 9 \
        -pady 3 -text Reload 
    button $base.exitbtn \
        -borderwidth 1 \
        -command {
if {[mw_save_new_record]} {
	.mw.c delete rows
	.mw.c delete header
	set sortfield {}
	set filter {}
	Window hide .mw
}
} \
        -padx 9 \
        -pady 3 -text Close 
    canvas $base.c \
        -background #fefefe -borderwidth 2 -height 207 -highlightthickness 0 \
        -relief ridge -selectborderwidth 0 -takefocus 1 -width 295 
    bind $base.c <Button-1> {
        mw_canvas_click %x %y
    }
    bind $base.c <Button-3> {
        if {[mw_exit_edit]} {mw_save_new_record}
    }
    label $base.msglbl \
        -anchor w -borderwidth 1 \
        -relief sunken -textvariable msg 
    scrollbar $base.sb \
        -borderwidth 1 -command mw_scroll_window -highlightthickness 0 \
        -orient vert 
    button $base.ert \
        -borderwidth 1 -command mw_pan_left \
        -font -Adobe-Helvetica-Bold-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text > 
    button $base.dfggfh \
        -borderwidth 1 -command mw_pan_right \
        -font -Adobe-Helvetica-Bold-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text < 
    entry $base.tbn \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable filter 
    label $base.tbllbl \
        -borderwidth 0 \
        -relief raised -text {Filter conditions} 
    entry $base.dben \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -textvariable sortfield 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.hoslbl \
        -x 5 -y 5 -anchor nw -bordermode ignore 
    place $base.fillbtn \
        -x 515 -y 1 -height 25 -anchor nw -bordermode ignore 
    place $base.exitbtn \
        -x 580 -y 1 -width 49 -height 25 -anchor nw -bordermode ignore 
    place $base.c \
        -x 5 -y 25 -width 608 -height 405 -anchor nw -bordermode ignore 
    place $base.msglbl \
        -x 33 -y 430 -width 567 -height 18 -anchor nw -bordermode ignore 
    place $base.sb \
        -x 612 -y 26 -width 13 -height 404 -anchor nw -bordermode ignore 
    place $base.ert \
        -x 603 -y 428 -width 25 -height 22 -anchor nw -bordermode ignore 
    place $base.dfggfh \
        -x 5 -y 428 -width 25 -height 22 -anchor nw -bordermode ignore 
    place $base.tbn \
        -x 295 -y 3 -width 203 -height 21 -anchor nw -bordermode ignore 
    place $base.tbllbl \
        -x 200 -y 5 -anchor nw -bordermode ignore 
    place $base.dben \
        -x 60 -y 3 -width 120 -height 21 -anchor nw -bordermode ignore 
}

proc vTclWindow.nt {base} {
    if {$base == ""} {
        set base .nt
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 633x270+128+209
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm title $base "Create table"
    entry $base.etabn  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable newtablename 
    bind $base.etabn <Key-Return> {
        focus .nt.e2
    }
    entry $base.e2  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable fldname 
    bind $base.e2 <Key-Return> {
        focus .nt.e1
    }
    entry $base.e1  -background #fefefe -borderwidth 1 -cursor {} -highlightthickness 1  -selectborderwidth 0 -textvariable fldtype 
    bind $base.e1 <Button-1> {
        tk_popup .nt.pop %X %Y
    }
    bind $base.e1 <Key-Return> {
        focus .nt.e5
    }
    bind $base.e1 <Key> {
        tk_popup .nt.pop [expr 150+[winfo rootx .nt]] [expr 65+[winfo rooty .nt]]
    }
    entry $base.e3  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable fldsize 
    bind $base.e3 <Key-Return> {
        focus .nt.e5
    }
    entry $base.e5  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable defaultval 
    bind $base.e5 <Key-Return> {
        focus .nt.cb1
    }
    checkbutton $base.cb1  -borderwidth 1 -offvalue { } -onvalue { NOT NULL} -text {field cannot be null}  -variable notnull 
    label $base.lab1  -borderwidth 0 -relief raised -text {Field type} 
    label $base.lab2  -borderwidth 0 -relief raised -text {Field name} 
    label $base.lab3  -borderwidth 0 -relief raised -text {Field size} 
    label $base.lab4  -borderwidth 0 -relief raised -text {Default value} 
    button $base.addfld  -borderwidth 1 -command add_new_field  -padx 9  -pady 3 -text {Add field} 
    button $base.delfld  -borderwidth 1 -command {catch {.nt.lb delete [.nt.lb curselection]}}  -padx 9  -pady 3 -text {Delete field} 
    button $base.emptb  -borderwidth 1 -command {.nt.lb delete 0 [.nt.lb size]}  -padx 9  -pady 3 -text {Delete all} 
    button $base.maketbl  -borderwidth 1  -command {if {$newtablename==""} then {
    show_error "You must supply a name for your table!"
    focus .nt.etabn
} elseif {[.nt.lb size]==0} then {
    show_error "Your table has no fields!"
    focus .nt.e2
} else {
    set temp "create table $newtablename ([join [.nt.lb get 0 end] ,])"
	cursor_watch .nt
    set retval [catch {
            set pgres [pg_exec $dbc $temp]
            pg_result $pgres -clear
        } errmsg ]
	cursor_arrow .nt
    if {$retval} {
        show_error "Error creating table\n$errmsg"
    } else {
        .nt.lb delete 0 end
        Window hide .nt
        cmd_Tables
    }
}}  -padx 9  -pady 3 -text {Create table} 
    listbox $base.lb  -background #fefefe -borderwidth 1  -font -*-Clean-Medium-R-Normal--*-130-*-*-*-*-*-*  -highlightthickness 1 -selectborderwidth 0  -yscrollcommand {.nt.sb set} 
    bind $base.lb <ButtonRelease-1> {
        if {[.nt.lb curselection]!=""} {
    set fldname [string trim [lindex [split [.nt.lb get [.nt.lb curselection]]] 0]]
}
    }
    button $base.exitbtn  -borderwidth 1 -command {Window hide .nt}  -padx 9  -pady 3 -text Cancel 
    label $base.l1  -anchor w -borderwidth 1 -relief raised -text {field name} 
    label $base.l2  -borderwidth 1 -relief raised -text type 
    label $base.l3  -borderwidth 1 -relief raised -text options 
    scrollbar $base.sb  -borderwidth 1 -command {.nt.lb yview} -orient vert 
    label $base.l93  -borderwidth 0  -relief raised -text {Table name} 
    menu $base.pop  -tearoff 0 
    $base.pop add command   -command {set fldtype char; if {("char"=="varchar")||("char"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -label char 
    $base.pop add command   -command {set fldtype char2; if {("char2"=="varchar")||("char2"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-*  -label char2 
    $base.pop add command   -command {set fldtype char4; if {("char4"=="varchar")||("char4"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-*  -label char4 
    $base.pop add command   -command {set fldtype char8; if {("char8"=="varchar")||("char8"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-*  -label char8 
    $base.pop add command   -command {set fldtype char16; if {("char16"=="varchar")||("char16"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-*  -label char16 
    $base.pop add command   -command {set fldtype varchar; if {("varchar"=="varchar")||("varchar"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-*  -label varchar 
    $base.pop add command   -command {set fldtype text; if {("text"=="varchar")||("text"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -label text 
    $base.pop add command   -command {set fldtype int2; if {("int2"=="varchar")||("int2"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -label int2 
    $base.pop add command   -command {set fldtype int4; if {("int4"=="varchar")||("int4"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -label int4 
    $base.pop add command   -command {set fldtype float4; if {("float4"=="varchar")||("float4"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-*  -label float4 
    $base.pop add command   -command {set fldtype float8; if {("float8"=="varchar")||("float8"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-*  -label float8 
    $base.pop add command   -command {set fldtype date; if {("date"=="varchar")||("date"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -label date 
    $base.pop add command   -command {set fldtype datetime; if {("datetime"=="varchar")||("datetime"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} }  -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-*  -label datetime 
    button $base.mvup  -borderwidth 1  -command {if {[.nt.lb size]>2} {
    set i [.nt.lb curselection]
    if {($i!="")&&($i>0)} {
        .nt.lb insert [expr $i-1] [.nt.lb get $i]
        .nt.lb delete [expr $i+1]
        .nt.lb selection set [expr $i-1]
    }
}}  -padx 9  -pady 3 -text {Move field up} 
    button $base.mvdn  -borderwidth 1  -command {if {[.nt.lb size]>2} {
    set i [.nt.lb curselection]
    if {($i!="")&&($i<[expr [.nt.lb size]-1])} {
        .nt.lb insert [expr $i+2] [.nt.lb get $i]
        .nt.lb delete $i
        .nt.lb selection set [expr $i+1]
    }
}} -padx 9  -pady 3 -text {Move field down} 
    label $base.ll  -borderwidth 1 -relief sunken 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.etabn  -x 95 -y 7 -anchor nw -bordermode ignore 
    place $base.e2  -x 95 -y 40 -anchor nw -bordermode ignore 
    place $base.e1  -x 95 -y 65 -anchor nw -bordermode ignore 
    place $base.e3  -x 95 -y 90 -anchor nw -bordermode ignore 
    place $base.e5  -x 95 -y 115 -anchor nw -bordermode ignore 
    place $base.cb1  -x 95 -y 140 -anchor nw -bordermode ignore 
    place $base.lab1  -x 10 -y 67 -anchor nw -bordermode ignore 
    place $base.lab2  -x 10 -y 42 -anchor nw -bordermode ignore 
    place $base.lab3  -x 10 -y 92 -anchor nw -bordermode ignore 
    place $base.lab4  -x 10 -y 117 -anchor nw -bordermode ignore 
    place $base.addfld  -x 10 -y 175 -anchor nw -bordermode ignore 
    place $base.delfld  -x 85 -y 175 -width 82 -anchor nw -bordermode ignore 
    place $base.emptb  -x 170 -y 175 -anchor nw -bordermode ignore 
    place $base.maketbl  -x 10 -y 235 -width 156 -height 26 -anchor nw -bordermode ignore 
    place $base.lb  -x 260 -y 25 -width 353 -height 236 -anchor nw -bordermode ignore 
    place $base.exitbtn  -x 170 -y 235 -width 77 -height 26 -anchor nw -bordermode ignore 
    place $base.l1  -x 261 -y 9 -width 98 -height 18 -anchor nw -bordermode ignore 
    place $base.l2  -x 360 -y 9 -width 86 -height 18 -anchor nw -bordermode ignore 
    place $base.l3  -x 446 -y 9 -width 166 -height 18 -anchor nw -bordermode ignore 
    place $base.sb  -x 610 -y 25 -width 18 -height 237 -anchor nw -bordermode ignore 
    place $base.l93  -x 10 -y 10 -anchor nw -bordermode ignore 
    place $base.mvup  -x 10 -y 205 -width 118 -height 26 -anchor nw -bordermode ignore 
    place $base.mvdn  -x 130 -y 205 -anchor nw -bordermode ignore 
    place $base.ll  -x 12 -y 165 -width 233 -height 2 -anchor nw -bordermode ignore
}

proc vTclWindow.pw {base} {
    if {$base == ""} {
        set base .pw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 322x167+210+219
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm title $base "Preferences"
    label $base.l1  -borderwidth 0 -relief raised -text {Max rows displayed in table/query view} 
    entry $base.e1  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable pref(rows) 
    label $base.l2  -borderwidth 0 -relief raised -text Font 
    radiobutton $base.tvf  -borderwidth 1 -text {fixed (clean)} -value clean -variable pref(tvfont) 
    radiobutton $base.tvfv  -borderwidth 1 -text {proportional (helvetica)} -value helv -variable pref(tvfont) 
    label $base.ll  -borderwidth 1 -relief sunken 
    checkbutton $base.alcb  -borderwidth 1 -text {Auto-load the last opened database at startup}  -variable pref(autoload) 
    button $base.okbtn  -borderwidth 1  -command {if {$pref(rows)>200} {
tk_messageBox -title Warning -message "A big number of rows displayed in table view will take a lot of memory!"
}
save_pref
Window hide .pw} -padx 9  -pady 3 -text Ok 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1  -x 10 -y 20 -anchor nw -bordermode ignore 
    place $base.e1  -x 245 -y 17 -width 65 -height 24 -anchor nw -bordermode ignore 
    place $base.l2  -x 10 -y 53 -anchor nw -bordermode ignore 
    place $base.tvf  -x 50 -y 50 -anchor nw -bordermode ignore 
    place $base.tvfv  -x 155 -y 50 -anchor nw -bordermode ignore 
    place $base.ll  -x 10 -y 85 -width 301 -height 2 -anchor nw -bordermode ignore 
    place $base.alcb  -x 10 -y 95 -anchor nw -bordermode ignore 
    place $base.okbtn  -x 125 -y 135 -width 80 -height 26 -anchor nw -bordermode ignore
}

proc vTclWindow.qb {base} {
    if {$base == ""} {
        set base .qb
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel \
        -cursor top_left_arrow 
    wm focusmodel $base passive
    wm geometry $base 442x344+277+276
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Query builder"
    label $base.lqn \
        -borderwidth 0 \
        -relief raised -text {Query name} 
    entry $base.eqn \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable queryname 
    button $base.savebtn \
        -borderwidth 1 \
        -command {if {$queryname==""} then {
    show_error "You have to supply a name for this query!"
    focus .qb.eqn
} else {
    set qcmd [.qb.text1 get 1.0 end]
    regsub -all "\n" $qcmd " " qcmd
    regsub -all "'" $qcmd "''" qcmd
    if {$qcmd==""} then {
        show_error "This query has no commands ?"
    } else {
        if { [lindex [split [string toupper [string trim $qcmd]]] 0] == "SELECT" } {
            set qtype S
        } else {
            set qtype A
        }
        if {$cbv} {
            set retval [catch {set pgres [pg_exec $dbc "create view $queryname as $qcmd"]} errmsg]
            if {$retval} {
                show_error "Error defining view\n\n$errmsg"
            } else {
                tab_click .dw.tabViews
                Window hide .qb
            }
        } else {
			cursor_watch .qb
            set retval [catch {
                if {$queryoid==0} then {
                    set pgres [pg_exec $dbc "insert into pga_queries values ('$queryname','$qtype','$qcmd')"]
                } else {
                    set pgres [pg_exec $dbc "update pga_queries set queryname='$queryname',querytype='$qtype',querycommand='$qcmd' where oid=$queryoid"]
                }
            } errmsg]
			cursor_arrow .qb
            if {$retval} then {
                show_error "Error executing query\n$errmsg"
            } else {
                cmd_Queries
                if {$queryoid==0} {set queryoid [pg_result $pgres -oid]}
            }
        }
        catch {pg_result $pgres -clear}
    }
}} \
        -padx 9 -pady 3 -text {Save query definition} 
    button $base.execbtn \
        -borderwidth 1 \
        -command {Window show .mw
set qcmd [.qb.text1 get 0.0 end]
regsub -all "\n" $qcmd " " qcmd
set mw(layout_name) $queryname
mw_load_layout $queryname
set mw(query) $qcmd
set mw(updatable) 0
set mw(isaquery) 1
mw_select_records $qcmd} \
        -padx 9 \
        -pady 3 -text {Execute query} 
    button $base.termbtn \
        -borderwidth 1 \
        -command {.qb.cbv configure -state normal
set cbv 0
set queryname {}
.qb.text1 delete 1.0 end
Window hide .qb} \
        -padx 9 \
        -pady 3 -text Close 
    text $base.text1 \
        -background #fefefe -borderwidth 1 \
        -highlightthickness 1 -wrap word 
    checkbutton $base.cbv \
        -borderwidth 1 \
        -text {Save this query as a view} -variable cbv 
    button $base.qlshow \
        -borderwidth 1 \
        -command {Window show .ql
ql_draw_lizzard
focus .ql.entt} \
        -padx 9 \
        -pady 3 -text {Visual designer} 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.lqn \
        -x 5 -y 5 -anchor nw -bordermode ignore 
    place $base.eqn \
        -x 80 -y 1 -width 355 -height 24 -anchor nw -bordermode ignore 
    place $base.savebtn \
        -x 5 -y 60 -anchor nw -bordermode ignore 
    place $base.execbtn \
        -x 150 -y 60 -anchor nw -bordermode ignore 
    place $base.termbtn \
        -x 375 -y 60 -anchor nw -bordermode ignore 
    place $base.text1 \
        -x 5 -y 90 -width 430 -height 246 -anchor nw -bordermode ignore 
    place $base.cbv \
        -x 5 -y 30 -anchor nw -bordermode ignore 
    place $base.qlshow \
        -x 255 -y 60 -anchor nw -bordermode ignore 
}

proc vTclWindow.ql {base} {
    if {$base == ""} {
        set base .ql
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel  -cursor top_left_arrow 
    wm focusmodel $base passive
    wm geometry $base 759x530+228+154
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm title $base "Visual query designer"
    bind $base <B1-Motion> {
        ql_pan %x %y
    }
    bind $base <Button-1> {
        qlc_click %x %y %W
    }
    bind $base <ButtonRelease-1> {
        ql_dragstop %x %y
    }
    bind $base <Key-Delete> {
        ql_delete_object
    }
    canvas $base.c  -background #fefefe -borderwidth 2 -height 207 -relief ridge  -takefocus 0 -width 295 
    button $base.b1  -borderwidth 1 -command ql_add_new_table  -padx 9  -pady 3 -text {Add table} 
    button $base.exitbtn  -borderwidth 1 -command {ql_init
Window hide .ql}  -padx 9  -pady 3 -text Close 
    button $base.showbtn  -borderwidth 1 -command ql_show_sql -padx 9  -pady 3 -text {Show SQL} 
    label $base.l12  -borderwidth 0 -relief raised -text Table 
    entry $base.entt  -background #fefefe -borderwidth 1 -highlightthickness 1  -selectborderwidth 0 -textvariable qlvar(newtablename) 
    bind $base.entt <Key-Return> {
        ql_add_new_table
    }
    button $base.execbtn  -borderwidth 1  -command {Window show .mw
set qcmd [ql_compute_sql]
set mw(layout_name) nolayoutneeded
mw_load_layout $mw(layout_name)
set mw(query) $qcmd
set mw(updatable) 0
set mw(isaquery) 1
mw_select_records $qcmd} -padx 9  -pady 3 -text {Execute SQL} 
    button $base.stoqb  -borderwidth 1  -command {Window show .qb
.qb.text1 delete 1.0 end
.qb.text1 insert end [ql_compute_sql]
focus .qb}  -padx 9  -pady 3 -text {Save to query builder} 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.c  -x 5 -y 30 -width 748 -height 500 -anchor nw -bordermode ignore 
    place $base.b1  -x 180 -y 5 -height 26 -anchor nw -bordermode ignore 
    place $base.exitbtn  -x 695 -y 5 -height 26 -anchor nw -bordermode ignore 
    place $base.showbtn  -x 367 -y 5 -height 26 -anchor nw -bordermode ignore 
    place $base.l12  -x 10 -y 8 -width 33 -height 16 -anchor nw -bordermode ignore 
    place $base.entt  -x 50 -y 7 -width 126 -height 20 -anchor nw -bordermode ignore 
    place $base.execbtn  -x 452 -y 5 -height 26 -anchor nw -bordermode ignore 
    place $base.stoqb  -x 550 -y 5 -height 26 -anchor nw -bordermode ignore
}

proc vTclWindow.rf {base} {
    if {$base == ""} {
        set base .rf
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 272x105+294+262
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Rename"
    label $base.l1  -borderwidth 0 -relief raised -text {New name} 
    entry $base.e1  -background #fefefe -borderwidth 1 -textvariable newobjname 
    button $base.b1  -borderwidth 1  -command {
			if {$newobjname==""} {
				show_error "You must give object a new name!"
			} elseif {$activetab=="Tables"} {
				set retval [sql_exec noquiet "alter table $oldobjname rename to $newobjname"]
				if {$retval} {
					sql_exec quiet "update pga_layout set tablename='$newobjname' where tablename='$oldobjname'"
					cmd_Tables
					Window hide .rf
				}			
			} elseif {$activetab=="Queries"} {
				set retval [catch {set pgres [pg_exec $dbc "select * from pga_queries where queryname='$newobjname'"]} errmsg]
				if {$retval} {
					show_error $errmsg
				} elseif {[pg_result $pgres -numTuples]>0} {
					show_error "Query $newobjname already exists!"
					pg_result $pgres -clear
				} else {
					pg_result $pgres -clear
					sql_exec noquiet "update pga_queries set queryname='$newobjname' where queryname='$oldobjname'"
					sql_exec noquiet "update pga_layout set tablename='$newobjname' where tablename='$oldobjname'"
					cmd_Queries
					Window hide .rf
				}
			}
       } -padx 9  -pady 3 -text Rename 
    button $base.b2  -borderwidth 1 -command {Window hide .rf} -padx 9  -pady 3 -text Cancel 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1  -x 15 -y 28 -anchor nw -bordermode ignore 
    place $base.e1  -x 100 -y 25 -anchor nw -bordermode ignore 
    place $base.b1  -x 65 -y 65 -width 70 -anchor nw -bordermode ignore 
    place $base.b2  -x 145 -y 65 -width 70 -anchor nw -bordermode ignore
}

proc vTclWindow.sqf {base} {
    if {$base == ""} {
        set base .sqf
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 310x223+245+158
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Sequence"
    label $base.l1  -anchor w -borderwidth 0 -relief raised -text {Sequence name} 
    entry $base.e1  -borderwidth 1 -highlightthickness 1 -textvariable seq_name 
    label $base.l2  -borderwidth 0 -relief raised -text Increment 
    entry $base.e2  -borderwidth 1 -highlightthickness 1 -selectborderwidth 0  -textvariable seq_inc 
    label $base.l3  -borderwidth 0 -relief raised -text {Start value} 
    entry $base.e3  -borderwidth 1 -highlightthickness 1 -selectborderwidth 0  -textvariable seq_start 
    label $base.l4  -borderwidth 0 -relief raised -text Minvalue 
    entry $base.e4  -borderwidth 1 -highlightthickness 1 -selectborderwidth 0  -textvariable seq_minval 
    label $base.l5  -borderwidth 0 -relief raised -text Maxvalue 
    entry $base.e5  -borderwidth 1 -highlightthickness 1 -selectborderwidth 0  -textvariable seq_maxval 
    button $base.defbtn  -borderwidth 1  -command {
        if {$seq_name==""} {
        	show_error "You should supply a name for this sequence"
        } else {
			set s1 {};set s2 {};set s3 {};set s4 {};
			if {$seq_inc!=""} {set s1 "increment $seq_inc"};
			if {$seq_start!=""} {set s2 "start $seq_start"};
			if {$seq_minval!=""} {set s3 "minvalue $seq_minval"};
			if {$seq_maxval!=""} {set s4 "maxvalue $seq_maxval"};
			set sqlcmd "create sequence $seq_name $s1 $s2 $s3 $s4"
			if {[sql_exec noquiet $sqlcmd]} {
				cmd_Sequences
				tk_messageBox -title Information -message "Sequence created!"
			}
        }
    }  -padx 9  -pady 3 -text {Define sequence} 
    button $base.closebtn  -borderwidth 1  -command {for {set i 1} {$i<6} {incr i} {
    .sqf.e$i configure -state normal
    .sqf.e$i delete 0 end
    .sqf.defbtn configure -state normal
    .sqf.l3 configure -text {Start value}
}
place .sqf.defbtn -x 40 -y 175
Window hide .sqf
} -padx 9  -pady 3 -text Close 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1  -x 20 -y 20 -width 111 -height 18 -anchor nw -bordermode ignore 
    place $base.e1  -x 135 -y 19 -anchor nw -bordermode ignore 
    place $base.l2  -x 20 -y 50 -anchor nw -bordermode ignore 
    place $base.e2  -x 135 -y 49 -anchor nw -bordermode ignore 
    place $base.l3  -x 20 -y 80 -anchor nw -bordermode ignore 
    place $base.e3  -x 135 -y 79 -anchor nw -bordermode ignore 
    place $base.l4  -x 20 -y 110 -anchor nw -bordermode ignore 
    place $base.e4  -x 135 -y 109 -anchor nw -bordermode ignore 
    place $base.l5  -x 20 -y 140 -anchor nw -bordermode ignore 
    place $base.e5  -x 135 -y 139 -anchor nw -bordermode ignore 
    place $base.defbtn  -x 40 -y 175 -anchor nw -bordermode ignore 
    place $base.closebtn  -x 195 -y 175 -anchor nw -bordermode ignore
}

proc vTclWindow.tiw {base} {
    if {$base == ""} {
        set base .tiw
    }
    if {[winfo exists $base]} {
        wm deiconify $base; return
    }
    ###################
    # CREATING WIDGETS
    ###################
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 390x460+243+120
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 1 1
    wm title $base "Table information"
    label $base.l1 \
        -borderwidth 0 \
        -relief raised -text {Table name} 
    label $base.l2 \
        -anchor w -borderwidth 0 \
        -relief raised -text conturi -textvariable tiw(tablename) 
    label $base.l3 \
        -borderwidth 0 \
        -relief raised -text Owner 
    label $base.l4 \
        -anchor w -borderwidth 1 \
        -textvariable tiw(owner) 
    listbox $base.lb \
        -background #fefefe -borderwidth 1 \
        -font -*-Clean-Medium-R-Normal--*-130-*-*-*-*-*-* \
        -highlightthickness 1 -selectborderwidth 0 \
        -yscrollcommand {.tiw.sb set} 
    scrollbar $base.sb \
        -activebackground #d9d9d9 -activerelief sunken -borderwidth 1 \
        -command {.tiw.lb yview} -orient vert 
    button $base.closebtn \
        -borderwidth 1 -command {Window hide .tiw} \
        -pady 3 -text Close 
    label $base.l10 \
        -borderwidth 1 \
        -relief raised -text {field name} 
    label $base.l11 \
        -borderwidth 1 \
        -relief raised -text {field type} 
    label $base.l12 \
        -borderwidth 1 \
        -relief raised -text size 
    label $base.lfi \
        -borderwidth 0 \
        -relief raised -text {Field information} 
    label $base.lii \
        -borderwidth 1 \
        -relief raised -text {Indexes defined} 
    listbox $base.ilb \
        -background #fefefe -borderwidth 1 \
        -highlightthickness 1 -selectborderwidth 0 
    bind $base.ilb <ButtonRelease-1> {
        tiw_show_index
    }
    label $base.lip \
        -borderwidth 1 \
        -relief raised -text {index properties} 
    frame $base.fr11 \
        -borderwidth 1 -height 75 -relief sunken -width 125 
    label $base.fr11.l9 \
        -borderwidth 0 \
        -relief raised -text {Is clustered ?} 
    label $base.fr11.l2 \
        -borderwidth 0 \
        -relief raised -text {Is unique ?} 
    label $base.fr11.liu \
        -anchor nw -borderwidth 0 \
        -relief raised -text Yes -textvariable tiw(isunique) 
    label $base.fr11.lic \
        -anchor nw -borderwidth 0 \
        -relief raised -text No -textvariable tiw(isclustered) 
    label $base.fr11.l5 \
        -borderwidth 0 \
        -relief raised -text {Fields :} 
    label $base.fr11.lif \
        -anchor nw -borderwidth 1 \
        -justify left -relief sunken -text cont \
        -textvariable tiw(indexfields) -wraplength 170 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1 \
        -x 20 -y 15 -anchor nw -bordermode ignore 
    place $base.l2 \
        -x 100 -y 14 -width 161 -height 18 -anchor nw -bordermode ignore 
    place $base.l3 \
        -x 20 -y 35 -anchor nw -bordermode ignore 
    place $base.l4 \
        -x 100 -y 34 -width 226 -height 18 -anchor nw -bordermode ignore 
    place $base.lb \
        -x 20 -y 91 -width 338 -height 171 -anchor nw -bordermode ignore 
    place $base.sb \
        -x 355 -y 90 -width 18 -height 173 -anchor nw -bordermode ignore 
    place $base.closebtn \
        -x 325 -y 5 -anchor nw -bordermode ignore 
    place $base.l10 \
        -x 21 -y 75 -width 204 -height 18 -anchor nw -bordermode ignore 
    place $base.l11 \
        -x 225 -y 75 -width 90 -height 18 -anchor nw -bordermode ignore 
    place $base.l12 \
        -x 315 -y 75 -width 41 -height 18 -anchor nw -bordermode ignore 
    place $base.lfi \
        -x 20 -y 55 -anchor nw -bordermode ignore 
    place $base.lii \
        -x 20 -y 280 -width 151 -height 18 -anchor nw -bordermode ignore 
    place $base.ilb \
        -x 20 -y 296 -width 150 -height 148 -anchor nw -bordermode ignore 
    place $base.lip \
        -x 171 -y 280 -width 198 -height 18 -anchor nw -bordermode ignore 
    place $base.fr11 \
        -x 170 -y 297 -width 199 -height 147 -anchor nw -bordermode ignore 
    place $base.fr11.l9 \
        -x 10 -y 30 -anchor nw -bordermode ignore 
    place $base.fr11.l2 \
        -x 10 -y 10 -anchor nw -bordermode ignore 
    place $base.fr11.liu \
        -x 95 -y 10 -width 27 -height 16 -anchor nw -bordermode ignore 
    place $base.fr11.lic \
        -x 95 -y 30 -width 32 -height 16 -anchor nw -bordermode ignore 
    place $base.fr11.l5 \
        -x 10 -y 55 -anchor nw -bordermode ignore 
    place $base.fr11.lif \
        -x 10 -y 70 -width 178 -height 68 -anchor nw -bordermode ignore 
}

Window show .
Window show .dw

main $argc $argv
