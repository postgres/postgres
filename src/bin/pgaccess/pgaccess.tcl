#!/usr/bin/wish
#############################################################################
# Visual Tcl v1.10 Project
#

#################################
# GLOBAL VARIABLES
#
global activetab; 
global dbc; 
global dirty; 
global fldval; 
global host; 
global pport; 
global sdbname; 
global tablist; 
global widget; 

#################################
# USER DEFINED PROCEDURES
#
proc init {argc argv} {
global dbc host pport tablist dirty fldval activetab
set host localhost
set pport 5432
set dbc {}
set tablist [list Tables Queries Views Sequences Functions Reports Scripts]
set activetab {}
set dirty false
set fldval ""
trace variable fldval w mark_dirty
}

init $argc $argv


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
		if {[tk_messageBox -title "FINAL WARNING" -message "Youa re going to delete view:\n\n$objtodelete\n\nProceed ?" -type yesno -default no]=="yes"} {
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

proc cmd_New {} {
global dbc activetab queryname queryoid cbv
if {$dbc==""} return;
switch $activetab {
    Tables {Window show .nt; focus .nt.etabn}
    Queries {
            Window show .qb
            set cbv 0
        }
    Views {
            Window show .qb
            set cbv 1
            .qb.cbv configure -state disabled
        }
    Sequences {
			Window show .sqf
			focus .sqf.e1
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
}
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

proc cursor_arrow {w} {
$w configure -cursor top_left_arrow
update idletasks
}

proc cursor_watch {w} {
$w configure -cursor watch
update idletasks
}

proc drag_it {w x y} {
global draglocation
    if {"$draglocation(obj)" != ""} {
        set dx [expr $x - $draglocation(x)]
        set dy [expr $y - $draglocation(y)]
        $w move $draglocation(obj) $dx $dy
        set draglocation(x) $x
        set draglocation(y) $y
    }
}

proc drag_start {w x y} {
global draglocation
catch {unset draglocation}
set draglocation(obj) [$w find closest $x $y]
set draglocation(x) $x
set draglocation(y) $y
set draglocation(start) $x
}

proc drag_stop {w x y} {
global draglocation colcount colwidth layout_name dbc
    if {"$draglocation(obj)" != ""} {
        set ctr [get_tag_info $draglocation(obj) g]
        set diff [expr $x-$draglocation(start)]
        if {$diff==0} return;
        set newcw {}
        for {set i 0} {$i<$colcount} {incr i} {
            if {$i==$ctr} {
                lappend newcw [expr [lindex $colwidth $i]+$diff]
            } else {
                lappend newcw [lindex $colwidth $i]
            }
        }
        set colwidth $newcw
        draw_headers
        for {set i [expr $ctr+1]} {$i<$colcount} {incr i} {
            .mw.c move c$i $diff 0
        }
		cursor_watch .mw
        sql_exec quiet "update pga_layout set colwidth='$colwidth' where tablename='$layout_name'"
		cursor_arrow .mw
    }
}

proc draw_headers {} {
global colcount colname colwidth

.mw.c delete header
set posx 5
for {set i 0} {$i<$colcount} {incr i} {
    set xf [expr $posx+[lindex $colwidth $i]]
    .mw.c create rectangle $posx 3 $xf 22 -fill lightgray -outline "" -width 0 -tags header
    .mw.c create text [expr $posx+[lindex $colwidth $i]*1.0/2] 14 -text [lindex $colname $i] -tags header -fill navy -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
    .mw.c create line $posx 22 [expr $xf-1] 22 -fill darkgray -tags header
    .mw.c create line [expr $xf-1] 5 [expr $xf-1] 22 -fill darkgray -tags header
    .mw.c create line [expr $xf+1] 5 [expr $xf+1] 22 -fill white -tags header
    .mw.c create line $xf -15000 $xf 15000 -fill gray -tags [subst {header movable g$i}]
    set posx [expr $xf+2]
}
for {set i 0} {$i < 100} {incr i} {
    .mw.c create line 0 [expr 37+$i*14] $posx [expr 37+$i*14] -fill gray -tags header
}
.mw.c bind movable <Button-1> {drag_start %W %x %y}
.mw.c bind movable <B1-Motion> {drag_it %W %x %y}
.mw.c bind movable <ButtonRelease-1> {drag_stop %W %x %y}
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

proc get_dwlb_Selection {} {
set temp [.dw.lb curselection]
if {$temp==""} return "";
return [.dw.lb get $temp]
}

proc get_tag_info {itemid prefix} {
set taglist [.mw.c itemcget $itemid -tags]
set i [lsearch -glob $taglist $prefix*]
set thetag [lindex $taglist $i]
return [string range $thetag 1 end]
}

proc hide_entry {} {
global dirty dbc msg fldval itemid colname tablename

if {$dirty} {
    cursor_watch .mw
    set msg "Saving record ..."
    after 1000 {set msg ""}
    set oid [get_tag_info $itemid o]
    set fld [lindex $colname [get_tag_info $itemid c]]
    set retval [catch {
        set pgr [pg_exec $dbc "update $tablename set $fld='$fldval' where oid=$oid"]
        pg_result $pgr -clear
        } errmsg ]
    cursor_arrow .mw
    if {$retval} {
        show_error "Error updating record:\n$errmsg"
        return
    }
    .mw.c itemconfigure $itemid -text $fldval
}
catch {destroy .mw.entf}
set dirty false
}

proc load_layout {tablename} {
global dbc msg colcount colname colwidth layout_found layout_name

cursor_watch .mw
set layout_name $tablename
catch {unset colcount colname colwidth}
set layout_found false
set retval [catch {set pgres [pg_exec $dbc "select * from pga_layout where tablename='$tablename'"]}]
if {$retval} {
    # Probably table pga_layout isn't yet defined
    sql_exec noquiet "create table pga_layout (tablename varchar(64),nrcols int2,colname text,colwidth text)"
	sql_exec quiet "grant ALL on pga_layout to PUBLIC"
} else {
    if {[pg_result $pgres -numTuples]==1} {
        set layoutinfo [pg_result $pgres -getTuple 0]
        set colcount [lindex $layoutinfo 1]
        set colname  [lindex $layoutinfo 2]
        set colwidth [lindex $layoutinfo 3]
        set layout_found true
    } elseif {[pg_result $pgres -numTuples]>1} {
		show_error "Multiple ([pg_result $pgres -numTuples]) layout info found\n\nPlease report the bug!"
    }
}
catch {pg_result $pgres -clear}
}

proc load_table {tablename} {
global ds_query ds_updatable ds_isaquery sortfield filter
load_layout $tablename
set ds_query "select oid,$tablename.* from $tablename"
set ds_updatable true
set ds_isaquery false
select_records $ds_query
}

proc mark_dirty {name1 name2 op} {
global dirty
set dirty true
}

proc open_database {} {
global dbc host pport dbname sdbname newdbname newhost newpport
cursor_watch .dbod
if {[catch {set newdbc [pg_connect $newdbname -host $newhost -port $newpport]} msg]} {
    cursor_arrow .dbod
    show_error "Error connecting database\n$msg"
} else {
    catch {pg_disconnect $dbc}
    set dbc $newdbc
    set host $newhost
    set pport $newpport
    set dbname $newdbname
	set sdbname $dbname
    cursor_arrow .dbod
    Window hide .dbod
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

proc open_query {how} {
global dbc queryname layout_found queryoid ds_query ds_updatable ds_isaquery sortfield filter

if {[.dw.lb curselection]==""} return;
set queryname [.dw.lb get [.dw.lb curselection]]
if {[catch {set pgres [pg_exec $dbc "select querycommand,querytype,oid from pga_queries where queryname='$queryname'"]}]} then {
    show_error "Error retrieving query definition
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
        load_layout $queryname
        set ds_query $qcmd
        set ds_updatable false
        set ds_isaquery true
        select_records $qcmd
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
global ds_query ds_updatable ds_isaquery
set vn [get_dwlb_Selection]
if {$vn==""} return;
Window show .mw
set ds_query "select * from $vn"
set ds_isaquery false
set ds_updatable false
load_layout $vn
select_records $ds_query
}

proc pan_left {} {
global leftcol leftoffset colwidth colcount
hide_entry
if {$leftcol==[expr $colcount-1]} return;
set diff [expr 2+[lindex $colwidth $leftcol]]
incr leftcol
incr leftoffset $diff
.mw.c move header -$diff 0
.mw.c move rows -$diff 0
}

proc pan_right {} {
global leftcol leftoffset colcount colwidth
hide_entry
if {$leftcol==0} return;
incr leftcol -1
set diff [expr 2+[lindex $colwidth $leftcol]]
incr leftoffset -$diff
.mw.c move header $diff 0
.mw.c move rows $diff 0
}

proc scroll_window {par1 par2 args} {
global nrecs toprec
hide_entry
if {$par1=="scroll"} {
    set newtop $toprec
    if {[lindex $args 0]=="units"} {
        incr newtop $par2
    } else {
        incr newtop [expr $par2*25]
        if {$newtop<0} {set newtop 0}
        if {$newtop>=[expr $nrecs-1]} {set newtop [expr $nrecs-1]}
    }
} else {
    set newtop [expr int($par2*$nrecs)]
}
if {$newtop<0} return;
if {$newtop>=[expr $nrecs-1]} return;
.mw.c move rows 0 [expr 14*($toprec-$newtop)]
set toprec $newtop
set_scrollbar
}

proc select_records {sql} {
global dbc field dirty nrecs toprec colwidth colname colcount ds_updatable
global layout_found layout_name tablename leftcol leftoffset msg
hide_entry
.mw.c delete rows
.mw.c delete header
set leftcol 0
set leftoffset 0
set msg {}
cursor_watch .mw
set retval [catch {set pgres [pg_exec $dbc $sql]} errmsg]
if {$retval} {
    cursor_arrow .mw
    show_error "Error executing SQL command\n\n$sql\n\nError message:$errmsg"
    set msg "Error executing : $sql"
    return
}
if {$ds_updatable} then {set shift 1} else {set shift 0}
#
# checking at least the numer of fields
set attrlist [pg_result $pgres -lAttributes]
if {$layout_found} then {
    if {  ($colcount != [expr [llength $attrlist]-$shift]) ||
          ($colcount != [llength $colname]) ||
          ($colcount != [llength $colwidth]) } then {
        # No. of columns don't match, something is wrong
		show_error "Layout info corrupted!"
        set layout_found false
        sql_exec quiet "delete from pga_layout where tablename='$tablename'"
    }
}
if {$layout_found=="false"} {
    set colcount [llength $attrlist]
    if {$ds_updatable} then {incr colcount -1}
    set colname {}
    set colwidth {}
    for {set i 0} {$i<$colcount} {incr i} {
        lappend colname [lindex [lindex $attrlist [expr $i+$shift]] 0]
        lappend colwidth 150
    }
    sql_exec quiet "insert into pga_layout values ('$layout_name',$colcount,'$colname','$colwidth')"
}
set nrecs [pg_result $pgres -numTuples]
if {$nrecs>200} {
	set msg "Only first 200 records from $nrecs have been loaded"
	set nrecs 200
}
set tagoid {}
for {set i 0} {$i<$nrecs} {incr i} {
    set curtup [pg_result $pgres -getTuple $i]
    if {$ds_updatable} then {set tagoid o[lindex $curtup 0]}
    set posx 10
    for {set j 0} {$j<$colcount} {incr j} {
        set fldtext [lindex $curtup [expr $j+$shift]]
        if {$fldtext==""} {set fldtext "  "};
        .mw.c create text $posx [expr 30+$i*14] -text $fldtext -tags [subst {$tagoid c$j rows}] -anchor w -font -*-Clean-Medium-R-Normal-*-*-130-*-*-*-*-*
        incr posx [expr [lindex $colwidth $j]+2]
    }
}
pg_result $pgres -clear
set toprec 0
set_scrollbar
if {$ds_updatable} then {
	.mw.c bind rows <Button-1> {show_entry [%W find closest %x %y]}
} else {
	.mw.c bind rows <Button-1> {bell}
}
set dirty false
draw_headers
cursor_arrow .mw
}

proc set_scrollbar {} {
global nrecs toprec

if {$nrecs==0} return;
.mw.sb set [expr $toprec*1.0/$nrecs] [expr ($toprec+27.0)/$nrecs]
}

proc show_entry {id} {
global dirty fldval msg itemid colname colwidth

hide_entry
set itemid $id
set colidx [get_tag_info $id c]
set fldval [.mw.c itemcget $id -text]
set dirty false
set coord [.mw.c coords $id]
entry .mw.entf -textvar fldval -width [expr int(([lindex $colwidth $colidx]-5)/6.2)] -borderwidth 0 -background #ddfefe  -highlightthickness 0 -selectborderwidth 0  -font -*-Clean-Medium-R-Normal-*-*-130-*-*-*-*-*;
place .mw.entf -x [expr 4+[lindex $coord 0]] -y [expr 18+[lindex $coord 1]];
focus .mw.entf
bind .mw.entf <Return> {hide_entry}
bind .mw.entf <Escape> {set dirty false;hide_entry;set msg {}}
set msg "Editing field [lindex $colname $colidx]"
after 2000 {set msg ""}
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
if {$activetab!=""} {
    place .dw.tab$activetab -x 10
    .dw.tab$activetab configure -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
}
$w configure -font -Adobe-Helvetica-Bold-R-Normal-*-*-120-*-*-*-*-*
place $w -x 7
place .dw.lmask -x 80 -y [expr 86+25*[lsearch -exact $tablist $curtab]]
set activetab $curtab
.dw.lb delete 0 end
cmd_$curtab
}

proc main {argc argv} {
load libpgtcl.so
catch {draw_tabs}
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
        -relief ridge -text PGACCESS 
    label $base.l2 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief groove \
        -text {A Tcl/Tk interface to
PostgreSQL
by Constantin Teodorescu} 
    label $base.l3 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief sunken -text {vers 0.3} 
    label $base.l4 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief groove \
        -text {You will always get the latest version at:
http://ww.flex.ro/pgaccess

Suggestions : teo@flex.ro} 
    button $base.b1 \
        -borderwidth 1 -command {Window hide .about} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
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
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 282x128+353+310
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Open database"
    label $base.lhost \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text Host 
    entry $base.ehost \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable newhost 
    label $base.lport \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text Port 
    entry $base.epport \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable newpport 
    label $base.ldbname \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text Database 
    entry $base.edbname \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable newdbname 
    button $base.opbtu \
        -borderwidth 1 -command open_database \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Open 
    button $base.canbut \
        -borderwidth 1 -command {Window hide .dbod} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
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
    wm geometry $base 322x355+147+218
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm deiconify $base
    wm title $base "PostgreSQL access"
    label $base.labframe \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised 
    listbox $base.lb \
        -background #fefefe \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -highlightthickness 0 -selectborderwidth 0 \
        -yscrollcommand {.dw.sb set} 
    button $base.btnnew \
        -borderwidth 1 -command cmd_New \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text New 
    button $base.btnopen \
        -borderwidth 1 -command cmd_Open \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Open 
    button $base.btndesign \
        -borderwidth 1 -command cmd_Design \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Design 
    label $base.lmask \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {  } 
    label $base.label22 \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
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
        -label Open -state active 
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
        -command { catch {pg_disconnect $dbc}; exit } -label Exit 
    label $base.lshost \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief groove -text localhost -textvariable host 
    label $base.lsdbname \
        -anchor w -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief groove -textvariable sdbname 
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
        -command cmd_New -label New -state active 
    $base.mnob.m add command \
        -command {cmd_Delete } -label Delete 
    $base.mnob.m add command \
        -command {cmd_Rename } -label Rename 
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
        -x 155 -y 45 -height 23 -anchor nw -bordermode ignore 
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
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Table name} 
    entry $base.e1 \
        -background #fefefe -borderwidth 1 -textvariable ie_tablename 
    label $base.l2 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {File name} 
    entry $base.e2 \
        -background #fefefe -borderwidth 1 -textvariable ie_filename 
    label $base.l3 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
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
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Export 
    button $base.cancelbtn \
        -borderwidth 1 -command {Window hide .iew} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Cancel 
    checkbutton $base.oicb \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
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
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 631x452+152+213
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Table browser"
    label $base.hoslbl \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Sort field} 
    button $base.fillbtn \
        -borderwidth 1 \
        -command {set nq $ds_query
if {($ds_isaquery=="true") && ("$filter$sortfield"!="")} {
    show_error "Sorting and filtering not (yet) available from queries!\n\nPlease enter them in the query definition!"
	set sortfield {}
	set filter {}
} else {
    if {$filter!=""} {
        set nq "$ds_query where ($filter)"
    } else {
        set nq $ds_query
    }
    if {$sortfield!=""} {
        set nq "$nq order by $sortfield"
    }
}
select_records $nq} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Reload 
    button $base.exitbtn \
        -borderwidth 1 \
        -command {.mw.c delete rows
.mw.c delete header
set sortfield {}
set filter {}
Window hide .mw} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Close 
    canvas $base.c \
        -background #fefefe -borderwidth 2 -height 207 -relief ridge \
        -width 295 
    label $base.msglbl \
        -anchor w -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief sunken -textvariable msg 
    scrollbar $base.sb \
        -borderwidth 1 -command scroll_window -orient vert 
    button $base.ert \
        -borderwidth 1 -command pan_left \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text < 
    button $base.dfggfh \
        -borderwidth 1 -command pan_right \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text > 
    entry $base.tbn \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable filter 
    label $base.tbllbl \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
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
        -x 487 -y 1 -height 25 -anchor nw -bordermode ignore 
    place $base.exitbtn \
        -x 590 -y 1 -width 39 -height 25 -anchor nw -bordermode ignore 
    place $base.c \
        -x 5 -y 25 -width 608 -height 405 -anchor nw -bordermode ignore 
    place $base.msglbl \
        -x 9 -y 430 -width 616 -height 18 -anchor nw -bordermode ignore 
    place $base.sb \
        -x 610 -y 26 -width 18 -height 404 -anchor nw -bordermode ignore 
    place $base.ert \
        -x 552 -y 1 -width 20 -height 25 -anchor nw -bordermode ignore 
    place $base.dfggfh \
        -x 570 -y 1 -width 20 -height 25 -anchor nw -bordermode ignore 
    place $base.tbn \
        -x 280 -y 3 -width 203 -height 21 -anchor nw -bordermode ignore 
    place $base.tbllbl \
        -x 180 -y 5 -anchor nw -bordermode ignore 
    place $base.dben \
        -x 65 -y 3 -width 81 -height 21 -anchor nw -bordermode ignore 
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
    wm geometry $base 628x239+143+203
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Create table"
    entry $base.etabn \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable newtablename 
    bind $base.etabn <Key-Return> {
        focus .nt.e2
    }
    entry $base.e2 \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable fldname 
    bind $base.e2 <Key-Return> {
        focus .nt.e1
    }
    entry $base.e1 \
        -background #fefefe -borderwidth 1 -cursor {} -highlightthickness 1 \
        -selectborderwidth 0 -textvariable fldtype 
    bind $base.e1 <Button-1> {
        tk_popup .nt.pop %X %Y
    }
    bind $base.e1 <Key-Return> {
        focus .nt.e5
    }
    bind $base.e1 <Key> {
        tk_popup .nt.pop [expr 150+[winfo rootx .nt]] [expr 65+[winfo rooty .nt]]
    }
    entry $base.e3 \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -state disabled -textvariable fldsize 
    bind $base.e3 <Key-Return> {
        focus .nt.e5
    }
    entry $base.e5 \
        -background #fefefe -borderwidth 1 -highlightthickness 1 \
        -selectborderwidth 0 -textvariable defaultval 
    bind $base.e5 <Key-Return> {
        focus .nt.cb1
    }
    checkbutton $base.cb1 \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -offvalue { } -onvalue { NOT NULL} -text {field cannot be null} \
        -variable notnull 
    label $base.lab1 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Field type} 
    label $base.lab2 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Field name} 
    label $base.lab3 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Field size} 
    label $base.lab4 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Default value} 
    button $base.addfld \
        -borderwidth 1 \
        -command {if {$fldname==""} {
    show_error "Enter a field name"
    focus .nt.e2
} elseif {$fldtype==""} {
    show_error "The field type is not specified!"
} elseif {(($fldtype=="varchar")||($fldtype=="char"))&&($fldsize=="")} {
    focus .nt.e3
    show_error "You must specify field size!"
} else {
  if {$fldsize==""} then {set sup ""} else {set sup "($fldsize)"}
  if {$defaultval==""} then {set sup2 ""} else {set sup2 " DEFAULT '$defaultval'"}
  .nt.lb insert end [format "%-17s%-14s%-16s" $fldname $fldtype$sup $sup2$notnull]
  focus .nt.e2
  set fldname {}
  set fldsize {}
  set defaultval {}
}} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Add field} 
    button $base.delfld \
        -borderwidth 1 -command {catch {.nt.lb delete [.nt.lb curselection]}} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Delete field} 
    button $base.emptb \
        -borderwidth 1 -command {.nt.lb delete 0 [.nt.lb size]} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Delete all} 
    button $base.maketbl \
        -borderwidth 1 \
        -command {if {$newtablename==""} then {
    show_error "You must supply a name for your table!"
    focus .nt.etabn
} elseif {[.nt.lb size]==0} then {
    show_error "Your table has no fields!"
    focus .nt.e2
} else {
    set temp "create table $newtablename ([join [.nt.lb get 0 end] ,])"
    set retval [catch {
            set pgres [pg_exec $dbc $temp]
            pg_result $pgres -clear
        } errmsg ]
    if {$retval} {
        show_error "Error creating table\n$errmsg"
    } else {
        .nt.lb delete 0 end
        Window hide .nt
        cmd_Tables
    }
}} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Create table} 
    listbox $base.lb \
        -background #fefefe -borderwidth 1 \
        -font -*-Clean-Medium-R-Normal--*-130-*-*-*-*-*-* \
        -highlightthickness 1 -selectborderwidth 0 \
        -yscrollcommand {.nt.sb set} 
    button $base.exitbtn \
        -borderwidth 1 -command {Window hide .nt} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Cancel 
    label $base.l1 \
        -anchor w -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {field name} 
    label $base.l2 \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text type 
    label $base.l3 \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text options 
    scrollbar $base.sb \
        -borderwidth 1 -command {.nt.lb yview} -orient vert 
    label $base.l93 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Table name} 
    menu $base.pop \
        -tearoff 0 
    $base.pop add command \
        \
        -command {set fldtype char; if {("char"=="varchar")||("char"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} } \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -label char 
    $base.pop add command \
        \
        -command {set fldtype char4; if {("char4"=="varchar")||("char4"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} } \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -label char4 
    $base.pop add command \
        \
        -command {set fldtype char8; if {("char8"=="varchar")||("char8"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} } \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -label char8 
    $base.pop add command \
        \
        -command {set fldtype char16; if {("char16"=="varchar")||("char16"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} } \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -label char16 
    $base.pop add command \
        \
        -command {set fldtype varchar; if {("varchar"=="varchar")||("varchar"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} } \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -label varchar 
    $base.pop add command \
        \
        -command {set fldtype text; if {("text"=="varchar")||("text"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} } \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -label text 
    $base.pop add command \
        \
        -command {set fldtype int2; if {("int2"=="varchar")||("int2"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} } \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -label int2 
    $base.pop add command \
        \
        -command {set fldtype int4; if {("int4"=="varchar")||("int4"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} } \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -label int4 
    $base.pop add command \
        \
        -command {set fldtype float4; if {("float4"=="varchar")||("float4"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} } \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -label float4 
    $base.pop add command \
        \
        -command {set fldtype float8; if {("float8"=="varchar")||("float8"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} } \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -label float8 
    $base.pop add command \
        \
        -command {set fldtype date; if {("date"=="varchar")||("date"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} } \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* -label date 
    $base.pop add command \
        \
        -command {set fldtype datetime; if {("datetime"=="varchar")||("datetime"=="char")} then {.nt.e3 configure -state normal;focus .nt.e3} else {.nt.e3 configure -state disabled;focus .nt.e5} } \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -label datetime 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.etabn \
        -x 95 -y 7 -anchor nw -bordermode ignore 
    place $base.e2 \
        -x 95 -y 40 -anchor nw -bordermode ignore 
    place $base.e1 \
        -x 95 -y 65 -anchor nw -bordermode ignore 
    place $base.e3 \
        -x 95 -y 90 -anchor nw -bordermode ignore 
    place $base.e5 \
        -x 95 -y 115 -anchor nw -bordermode ignore 
    place $base.cb1 \
        -x 95 -y 135 -anchor nw -bordermode ignore 
    place $base.lab1 \
        -x 10 -y 67 -anchor nw -bordermode ignore 
    place $base.lab2 \
        -x 10 -y 45 -anchor nw -bordermode ignore 
    place $base.lab3 \
        -x 10 -y 93 -anchor nw -bordermode ignore 
    place $base.lab4 \
        -x 10 -y 118 -anchor nw -bordermode ignore 
    place $base.addfld \
        -x 10 -y 175 -anchor nw -bordermode ignore 
    place $base.delfld \
        -x 90 -y 175 -width 82 -anchor nw -bordermode ignore 
    place $base.emptb \
        -x 175 -y 175 -anchor nw -bordermode ignore 
    place $base.maketbl \
        -x 10 -y 205 -width 161 -height 26 -anchor nw -bordermode ignore 
    place $base.lb \
        -x 260 -y 25 -width 353 -height 206 -anchor nw -bordermode ignore 
    place $base.exitbtn \
        -x 175 -y 205 -width 77 -height 26 -anchor nw -bordermode ignore 
    place $base.l1 \
        -x 261 -y 9 -width 98 -height 18 -anchor nw -bordermode ignore 
    place $base.l2 \
        -x 360 -y 9 -width 86 -height 18 -anchor nw -bordermode ignore 
    place $base.l3 \
        -x 446 -y 9 -width 166 -height 18 -anchor nw -bordermode ignore 
    place $base.sb \
        -x 610 -y 25 -width 18 -height 207 -anchor nw -bordermode ignore 
    place $base.l93 \
        -x 10 -y 10 -anchor nw -bordermode ignore 
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
    toplevel $base -class Toplevel
    wm focusmodel $base passive
    wm geometry $base 442x344+258+271
    wm maxsize $base 1009 738
    wm minsize $base 1 1
    wm overrideredirect $base 0
    wm resizable $base 0 0
    wm title $base "Query builder"
    label $base.lqn \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
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
            set retval [catch {
                if {$queryoid==0} then {
                    set pgres [pg_exec $dbc "insert into pga_queries values ('$queryname','$qtype','$qcmd')"]
                } else {
                    set pgres [pg_exec $dbc "update pga_queries set queryname='$queryname',querytype='$qtype',querycommand='$qcmd' where oid=$queryoid"]
                }
            } errmsg]
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
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Save query definition} 
    button $base.execbtn \
        -borderwidth 1 \
        -command {Window show .mw
set qcmd [.qb.text1 get 0.0 end]
regsub -all "\n" $qcmd " " qcmd
set layout_name $queryname
load_layout $queryname
set ds_query $qcmd
set ds_updatable false
set ds_isaquery true
select_records $qcmd} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Execute query} 
    button $base.termbtn \
        -borderwidth 1 \
        -command {.qb.cbv configure -state normal
set cbv 0
set queryname {}
.qb.text1 delete 1.0 end
Window hide .qb} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Close 
    text $base.text1 \
        -background #fefefe -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -highlightthickness 1 
    checkbutton $base.cbv \
        -borderwidth 1 \
        -font -Adobe-Helvetica-Medium-R-Normal--*-120-*-*-*-*-*-* \
        -text {Save this query as a view} -variable cbv 
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
        -x 380 -y 60 -anchor nw -bordermode ignore 
    place $base.text1 \
        -x 5 -y 90 -width 430 -height 246 -anchor nw -bordermode ignore 
    place $base.cbv \
        -x 5 -y 30 -anchor nw -bordermode ignore 
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
    label $base.l1 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {New name} 
    entry $base.e1 \
        -background #fefefe -borderwidth 1 -textvariable newobjname 
    button $base.b1 \
        -borderwidth 1 \
        -command {
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
       } \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Rename 
    button $base.b2 \
        -borderwidth 1 -command {Window hide .rf} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Cancel 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1 \
        -x 15 -y 28 -anchor nw -bordermode ignore 
    place $base.e1 \
        -x 100 -y 25 -anchor nw -bordermode ignore 
    place $base.b1 \
        -x 65 -y 65 -width 70 -anchor nw -bordermode ignore 
    place $base.b2 \
        -x 145 -y 65 -width 70 -anchor nw -bordermode ignore 
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
    label $base.l1 \
        -anchor w -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Sequence name} 
    entry $base.e1 \
        -borderwidth 1 -highlightthickness 1 -textvariable seq_name 
    label $base.l2 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text Increment 
    entry $base.e2 \
        -borderwidth 1 -highlightthickness 1 -selectborderwidth 0 \
        -textvariable seq_inc 
    label $base.l3 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text {Start value} 
    entry $base.e3 \
        -borderwidth 1 -highlightthickness 1 -selectborderwidth 0 \
        -textvariable seq_start 
    label $base.l4 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text Minvalue 
    entry $base.e4 \
        -borderwidth 1 -highlightthickness 1 -selectborderwidth 0 \
        -textvariable seq_minval 
    label $base.l5 \
        -borderwidth 0 \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* \
        -relief raised -text Maxvalue 
    entry $base.e5 \
        -borderwidth 1 -highlightthickness 1 -selectborderwidth 0 \
        -textvariable seq_maxval 
    button $base.defbtn \
        -borderwidth 1 \
        -command {
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
    } \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Define sequence} 
    button $base.closebtn \
        -borderwidth 1 \
        -command {for {set i 1} {$i<6} {incr i} {
    .sqf.e$i configure -state normal
    .sqf.e$i delete 0 end
    .sqf.defbtn configure -state normal
    .sqf.l3 configure -text {Start value}
}
place .sqf.defbtn -x 40 -y 175
Window hide .sqf
} \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text Close 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.l1 \
        -x 20 -y 20 -width 111 -height 18 -anchor nw -bordermode ignore 
    place $base.e1 \
        -x 135 -y 19 -anchor nw -bordermode ignore 
    place $base.l2 \
        -x 20 -y 50 -anchor nw -bordermode ignore 
    place $base.e2 \
        -x 135 -y 49 -anchor nw -bordermode ignore 
    place $base.l3 \
        -x 20 -y 80 -anchor nw -bordermode ignore 
    place $base.e3 \
        -x 135 -y 79 -anchor nw -bordermode ignore 
    place $base.l4 \
        -x 20 -y 110 -anchor nw -bordermode ignore 
    place $base.e4 \
        -x 135 -y 109 -anchor nw -bordermode ignore 
    place $base.l5 \
        -x 20 -y 140 -anchor nw -bordermode ignore 
    place $base.e5 \
        -x 135 -y 139 -anchor nw -bordermode ignore 
    place $base.defbtn \
        -x 40 -y 175 -anchor nw -bordermode ignore 
    place $base.closebtn \
        -x 195 -y 175 -anchor nw -bordermode ignore 
}

Window show .
Window show .dw

main $argc $argv
