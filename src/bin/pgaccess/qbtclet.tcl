#################################
# GLOBAL VARIABLES
#
global qlvar; 
global widget; 

#################################
# USER DEFINED PROCEDURES
#
proc init {argc argv} {
global qlvar
set qlvar(yoffs) 360
set qlvar(xoffs) 50
set qlvar(reswidth) 150
}

init $argc $argv

proc main {argc argv} {

}

proc show_message {usrmsg} {
global msg
set msg $usrmsg
after 2000 {set msg {}}
}

proc ql_delete_object {} {
global qlvar
# Checking if there 
set obj [.c find withtag hili]
if {$obj==""} return
if {[ql_get_tag_info $obj link]=="s"} {
#    if {[tk_messageBox -title WARNING -icon question -message "Remove link ?" -type yesno -default no]=="no"} return
	show_message "Deleting the link from tables ..."
    set linkid [ql_get_tag_info $obj lkid]
    set qlvar(links) [lreplace $qlvar(links) $linkid $linkid]
    .c delete links
    ql_draw_links
} else {
    set tablename [ql_get_tag_info $obj tab]
    if {$tablename==""} return
#    if {[tk_messageBox -title WARNING -icon question -message "Remove table $tablename from query ?" -type yesno -default no]=="no"} return
	show_message "Deleting table from query ..."
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
    .c delete tab$tablename
    .c delete links
    ql_draw_links
    ql_draw_res_panel
}
}

