namespace eval VisualQueryBuilder {

# The following array will hold all the local variables

variable vqb

proc {addNewTable} {{tabx 0} {taby 0} {alias -1}} {
global PgAcVar CurrentDB
variable vqb
if {$vqb(newtablename)==""} return
set fldlist {}
setCursor CLOCK
wpg_select $CurrentDB "select attnum,attname from pg_class,pg_attribute where (pg_class.relname='$vqb(newtablename)') and (pg_class.oid=pg_attribute.attrelid) and (attnum>0) order by attnum" rec {
		lappend fldlist $rec(attname)
}
setCursor DEFAULT
if {$fldlist==""} {
	showError [format [intlmsg "Table '%s' not found!"] $vqb(newtablename)]
	return
}
if {$alias==-1} {
	set tabnum $vqb(ntables)
} else {
	regsub t $alias "" tabnum
}
set vqb(tablename$tabnum) $vqb(newtablename)
set vqb(tablestruct$tabnum) $fldlist
set vqb(tablealias$tabnum) "t$tabnum"
set vqb(ali_t$tabnum) $vqb(newtablename)
set vqb(tablex$tabnum) $tabx
set vqb(tabley$tabnum) $taby

incr vqb(ntables)
if {$vqb(ntables)==1} {
	repaintAll
} else {
	drawTable [expr $vqb(ntables)-1]
}
set vqb(newtablename) {}
focus .pgaw:VisualQuery.fb.entt
}

proc {computeSQL} {} {
global PgAcVar
variable vqb
set sqlcmd "select "
#rjr 8Mar1999 added logical return state for results
for {set i 0} {$i<[llength $vqb(resfields)]} {incr i} {
	if {[lindex $vqb(resreturn) $i]==[intlmsg Yes]} {
		if {$sqlcmd!="select "} {set sqlcmd "$sqlcmd, "}
		set sqlcmd "$sqlcmd[lindex $vqb(restables) $i].\"[lindex $vqb(resfields) $i]\""
	}
}
set tables {}
for {set i 0} {$i<$vqb(ntables)} {incr i} {
	set thename {}
	catch {set thename $vqb(tablename$i)}
	if {$thename!=""} {lappend tables "\"$vqb(tablename$i)\" $vqb(tablealias$i)"}
}
set sqlcmd "$sqlcmd from [join $tables ,] "
set sup1 {}
if {[llength $vqb(links)]>0} {
	set sup1 "where "
	foreach link $vqb(links) {
		if {$sup1!="where "} {set sup1 "$sup1 and "}
		set sup1 "$sup1 ([lindex $link 0].\"[lindex $link 1]\"=[lindex $link 2].\"[lindex $link 3]\")"
	}
}
for {set i 0} {$i<[llength $vqb(resfields)]} {incr i} {
	set crit [lindex $vqb(rescriteria) $i]
	if {$crit!=""} {
		if {$sup1==""} {set sup1 "where "}
		if {[string length $sup1]>6} {set sup1 "$sup1 and "}
		set sup1 "$sup1 ([lindex $vqb(restables) $i].\"[lindex $vqb(resfields) $i]\" $crit) "        
	}        
}
set sqlcmd "$sqlcmd $sup1"
set sup2 {}
for {set i 0} {$i<[llength $vqb(ressort)]} {incr i} {
	set how [lindex $vqb(ressort) $i]
	if {$how!="unsorted"} {
		if {$how=="Ascending"} {set how asc} else {set how desc}
		if {$sup2==""} {set sup2 " order by "} else {set sup2 "$sup2,"}
		set sup2 "$sup2 [lindex $vqb(restables) $i].\"[lindex $vqb(resfields) $i]\" $how "
	}
}
set sqlcmd "$sqlcmd $sup2"
set vqb(qcmd) $sqlcmd
return $sqlcmd
}

proc {deleteObject} {} {
global PgAcVar
variable vqb
# Checking if there is a highlighted object (i.e. is selected)
set obj [.pgaw:VisualQuery.c find withtag hili]
if {$obj==""} return
#
# Is object a link ?
if {[getTagInfo $obj link]=="s"} {
	if {[tk_messageBox -title [intlmsg Warning] -icon question -parent .pgaw:VisualQuery -message [intlmsg "Remove link ?"] -type yesno -default no]=="no"} return
	set linkid [getTagInfo $obj lkid]
	set vqb(links) [lreplace $vqb(links) $linkid $linkid]
	.pgaw:VisualQuery.c delete links
	drawLinks
	return
}
#
# Is object a result field ?
if {[getTagInfo $obj res]=="f"} {
	set col [getTagInfo $obj col]
	if {$col==""} return
	if {[tk_messageBox -title [intlmsg Warning] -icon question -parent .pgaw:VisualQuery -message [intlmsg "Remove field from result ?"] -type yesno -default no]=="no"} return
	set vqb(resfields) [lreplace $vqb(resfields) $col $col]
	set vqb(ressort) [lreplace $vqb(ressort) $col $col]
	set vqb(resreturn) [lreplace $vqb(resreturn) $col $col]
	set vqb(restables) [lreplace $vqb(restables) $col $col]
	set vqb(rescriteria) [lreplace $vqb(rescriteria) $col $col]
	drawResultPanel
	return
}
#
# Is object a table ?
set tablealias [getTagInfo $obj tab]
set tablename $vqb(ali_$tablealias)
if {"$tablename"==""} return
if {[tk_messageBox -title [intlmsg Warning] -icon question -parent .pgaw:VisualQuery -message [format [intlmsg "Remove table %s from query?"] $tablename] -type yesno -default no]=="no"} return
for {set i [expr [llength $vqb(restables)]-1]} {$i>=0} {incr i -1} {
	if {"$tablealias"==[lindex $vqb(restables) $i]} {
	   set vqb(resfields) [lreplace $vqb(resfields) $i $i]
	   set vqb(ressort) [lreplace $vqb(ressort) $i $i]
	   set vqb(resreturn) [lreplace $vqb(resreturn) $i $i]
	   set vqb(restables) [lreplace $vqb(restables) $i $i]
	   set vqb(rescriteria) [lreplace $vqb(rescriteria) $i $i]
	}
}
for {set i [expr [llength $vqb(links)]-1]} {$i>=0} {incr i -1} {
	set thelink [lindex $vqb(links) $i]
	if {($tablealias==[lindex $thelink 0]) || ($tablealias==[lindex $thelink 2])} {
		set vqb(links) [lreplace $vqb(links) $i $i]
	}
}
for {set i 0} {$i<$vqb(ntables)} {incr i} {
	set temp {}
	catch {set temp $vqb(tablename$i)}
	if {"$temp"=="$tablename"} {
		unset vqb(tablename$i)
		unset vqb(tablestruct$i)
		unset vqb(tablealias$i)
		break
	}
}
unset vqb(ali_$tablealias)
#incr vqb(ntables) -1
.pgaw:VisualQuery.c delete tab$tablealias
.pgaw:VisualQuery.c delete links
drawLinks
drawResultPanel
}


proc {dragObject} {w x y} {
global PgAcVar
variable vqb
	if {"$PgAcVar(draginfo,obj)" == ""} {return}
	set dx [expr $x - $PgAcVar(draginfo,x)]
	set dy [expr $y - $PgAcVar(draginfo,y)]
	if {$PgAcVar(draginfo,is_a_table)} {
		$w move $PgAcVar(draginfo,tabletag) $dx $dy
		drawLinks
	} else {
		$w move $PgAcVar(draginfo,obj) $dx $dy
	}
	set PgAcVar(draginfo,x) $x
	set PgAcVar(draginfo,y) $y
}


proc {dragStart} {w x y} {
global PgAcVar
variable vqb
PgAcVar:clean draginfo,*
set PgAcVar(draginfo,obj) [$w find closest $x $y]
if {[getTagInfo $PgAcVar(draginfo,obj) r]=="ect"} {
	# If it'a a rectangle, exit
	set PgAcVar(draginfo,obj) {}
	return
}
.pgaw:VisualQuery configure -cursor hand1
.pgaw:VisualQuery.c raise $PgAcVar(draginfo,obj)
set PgAcVar(draginfo,table) 0
if {[getTagInfo $PgAcVar(draginfo,obj) table]=="header"} {
	set PgAcVar(draginfo,is_a_table) 1
	set taglist [.pgaw:VisualQuery.c gettags $PgAcVar(draginfo,obj)]
	set PgAcVar(draginfo,tabletag) [lindex $taglist [lsearch -regexp $taglist "^tab\[0-9\]*"]]
	.pgaw:VisualQuery.c raise $PgAcVar(draginfo,tabletag)
	.pgaw:VisualQuery.c itemconfigure [.pgaw:VisualQuery.c find withtag hili] -fill black
	.pgaw:VisualQuery.c dtag [.pgaw:VisualQuery.c find withtag hili] hili
	.pgaw:VisualQuery.c addtag hili withtag $PgAcVar(draginfo,obj)
	.pgaw:VisualQuery.c itemconfigure hili -fill blue
} else {
	set PgAcVar(draginfo,is_a_table) 0
}
set PgAcVar(draginfo,x) $x
set PgAcVar(draginfo,y) $y
set PgAcVar(draginfo,sx) $x
set PgAcVar(draginfo,sy) $y
}


proc {dragStop} {x y} {
global PgAcVar
variable vqb
# when click Close, ql window is destroyed but event ButtonRelease-1 is fired
if {![winfo exists .pgaw:VisualQuery]} return;
.pgaw:VisualQuery configure -cursor left_ptr
set este {}
catch {set este $PgAcVar(draginfo,obj)}
if {$este==""} return
# Re-establish the normal paint order so
# information won't be overlapped by table rectangles
# or link lines
.pgaw:VisualQuery.c lower $PgAcVar(draginfo,obj)
.pgaw:VisualQuery.c lower rect
.pgaw:VisualQuery.c lower links
set vqb(panstarted) 0
if {$PgAcVar(draginfo,is_a_table)} {
	set tabnum [getTagInfo $PgAcVar(draginfo,obj) tabt]
	foreach w [.pgaw:VisualQuery.c find withtag $PgAcVar(draginfo,tabletag)] {
		if {[lsearch [.pgaw:VisualQuery.c gettags $w] outer] != -1} {
			foreach [list vqb(tablex$tabnum) vqb(tabley$tabnum) x1 y1] [.pgaw:VisualQuery.c coords $w] {}
		}
	}
	set PgAcVar(draginfo,obj) {}
	.pgaw:VisualQuery.c delete links
	drawLinks
	return
}
.pgaw:VisualQuery.c move $PgAcVar(draginfo,obj) [expr $PgAcVar(draginfo,sx)-$x] [expr $PgAcVar(draginfo,sy)-$y]
if {($y>$vqb(yoffs)) && ($x>$vqb(xoffs))} {
	# Drop position : inside the result panel
	# Compute the offset of the result panel due to panning
	set resoffset [expr [lindex [.pgaw:VisualQuery.c bbox resmarker] 0]-$vqb(xoffs)]
	set newfld [.pgaw:VisualQuery.c itemcget $PgAcVar(draginfo,obj) -text]
	set tabtag [getTagInfo $PgAcVar(draginfo,obj) tab]
	set col [expr int(($x-$vqb(xoffs)-$resoffset)/$vqb(reswidth))]
	set vqb(resfields) [linsert $vqb(resfields) $col $newfld]
	set vqb(ressort) [linsert $vqb(ressort) $col unsorted]
	set vqb(rescriteria) [linsert $vqb(rescriteria) $col {}]
	set vqb(restables) [linsert $vqb(restables) $col $tabtag]
	set vqb(resreturn) [linsert $vqb(resreturn) $col [intlmsg Yes]]
	drawResultPanel    
} else {
	# Drop position : in the table panel
	set droptarget [.pgaw:VisualQuery.c find overlapping $x $y $x $y]
	set targettable {}
	foreach item $droptarget {
		set targettable [getTagInfo $item tab]
		set targetfield [getTagInfo $item f-]
		if {($targettable!="") && ($targetfield!="")} {
			set droptarget $item
			break
		}
	}
	# check if target object isn't a rectangle
	if {[getTagInfo $droptarget rec]=="t"} {set targettable {}}
	if {$targettable!=""} {
		# Target has a table
		# See about originate table
		set sourcetable [getTagInfo $PgAcVar(draginfo,obj) tab]
		if {$sourcetable!=""} {
			# Source has also a tab .. tag
			set sourcefield [getTagInfo $PgAcVar(draginfo,obj) f-]
			if {$sourcetable!=$targettable} {
				lappend vqb(links) [list $sourcetable $sourcefield $targettable $targetfield]
				drawLinks
			}
		}
	}
}
# Erase information about onbject beeing dragged
set PgAcVar(draginfo,obj) {}
}


proc {getTableList} {} {
global PgAcVar
variable vqb
	set tablelist {}
	foreach name [array names vqb tablename*] {
		regsub tablename $name "" num
		lappend tablelist $vqb($name) $vqb(tablex$num) $vqb(tabley$num) t$num
	}
	return $tablelist
}


proc {getLinkList} {} {
global PgAcVar
variable vqb
	set linklist {}
	foreach l $vqb(links) {
		lappend linklist [lindex $l 0] [lindex $l 1] [lindex $l 2] [lindex $l 3] 
		}
	return $linklist
}


proc {loadVisualLayout} {} {
global PgAcVar
variable vqb
	init
	foreach {t x y a} $PgAcVar(query,tables) {set vqb(newtablename) $t; addNewTable $x $y $a}
	foreach {t0 f0 t1 f1} $PgAcVar(query,links) {lappend vqb(links) [list $t0 $f0 $t1 $f1]}
	foreach {f t s c r} $PgAcVar(query,results) {addResultColumn $f $t $s $c $r}
	repaintAll
}


proc {findField} {alias field} {
	foreach obj [.pgaw:VisualQuery.c find withtag f-${field}] {
		if {[lsearch [.pgaw:VisualQuery.c gettags $obj] tab$alias] != -1} {return $obj}
	}
	return -1
}


proc {getResultList} {} {
global PgAcVar
variable vqb
	set reslist {}
	for {set i 0} {$i < [llength $vqb(resfields)]} {incr i} {
		lappend reslist [lindex $vqb(resfields) $i]
		lappend reslist [lindex $vqb(restables) $i]
		lappend reslist [lindex $vqb(ressort) $i]
		lappend reslist [lindex $vqb(rescriteria) $i]
		lappend reslist [lindex $vqb(resreturn) $i]
	}
	return $reslist
}


proc {addResultColumn} {f t s c r} {
global PgAcVar
variable vqb
	lappend vqb(resfields) $f
	lappend vqb(restables) $t
	lappend vqb(ressort) $s
	lappend vqb(rescriteria) $c
	lappend vqb(resreturn) $r
}


proc {drawLinks} {} {
global PgAcVar
variable vqb
.pgaw:VisualQuery.c delete links
set i 0
foreach link $vqb(links) {
	# Compute the source and destination right edge
	set sre [lindex [.pgaw:VisualQuery.c bbox tab[lindex $link 0]] 2]
	set dre [lindex [.pgaw:VisualQuery.c bbox tab[lindex $link 2]] 2]
	# Compute field bound boxes
	set sbbox [.pgaw:VisualQuery.c bbox [findField [lindex $link 0] [lindex $link 1]]]
	set dbbox [.pgaw:VisualQuery.c bbox [findField [lindex $link 2] [lindex $link 3]]]
	# Compute the auxiliary lines
	if {[lindex $sbbox 2] < [lindex $dbbox 0]} {
		# Source object is on the left of target object
		set x1 $sre
		set y1 [expr ([lindex $sbbox 1]+[lindex $sbbox 3])/2]
		.pgaw:VisualQuery.c create line $x1 $y1 [expr $x1+10] $y1 -tags [subst {links lkid$i}] -width 3
		set x2 [lindex $dbbox 0]
		set y2 [expr ([lindex $dbbox 1]+[lindex $dbbox 3])/2]
		.pgaw:VisualQuery.c create line [expr $x2-10] $y2 $x2 $y2 -tags [subst {links lkid$i}] -width 3
		.pgaw:VisualQuery.c create line [expr $x1+10] $y1 [expr $x2-10] $y2 -tags [subst {links lkid$i}] -width 2
	} else {
		# source object is on the right of target object
		set x1 [lindex $sbbox 0]
		set y1 [expr ([lindex $sbbox 1]+[lindex $sbbox 3])/2]
		.pgaw:VisualQuery.c create line $x1 $y1 [expr $x1-10] $y1 -tags [subst {links lkid$i}] -width 3
		set x2 $dre
		set y2 [expr ([lindex $dbbox 1]+[lindex $dbbox 3])/2]
		.pgaw:VisualQuery.c create line $x2 $y2 [expr $x2+10] $y2 -width 3 -tags [subst {links lkid$i}]
		.pgaw:VisualQuery.c create line [expr $x1-10] $y1 [expr $x2+10] $y2 -tags [subst {links lkid$i}] -width 2
	}
	incr i
}
.pgaw:VisualQuery.c lower links
.pgaw:VisualQuery.c bind links <Button-1> {VisualQueryBuilder::linkClick %x %y}
}


proc {repaintAll} {} {
global PgAcVar
variable vqb
.pgaw:VisualQuery.c delete all
set posx 20
foreach tn [array names vqb tablename*] {
	regsub tablename $tn "" it
	drawTable $it
}
.pgaw:VisualQuery.c lower rect
.pgaw:VisualQuery.c create line 0 $vqb(yoffs) 10000 $vqb(yoffs) -width 3
.pgaw:VisualQuery.c create rectangle 0 $vqb(yoffs) 10000 5000 -fill #FFFFFF
for {set i [expr 15+$vqb(yoffs)]} {$i<500} {incr i 15} {
	.pgaw:VisualQuery.c create line $vqb(xoffs) $i 10000 $i -fill #CCCCCC -tags {resgrid}
}    
for {set i $vqb(xoffs)} {$i<10000} {incr i $vqb(reswidth)} {
	.pgaw:VisualQuery.c create line $i [expr 1+$vqb(yoffs)] $i 10000 -fill #cccccc -tags {resgrid}
}
# Make a marker for result panel offset calculations (due to panning)
.pgaw:VisualQuery.c create line $vqb(xoffs) $vqb(yoffs) $vqb(xoffs) 500 -tags {resmarker resgrid}
.pgaw:VisualQuery.c create rectangle 0 $vqb(yoffs) $vqb(xoffs) 5000 -fill #EEEEEE -tags {reshdr}
.pgaw:VisualQuery.c create text 5 [expr 1+$vqb(yoffs)]  -text [intlmsg Field] -anchor nw -font $PgAcVar(pref,font_normal) -tags {reshdr}
.pgaw:VisualQuery.c create text 5 [expr 16+$vqb(yoffs)] -text [intlmsg Table] -anchor nw -font $PgAcVar(pref,font_normal) -tags {reshdr}
.pgaw:VisualQuery.c create text 5 [expr 31+$vqb(yoffs)] -text [intlmsg Sort] -anchor nw -font $PgAcVar(pref,font_normal) -tags {reshdr}
.pgaw:VisualQuery.c create text 5 [expr 46+$vqb(yoffs)] -text [intlmsg Criteria] -anchor nw -font $PgAcVar(pref,font_normal) -tags {reshdr}
.pgaw:VisualQuery.c create text 5 [expr 61+$vqb(yoffs)] -text [intlmsg Return] -anchor nw -font $PgAcVar(pref,font_normal) -tags {reshdr}

drawLinks
drawResultPanel

.pgaw:VisualQuery.c bind mov <Button-1> {VisualQueryBuilder::dragStart %W %x %y}
.pgaw:VisualQuery.c bind mov <B1-Motion> {VisualQueryBuilder::dragObject %W %x %y}
bind .pgaw:VisualQuery <ButtonRelease-1> {VisualQueryBuilder::dragStop %x %y}
bind .pgaw:VisualQuery <Button-1> {VisualQueryBuilder::canvasClick %x %y %W}
bind .pgaw:VisualQuery <B1-Motion> {VisualQueryBuilder::panning %x %y}
bind .pgaw:VisualQuery <Key-Delete> {VisualQueryBuilder::deleteObject}
}


proc {drawResultPanel} {} {
global PgAcVar
variable vqb
# Compute the offset of the result panel due to panning
set resoffset [expr [lindex [.pgaw:VisualQuery.c bbox resmarker] 0]-$vqb(xoffs)]
.pgaw:VisualQuery.c delete resp
for {set i 0} {$i<[llength $vqb(resfields)]} {incr i} {
	.pgaw:VisualQuery.c create text [expr $resoffset+4+$vqb(xoffs)+$i*$vqb(reswidth)] [expr 1+$vqb(yoffs)] -text [lindex $vqb(resfields) $i] -anchor nw -tags [subst {resf resp col$i}] -font $PgAcVar(pref,font_normal)
	.pgaw:VisualQuery.c create text [expr $resoffset+4+$vqb(xoffs)+$i*$vqb(reswidth)] [expr 16+$vqb(yoffs)] -text $vqb(ali_[lindex $vqb(restables) $i]) -anchor nw -tags {resp rest} -font $PgAcVar(pref,font_normal)
	.pgaw:VisualQuery.c create text [expr $resoffset+4+$vqb(xoffs)+$i*$vqb(reswidth)] [expr 31+$vqb(yoffs)] -text [lindex $vqb(ressort) $i] -anchor nw -tags {resp sort} -font $PgAcVar(pref,font_normal)
	if {[lindex $vqb(rescriteria) $i]!=""} {
		.pgaw:VisualQuery.c create text [expr $resoffset+4+$vqb(xoffs)+$i*$vqb(reswidth)]  [expr $vqb(yoffs)+46+15*0] -anchor nw -text [lindex $vqb(rescriteria) $i]  -font $PgAcVar(pref,font_normal) -tags [subst {resp cr-c$i-r0}]
	}
	.pgaw:VisualQuery.c create text [expr $resoffset+4+$vqb(xoffs)+$i*$vqb(reswidth)] [expr 61+$vqb(yoffs)] -text [lindex $vqb(resreturn) $i] -anchor nw -tags {resp retval} -font $PgAcVar(pref,font_normal)
}
.pgaw:VisualQuery.c raise reshdr
.pgaw:VisualQuery.c bind resf <Button-1> {VisualQueryBuilder::resultFieldClick %x %y}
.pgaw:VisualQuery.c bind sort <Button-1> {VisualQueryBuilder::toggleSortMode %W %x %y}
.pgaw:VisualQuery.c bind retval <Button-1> {VisualQueryBuilder::toggleReturn %W %x %y}
}


proc {drawTable} {it} {
global PgAcVar
variable vqb
if {$vqb(tablex$it)==0} {
	set posy 10
	set allbox [.pgaw:VisualQuery.c bbox rect]
	if {$allbox==""} {set posx 10} else {set posx [expr 20+[lindex $allbox 2]]}
	set vqb(tablex$it) $posx
	set vqb(tabley$it) $posy
} else {
	set posx [expr int($vqb(tablex$it))]
	set posy [expr int($vqb(tabley$it))]
}
set tablename $vqb(tablename$it)
set tablealias $vqb(tablealias$it)
.pgaw:VisualQuery.c create text $posx $posy -text "$tablename" -anchor nw -tags [subst {tab$tablealias f-oid mov tableheader}] -font $PgAcVar(pref,font_bold)
incr posy 16
foreach fld $vqb(tablestruct$it) {
   .pgaw:VisualQuery.c create text $posx $posy -text $fld -fill #010101 -anchor nw -tags [subst {f-$fld tab$tablealias mov}] -font $PgAcVar(pref,font_normal)
   incr posy 14
}
set reg [.pgaw:VisualQuery.c bbox tab$tablealias]
.pgaw:VisualQuery.c create rectangle [lindex $reg 0] [lindex $reg 1] [lindex $reg 2] [lindex $reg 3] -fill #EEEEEE -tags [subst {rect outer tab$tablealias}]
.pgaw:VisualQuery.c create line [lindex $reg 0] [expr [lindex $reg 1]+15] [lindex $reg 2] [expr [lindex $reg 1]+15] -tags [subst {rect tab$tablealias}]
.pgaw:VisualQuery.c lower tab$tablealias
.pgaw:VisualQuery.c lower rect
}


proc {getTagInfo} {obj prefix} {
variable vqb
	set taglist [.pgaw:VisualQuery.c gettags $obj]
	set tagpos [lsearch -regexp $taglist "^$prefix"]
	if {$tagpos==-1} {return ""}
	set thattag [lindex $taglist $tagpos]
	return [string range $thattag [string length $prefix] end]
}

proc {init} {} {
global PgAcVar
variable vqb
	catch { unset vqb }
	set vqb(yoffs) 360
	set vqb(xoffs) 50
	set vqb(reswidth) 150
	set vqb(resfields) {}
	set vqb(resreturn) {}
	set vqb(ressort) {}
	set vqb(rescriteria) {}
	set vqb(restables) {}
	set vqb(critedit) 0
	set vqb(links) {}
	set vqb(ntables) 0
	set vqb(newtablename) {}
}


proc {linkClick} {x y} {
global PgAcVar
variable vqb
	set obj [.pgaw:VisualQuery.c find closest $x $y 1 links]
	if {[getTagInfo $obj link]!="s"} return
	.pgaw:VisualQuery.c itemconfigure [.pgaw:VisualQuery.c find withtag hili] -fill black
	.pgaw:VisualQuery.c dtag [.pgaw:VisualQuery.c find withtag hili] hili
	.pgaw:VisualQuery.c addtag hili withtag $obj
	.pgaw:VisualQuery.c itemconfigure $obj -fill blue
}


proc {panning} {x y} {
global PgAcVar
variable vqb
	set panstarted 0
	catch {set panstarted $vqb(panstarted) }
	if {!$panstarted} return
	set dx [expr $x-$vqb(panstartx)]
	set dy [expr $y-$vqb(panstarty)]
	set vqb(panstartx) $x
	set vqb(panstarty) $y
	if {$vqb(panobject)=="tables"} {
		.pgaw:VisualQuery.c move mov $dx $dy
		.pgaw:VisualQuery.c move links $dx $dy
		.pgaw:VisualQuery.c move rect $dx $dy
	} else {
		.pgaw:VisualQuery.c move resp $dx 0
		.pgaw:VisualQuery.c move resgrid $dx 0
		.pgaw:VisualQuery.c raise reshdr
	}
}


proc {resultFieldClick} {x y} {
global PgAcVar
variable vqb
	set obj [.pgaw:VisualQuery.c find closest $x $y]
	if {[getTagInfo $obj res]!="f"} return
	.pgaw:VisualQuery.c itemconfigure [.pgaw:VisualQuery.c find withtag hili] -fill black
	.pgaw:VisualQuery.c dtag [.pgaw:VisualQuery.c find withtag hili] hili
	.pgaw:VisualQuery.c addtag hili withtag $obj
	.pgaw:VisualQuery.c itemconfigure $obj -fill blue
}


proc {showSQL} {} {
global PgAcVar
variable vqb
	set sqlcmd [computeSQL]
	.pgaw:VisualQuery.c delete sqlpage
	.pgaw:VisualQuery.c create rectangle 0 0 2000 [expr $vqb(yoffs)-1] -fill #ffffff -tags {sqlpage}
	.pgaw:VisualQuery.c create text 10 10 -text $sqlcmd -anchor nw -width 550 -tags {sqlpage} -font $PgAcVar(pref,font_normal)
	.pgaw:VisualQuery.c bind sqlpage <Button-1> {.pgaw:VisualQuery.c delete sqlpage}
}


proc {toggleSortMode} {w x y} {
global PgAcVar
variable vqb
	set obj [$w find closest $x $y]
	set taglist [.pgaw:VisualQuery.c gettags $obj]
	if {[lsearch $taglist sort]==-1} return
	set how [.pgaw:VisualQuery.c itemcget $obj -text]
	if {$how=="unsorted"} {
		set how Ascending
	} elseif {$how=="Ascending"} {
		set how Descending
	} else {
		set how unsorted
	}
	set col [expr int(($x-$vqb(xoffs))/$vqb(reswidth))]
	set vqb(ressort) [lreplace $vqb(ressort) $col $col $how]
	.pgaw:VisualQuery.c itemconfigure $obj -text $how
}


#rjr 8Mar1999 toggle logical return state for result
proc {toggleReturn} {w x y} {
global PgAcVar
variable vqb
	set obj [$w find closest $x $y]
	set taglist [.pgaw:VisualQuery.c gettags $obj]
	if {[lsearch $taglist retval]==-1} return
	set how [.pgaw:VisualQuery.c itemcget $obj -text]
	if {$how==[intlmsg Yes]} {
		set how [intlmsg No]
	} else {
		set how [intlmsg Yes]
	} 
	set col [expr int(($x-$vqb(xoffs))/$vqb(reswidth))]
	set vqb(resreturn) [lreplace $vqb(resreturn) $col $col $how]
	.pgaw:VisualQuery.c itemconfigure $obj -text $how
}


proc {canvasClick} {x y w} {
global PgAcVar
variable vqb
set vqb(panstarted) 0
if {$w==".pgaw:VisualQuery.c"} {
	set canpan 1
	if {$y<$vqb(yoffs)} {
		if {[llength [.pgaw:VisualQuery.c find overlapping $x $y $x $y]]!=0} {set canpan 0}
			set vqb(panobject) tables
	} else {
		set vqb(panobject) result
	}
	if {$canpan} {
		.pgaw:VisualQuery configure -cursor hand1
		set vqb(panstartx) $x
		set vqb(panstarty) $y
		set vqb(panstarted) 1
	}
}
set isedit 0
catch {set isedit $vqb(critedit)}
# Compute the offset of the result panel due to panning
set resoffset [expr [lindex [.pgaw:VisualQuery.c bbox resmarker] 0]-$vqb(xoffs)]
if {$isedit} {
	set vqb(rescriteria) [lreplace $vqb(rescriteria) $vqb(critcol) $vqb(critcol) $vqb(critval)]
	.pgaw:VisualQuery.c delete cr-c$vqb(critcol)-r$vqb(critrow)
	.pgaw:VisualQuery.c create text [expr $resoffset+4+$vqb(xoffs)+$vqb(critcol)*$vqb(reswidth)] [expr $vqb(yoffs)+46+15*$vqb(critrow)] -anchor nw -text $vqb(critval) -font $PgAcVar(pref,font_normal) -tags [subst {resp cr-c$vqb(critcol)-r$vqb(critrow)}]
	set vqb(critedit) 0
}
catch {destroy .pgaw:VisualQuery.entc}
if {$y<[expr $vqb(yoffs)+46]} return
if {$x<[expr $vqb(xoffs)+5]} return
set col [expr int(($x-$vqb(xoffs)-$resoffset)/$vqb(reswidth))]
if {$col>=[llength $vqb(resfields)]} return
set nx [expr $col*$vqb(reswidth)+8+$vqb(xoffs)+$resoffset]
set ny [expr $vqb(yoffs)+76]
# Get the old criteria value
set vqb(critval) [lindex $vqb(rescriteria) $col]
entry .pgaw:VisualQuery.entc -textvar VisualQueryBuilder::vqb(critval) -borderwidth 0 -background #FFFFFF -highlightthickness 0 -selectborderwidth 0  -font $PgAcVar(pref,font_normal)
place .pgaw:VisualQuery.entc -x $nx -y $ny -height 14
focus .pgaw:VisualQuery.entc
bind .pgaw:VisualQuery.entc <Button-1> {set VisualQueryBuilder::vqb(panstarted) 0}
set vqb(critcol) $col
set vqb(critrow) 0
set vqb(critedit) 1
}


proc {saveToQueryBuilder} {} {
global PgAcVar
variable vqb
	Window show .pgaw:QueryBuilder
	.pgaw:QueryBuilder.text1 delete 1.0 end
	set vqb(qcmd) [computeSQL]
	set PgAcVar(query,tables) [getTableList]
	set PgAcVar(query,links) [getLinkList]
	set PgAcVar(query,results) [getResultList]
	.pgaw:QueryBuilder.text1 insert end $vqb(qcmd)
	focus .pgaw:QueryBuilder
}


proc {executeSQL} {} {
global PgAcVar
variable vqb
	set vqb(qcmd) [computeSQL]
	set wn [Tables::getNewWindowName]
	set PgAcVar(mw,$wn,query) [subst $vqb(qcmd)]
	set PgAcVar(mw,$wn,updatable) 0
	set PgAcVar(mw,$wn,isaquery) 1
	Tables::createWindow
	Tables::loadLayout $wn nolayoutneeded
	Tables::selectRecords $wn $PgAcVar(mw,$wn,query)
}


proc {createDropDown} {} {
global PgAcVar
variable vqb
	if {[winfo exists .pgaw:VisualQuery.ddf]} {
		destroy .pgaw:VisualQuery.ddf
	} else {
		create_drop_down .pgaw:VisualQuery 70 27 200
		focus .pgaw:VisualQuery.ddf.sb
		foreach tbl [Database::getTablesList] {.pgaw:VisualQuery.ddf.lb insert end $tbl}
		bind .pgaw:VisualQuery.ddf.lb <ButtonRelease-1> {
			set i [.pgaw:VisualQuery.ddf.lb curselection]
			if {$i!=""} {
				set VisualQueryBuilder::vqb(newtablename) [.pgaw:VisualQuery.ddf.lb get $i]
				VisualQueryBuilder::addNewTable
			}
			destroy .pgaw:VisualQuery.ddf
			break
		}
	}
}

}