proc ql_dragit {w x y} {
global draginfo
if {"$draginfo(obj)" != ""} {
    set dx [expr $x - $draginfo(x)]
    set dy [expr $y - $draginfo(y)]
    if {$draginfo(is_a_table)} {
        set taglist [.c gettags $draginfo(obj)]
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
. configure -cursor hand1
.c raise $draginfo(obj)
set draginfo(table) 0
if {[ql_get_tag_info $draginfo(obj) table]=="header"} {
    set draginfo(is_a_table) 1
    .c itemconfigure [.c find withtag hili] -fill black
    .c dtag [.c find withtag hili] hili
    .c addtag hili withtag $draginfo(obj)
    .c itemconfigure hili -fill blue
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
. configure -cursor top_left_arrow
set este {}
catch {set este $draginfo(obj)}
if {$este==""} return
# Re-establish the normal paint order so
# information won't be overlapped by table rectangles
# or link linkes
.c lower $draginfo(obj)
.c lower rect
.c lower links
set qlvar(panstarted) 0
if {$draginfo(is_a_table)} {
    set draginfo(obj) {}
    .c delete links
    ql_draw_links
    return
}
.c move $draginfo(obj) [expr $draginfo(sx)-$x] [expr $draginfo(sy)-$y]
if {($y>$qlvar(yoffs)) && ($x>$qlvar(xoffs))} {
    # Drop position : inside the result panel
    # Compute the offset of the result panel due to panning
    set resoffset [expr [lindex [.c bbox resmarker] 0]-$qlvar(xoffs)]
    set newfld [.c itemcget $draginfo(obj) -text]
    set tabtag [ql_get_tag_info $draginfo(obj) tab]
    set col [expr int(($x-$qlvar(xoffs)-$resoffset)/$qlvar(reswidth))]
    set qlvar(resfields) [linsert $qlvar(resfields) $col $newfld]
    set qlvar(ressort) [linsert $qlvar(ressort) $col unsorted]
    set qlvar(rescriteria) [linsert $qlvar(rescriteria) $col {}]
    set qlvar(restables) [linsert $qlvar(restables) $col $tabtag]
    ql_draw_res_panel    
} else {
    # Drop position : in the table panel
    set droptarget [.c find overlapping $x $y $x $y]
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
.c delete links
set i 0
foreach link $qlvar(links) {
    # Compute the source and destination right edge
    set sre [lindex [.c bbox tab[lindex $link 0]] 2]
    set dre [lindex [.c bbox tab[lindex $link 2]] 2]
    # Compute field bound boxes
    set sbbox [.c bbox [lindex $link 4]]
    set dbbox [.c bbox [lindex $link 5]]
    # Compute the auxiliary lines
    if {[lindex $sbbox 2] < [lindex $dbbox 0]} {
        # Source object is on the left of target object
        set x1 $sre
        set y1 [expr ([lindex $sbbox 1]+[lindex $sbbox 3])/2]
        .c create line $x1 $y1 [expr $x1+10] $y1 -tags [subst {links lkid$i}] -width 3
        set x2 [lindex $dbbox 0]
        set y2 [expr ([lindex $dbbox 1]+[lindex $dbbox 3])/2]
        .c create line [expr $x2-10] $y2 $x2 $y2 -tags {links} -width 3
        .c create line [expr $x1+10] $y1 [expr $x2-10] $y2 -tags [subst {links lkid$i}] -width 2
    } else {
        # source object is on the right of target object
        set x1 [lindex $sbbox 0]
        set y1 [expr ([lindex $sbbox 1]+[lindex $sbbox 3])/2]
        .c create line $x1 $y1 [expr $x1-10] $y1 -tags [subst {links lkid$i}] -width 3
        set x2 $dre
        set y2 [expr ([lindex $dbbox 1]+[lindex $dbbox 3])/2]
        .c create line $x2 $y2 [expr $x2+10] $y2 -width 3 -tags [subst {links lkid$i}]
        .c create line [expr $x1-10] $y1 [expr $x2+10] $y2 -tags [subst {links lkid$i}] -width 2
    }
    incr i
}
.c lower links
.c bind links <Button-1> {ql_link_click %x %y}
}

proc ql_draw_lizzard {} {
global qlvar
ql_read_struct
.c delete all
set posx 20
for {set it 0} {$it<$qlvar(ntables)} {incr it} {
    ql_draw_table $it
#    set posy 10
#    set tablename $qlvar(tablename$it)
#    .c create text $posx $posy -text $tablename -anchor nw -tags [subst {tab$tablename f-oid mov tableheader}] -font -Adobe-Helvetica-Bold-R-Normal-*-*-120-*-*-*-*-*
#    incr posy 16
#    foreach fld $qlvar(tablestruct$it) {
#        .c create text $posx $posy -text $fld -anchor nw -tags [subst {f-$fld tab$tablename mov}] -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
#        incr posy 14
#    }
#    set reg [.c bbox tab$tablename]
#    .c create rectangle [lindex $reg 0] [lindex $reg 1] [lindex $reg 2] [lindex $reg 3] -fill #EEEEEE -tags [subst {rect tab$tablename}]
#    .c create line [lindex $reg 0] [expr [lindex $reg 1]+15] [lindex $reg 2] [expr [lindex $reg 1]+15] -tags [subst {rect tab$tablename}]
#    set posx [expr $posx+40+[lindex $reg 2]-[lindex $reg 0]]
}
.c lower rect
.c create line 0 $qlvar(yoffs) 10000 $qlvar(yoffs) -width 3
.c create rectangle 0 $qlvar(yoffs) 10000 5000 -fill #FFFFFF
for {set i [expr 15+$qlvar(yoffs)]} {$i<500} {incr i 15} {
    .c create line $qlvar(xoffs) $i 10000 $i -fill #CCCCCC -tags {resgrid}
}    
for {set i $qlvar(xoffs)} {$i<10000} {incr i $qlvar(reswidth)} {
    .c create line $i [expr 1+$qlvar(yoffs)] $i 10000 -fill #cccccc -tags {resgrid}
}
# Make a marker for result panel offset calculations (due to panning)
.c create line $qlvar(xoffs) $qlvar(yoffs) $qlvar(xoffs) 500 -tags {resmarker resgrid}
.c create rectangle 0 $qlvar(yoffs) $qlvar(xoffs) 5000 -fill #EEEEEE -tags {reshdr}
.c create text 5 [expr 1+$qlvar(yoffs)] -text Field: -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags {reshdr}
.c create text 5 [expr 16+$qlvar(yoffs)] -text Table: -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags {reshdr}
.c create text 5 [expr 31+$qlvar(yoffs)] -text Sort: -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags {reshdr}
.c create text 5 [expr 46+$qlvar(yoffs)] -text Criteria: -anchor nw -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags {reshdr}
.c bind mov <Button-1> {ql_dragstart %W %x %y}
.c bind mov <B1-Motion> {ql_dragit %W %x %y}
bind . <ButtonRelease-1> {ql_dragstop %x %y}
bind . <Button-1> {qlc_click %x %y %W}
bind . <B1-Motion> {ql_pan %x %y}
bind . <Key-Delete> {ql_delete_object}
set qlvar(resfields) {}
set qlvar(ressort) {}
set qlvar(rescriteria) {}
set qlvar(restables) {}
set qlvar(critedit) 0
set qlvar(links) {}
set qlvar(linktodelete) {}
}

proc ql_draw_res_panel {} {
global qlvar
# Compute the offset of the result panel due to panning
set resoffset [expr [lindex [.c bbox resmarker] 0]-$qlvar(xoffs)]
    .c delete resp
    for {set i 0} {$i<[llength $qlvar(resfields)]} {incr i} {
        .c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)] [expr 1+$qlvar(yoffs)] -text [lindex $qlvar(resfields) $i] -anchor nw -fill navy -tags {resf resp} -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
        .c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)] [expr 16+$qlvar(yoffs)] -text [lindex $qlvar(restables) $i] -anchor nw -tags {resp rest} -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
        .c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)] [expr 31+$qlvar(yoffs)] -text [lindex $qlvar(ressort) $i] -anchor nw -tags {resp sort} -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
        if {[lindex $qlvar(rescriteria) $i]!=""} {
            .c create text [expr $resoffset+4+$qlvar(xoffs)+$i*$qlvar(reswidth)]  [expr $qlvar(yoffs)+46+15*0] -anchor nw -text [lindex $qlvar(rescriteria) $i]  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*  -tags [subst {resp cr-c$i-r0}]
        }
    }
    .c raise reshdr
    .c bind sort <Button-1> {ql_swap_sort %W %x %y}
}