proc vTclWindow.pgaw:VisualQuery {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:VisualQuery
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 759x530+10+13
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm deiconify $base
	wm title $base [intlmsg "Visual query designer"]
	bind $base <B1-Motion> {
		VisualQueryBuilder::panning %x %y
	}
	bind $base <Button-1> {
		VisualQueryBuilder::canvasClick %x %y %W
	}
	bind $base <ButtonRelease-1> {
		VisualQueryBuilder::dragStop %x %y
	}
	bind $base <Key-Delete> {
		VisualQueryBuilder::deleteObject
	}
	bind $base <Key-F1> "Help::load visual_designer"
	canvas $base.c  -background #fefefe -borderwidth 2 -height 207 -relief ridge  -takefocus 0 -width 295 
	frame $base.fb -height 75 -width 125 
	label $base.fb.l12  -borderwidth 0 -text "[intlmsg {Add table}] "
	entry $base.fb.entt  -background #fefefe -borderwidth 1 -highlightthickness 1 \
		-selectborderwidth 0 -textvariable VisualQueryBuilder::vqb(newtablename) 
	bind $base.fb.entt <Key-Return> {
		VisualQueryBuilder::addNewTable
	}
	button $base.fb.bdd  -borderwidth 1 \
		-command VisualQueryBuilder::createDropDown -image dnarw 
	button $base.fb.showbtn \
		-command VisualQueryBuilder::showSQL \
		-text [intlmsg {Show SQL}]
	button $base.fb.execbtn \
		-command VisualQueryBuilder::executeSQL \
		-text [intlmsg {Execute SQL}]
	button $base.fb.stoqb \
		-command VisualQueryBuilder::saveToQueryBuilder \
		-text [intlmsg {Save to query builder}]
	button $base.fb.exitbtn \
		-command {Window destroy .pgaw:VisualQuery} \
		-text [intlmsg Close]
	place $base.c  -x 5 -y 30 -width 750 -height 500 -anchor nw -bordermode ignore 
	place $base.fb \
		-x 5 -y 0 -width 753 -height 31 -anchor nw -bordermode ignore 
	pack $base.fb.l12 \
		-in .pgaw:VisualQuery.fb -anchor center -expand 0 -fill none -side left 
	pack $base.fb.entt \
		-in .pgaw:VisualQuery.fb -anchor center -expand 0 -fill none -side left 
	pack $base.fb.bdd \
		-in .pgaw:VisualQuery.fb -anchor center -expand 0 -fill none -side left 
	pack $base.fb.exitbtn \
		-in .pgaw:VisualQuery.fb -anchor center -expand 0 -fill none -side right 
	pack $base.fb.stoqb \
		-in .pgaw:VisualQuery.fb -anchor center -expand 0 -fill none -side right 
	pack $base.fb.execbtn \
		-in .pgaw:VisualQuery.fb -anchor center -expand 0 -fill none -side right 
	pack $base.fb.showbtn \
		-in .pgaw:VisualQuery.fb -anchor center -expand 0 -fill none -side right 
}