proc ql_draw_table {it} {
global qlvar

set posy 10
set allbox [.c bbox rect]
if {$allbox==""} {set posx 10} else {set posx [expr 20+[lindex $allbox 2]]}
set tablename $qlvar(tablename$it)
.c create text $posx $posy -text $tablename -anchor nw -tags [subst {tab$tablename f-oid mov tableheader}] -font -Adobe-Helvetica-Bold-R-Normal-*-*-120-*-*-*-*-*
incr posy 16
foreach fld $qlvar(tablestruct$it) {
   .c create text $posx $posy -text $fld -anchor nw -tags [subst {f-$fld tab$tablename mov}] -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
   incr posy 14
}
set reg [.c bbox tab$tablename]
.c create rectangle [lindex $reg 0] [lindex $reg 1] [lindex $reg 2] [lindex $reg 3] -fill #EEEEEE -tags [subst {rect tab$tablename}]
.c create line [lindex $reg 0] [expr [lindex $reg 1]+15] [lindex $reg 2] [expr [lindex $reg 1]+15] -tags [subst {rect tab$tablename}]
}

proc ql_get_tag_info {obj prefix} {
set taglist [.c gettags $obj]
set tagpos [lsearch -regexp $taglist "^$prefix"]
if {$tagpos==-1} {return ""}
set thattag [lindex $taglist $tagpos]
return [string range $thattag [string length $prefix] end]
}

proc ql_link_click {x y} {
global qlvar

set obj [.c find closest $x $y 1 links]
if {[ql_get_tag_info $obj link]!="s"} return
.c itemconfigure [.c find withtag hili] -fill black
.c dtag [.c find withtag hili] hili
.c addtag hili withtag $obj
.c itemconfigure $obj -fill blue
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
    .c move mov $dx $dy
    .c move links $dx $dy
    .c move rect $dx $dy
} else {
    .c move resp $dx 0
    .c move resgrid $dx 0
    .c raise reshdr
}
}

proc ql_read_struct {} {
global qlvar

set qlvar(ntables) 3
set qlvar(tablename0) Facturi
set qlvar(tablename1) Nommat
set qlvar(tablename2) Incasari
set qlvar(tablestruct0) [list factura client valoare tva]
set qlvar(tablestruct1) [list cod denumire pret greutate procent_tva]
set qlvar(tablestruct2) [list data valoare nrdoc referinta]
}

proc ql_show_sql {} {
global qlvar

set sqlcmd "select "
for {set i 0} {$i<[llength $qlvar(resfields)]} {incr i} {
    if {$sqlcmd!="select "} {set sqlcmd "$sqlcmd, "}
    set sqlcmd "$sqlcmd[lindex $qlvar(restables) $i].[lindex $qlvar(resfields) $i]"
}
set tables {}
for {set i 0} {$i<$qlvar(ntables)} {incr i} {
    lappend tables $qlvar(tablename$i)    
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
        if {[string range $sup1 0 4]=="where"} {set sup1 "$sup1 and "}
        set sup1 "$sup1 ([lindex $qlvar(restables) $i].[lindex $qlvar(resfields) $i]$crit) "        
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
.c delete sqlpage
.c create rectangle 0 0 2000 [expr $qlvar(yoffs)-1] -fill #ffffff -tags {sqlpage}
.c create text 10 10 -text $sqlcmd -anchor nw -width 550 -tags {sqlpage} -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
.c bind sqlpage <Button-1> {.c delete sqlpage}
}

proc ql_swap_sort {w x y} {
global qlvar
set obj [$w find closest $x $y]
set taglist [.c gettags $obj]
if {[lsearch $taglist sort]==-1} return
set cum [.c itemcget $obj -text]
if {$cum=="unsorted"} {
    set cum Ascending
} elseif {$cum=="Ascending"} {
    set cum Descending
} else {
    set cum unsorted
}
set col [expr int(($x-$qlvar(xoffs))/$qlvar(reswidth))]
set qlvar(ressort) [lreplace $qlvar(ressort) $col $col $cum]
.c itemconfigure $obj -text $cum
}

proc qlc_click {x y w} {
global qlvar
set qlvar(panstarted) 0
if {$w==".c"} {
    set canpan 1
    if {$y<$qlvar(yoffs)} {
        if {[llength [.c find overlapping $x $y $x $y]]!=0} {set canpan 0}
            set qlvar(panobject) tables
    } else {
        set qlvar(panobject) result
    }
    if {$canpan} {
        . configure -cursor hand1
        set qlvar(panstartx) $x
        set qlvar(panstarty) $y
        set qlvar(panstarted) 1
    }
}
set isedit 0
catch {set isedit $qlvar(critedit)}
# Compute the offset of the result panel due to panning
set resoffset [expr [lindex [.c bbox resmarker] 0]-$qlvar(xoffs)]
if {$isedit} {
    set qlvar(rescriteria) [lreplace $qlvar(rescriteria) $qlvar(critcol) $qlvar(critcol) $qlvar(critval)]
    .c delete cr-c$qlvar(critcol)-r$qlvar(critrow)
    .c create text [expr $resoffset+4+$qlvar(xoffs)+$qlvar(critcol)*$qlvar(reswidth)] [expr $qlvar(yoffs)+46+15*$qlvar(critrow)] -anchor nw -text $qlvar(critval) -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -tags [subst {resp cr-c$qlvar(critcol)-r$qlvar(critrow)}]
    set qlvar(critedit) 0
}
catch {destroy .entc}
if {$y<[expr $qlvar(yoffs)+46]} return
if {$x<[expr $qlvar(xoffs)+5]} return
set col [expr int(($x-$qlvar(xoffs)-$resoffset)/$qlvar(reswidth))]
if {$col>=[llength $qlvar(resfields)]} return
set nx [expr $col*$qlvar(reswidth)+8+$qlvar(xoffs)+$resoffset]
set ny [expr $qlvar(yoffs)+76]
# Get the old criteria value
set qlvar(critval) [lindex $qlvar(rescriteria) $col]
entry .entc -textvar qlvar(critval) -borderwidth 0 -background #FFFFFF -highlightthickness 0 -selectborderwidth 0  -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-*
place .entc -x $nx -y $ny -height 14
focus .entc
bind .entc <Button-1> {set qlvar(panstarted) 0}
set qlvar(critcol) $col
set qlvar(critrow) 0
set qlvar(critedit) 1
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


	set base ""
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
    canvas $base.c \
        -background #fefefe -borderwidth 2 -height 207 -relief ridge \
        -takefocus 0 -width 295 
    label $base.msg -textvar msg -borderwidth 1 -relief sunken
    button $base.b2 \
        -borderwidth 1 -command ql_draw_lizzard \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Paint demo tables} 
    button $base.showbtn \
        -borderwidth 1 -command ql_show_sql \
        -font -Adobe-Helvetica-Medium-R-Normal-*-*-120-*-*-*-*-* -padx 9 \
        -pady 3 -text {Show SQL} 
    ###################
    # SETTING GEOMETRY
    ###################
    place $base.c \
        -x 5 -y 30 -width 578 -height 425 -anchor nw -bordermode ignore 
    place $base.b2 \
        -x 5 -y 5 -height 26 -anchor nw -bordermode ignore 
    place $base.showbtn \
        -x 130 -y 5 -height 26 -anchor nw -bordermode ignore 
	place $base.msg \
		-x 5 -y 460 -width 578 -anchor nw

main $argc $argv
