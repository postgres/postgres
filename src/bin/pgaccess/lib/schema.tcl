namespace eval Schema {


proc {new} {} {
global PgAcVar
	init
	Window show .pgaw:Schema
	set PgAcVar(schema,oid) 0
	set PgAcVar(schema,name) {}
	set PgAcVar(schema,tables) {}
	set PgAcVar(schema,links) {}
	set PgAcVar(schema,results) {}
	focus .pgaw:Schema.f.e
}


proc {open} {obj} {
global PgAcVar CurrentDB
	init
	set PgAcVar(schema,name) $obj
	if {[set pgres [wpg_exec $CurrentDB "select schematables,schemalinks,oid from pga_schema where schemaname='$PgAcVar(schema,name)'"]]==0} then {
		showError [intlmsg "Error retrieving schema definition"]
		return
	}
	if {[pg_result $pgres -numTuples]==0} {
		showError [format [intlmsg "Schema '%s' was not found!"] $PgAcVar(schema,name)]
		pg_result $pgres -clear
		return
	}
	set tuple [pg_result $pgres -getTuple 0]
	set tables [lindex $tuple 0]
	set links [lindex $tuple 1]
	set PgAcVar(schema,oid) [lindex $tuple 2]
	pg_result $pgres -clear
	Window show .pgaw:Schema
	foreach {t x y} $tables { 
		set PgAcVar(schema,newtablename) $t
		addNewTable $x $y
	}
	set PgAcVar(schema,links) $links
	drawLinks
	foreach {ulx uly lrx lry} [.pgaw:Schema.c bbox all] {
		wm geometry .pgaw:Schema [expr $lrx+30]x[expr $lry+30]
	}
}


proc {addNewTable} {{tabx 0} {taby 0}} {
global PgAcVar CurrentDB

if {$PgAcVar(schema,newtablename)==""} return
if {$PgAcVar(schema,newtablename)=="*"} {
	set tbllist [Database::getTablesList]
	foreach tn [array names PgAcVar schema,tablename*] {
		if { [set linkid [lsearch $tbllist $PgAcVar($tn)]] != -1 } {
			set tbllist [lreplace $tbllist $linkid $linkid]
		}
	}
	foreach t $tbllist {
		set PgAcVar(schema,newtablename) $t
		addNewTable
	}
	return
}

foreach tn [array names PgAcVar schema,tablename*] {
	if {$PgAcVar(schema,newtablename)==$PgAcVar($tn)} {
		showError [format [intlmsg "Table '%s' already in schema"] $PgAcVar($tn)]
		return
	}
}
set fldlist {}
setCursor CLOCK
wpg_select $CurrentDB "select attnum,attname,typname from pg_class,pg_attribute,pg_type where (pg_class.relname='$PgAcVar(schema,newtablename)') and (pg_class.oid=pg_attribute.attrelid) and (attnum>0) and (atttypid=pg_type.oid) order by attnum" rec {
		lappend fldlist $rec(attname) $rec(typname)
}
setCursor DEFAULT
if {$fldlist==""} {
	showError [format [intlmsg "Table '%s' not found!"] $PgAcVar(schema,newtablename)]
	return
}
set PgAcVar(schema,tablename$PgAcVar(schema,ntables)) $PgAcVar(schema,newtablename)
set PgAcVar(schema,tablestruct$PgAcVar(schema,ntables)) $fldlist
set PgAcVar(schema,tablex$PgAcVar(schema,ntables)) $tabx
set PgAcVar(schema,tabley$PgAcVar(schema,ntables)) $taby
incr PgAcVar(schema,ntables)
if {$PgAcVar(schema,ntables)==1} {
   drawAll
} else {
   drawTable [expr $PgAcVar(schema,ntables)-1]
}
lappend PgAcVar(schema,tables) $PgAcVar(schema,newtablename)  $PgAcVar(schema,tablex[expr $PgAcVar(schema,ntables)-1]) $PgAcVar(schema,tabley[expr $PgAcVar(schema,ntables)-1])
set PgAcVar(schema,newtablename) {}
focus .pgaw:Schema.f.e
}

proc {drawAll} {} {
global PgAcVar
	.pgaw:Schema.c delete all
	for {set it 0} {$it<$PgAcVar(schema,ntables)} {incr it} {
		drawTable $it
	}
	.pgaw:Schema.c lower rect
	drawLinks

	.pgaw:Schema.c bind mov <Button-1> {Schema::dragStart %W %x %y %s}
	.pgaw:Schema.c bind mov <B1-Motion> {Schema::dragMove %W %x %y}
	bind .pgaw:Schema.c <ButtonRelease-1> {Schema::dragStop %x %y}
	bind .pgaw:Schema <Button-1> {Schema::canvasClick %x %y %W}
	bind .pgaw:Schema <B1-Motion> {Schema::canvasPanning %x %y}
	bind .pgaw:Schema <Key-Delete> {Schema::deleteObject}
}


proc {drawTable} {it} {
global PgAcVar

if {$PgAcVar(schema,tablex$it)==0} {
	set posy $PgAcVar(schema,nexty)
	set posx $PgAcVar(schema,nextx)
	set PgAcVar(schema,tablex$it) $posx
	set PgAcVar(schema,tabley$it) $posy
} else {
	set posx [expr int($PgAcVar(schema,tablex$it))]
	set posy [expr int($PgAcVar(schema,tabley$it))]
}
set tablename $PgAcVar(schema,tablename$it)
.pgaw:Schema.c create text $posx $posy -text "$tablename" -anchor nw -tags [subst {tab$it f-oid mov tableheader}] -font $PgAcVar(pref,font_bold)
incr posy 16
foreach {fld ftype} $PgAcVar(schema,tablestruct$it) {
   if {[set cindex [lsearch $PgAcVar(pref,typelist) $ftype]] == -1} {set cindex 1}
   .pgaw:Schema.c create text $posx $posy -text $fld -fill [lindex $PgAcVar(pref,typecolors) $cindex] -anchor nw -tags [subst {f-$fld tab$it mov}] -font $PgAcVar(pref,font_normal)
   incr posy 14
}
set reg [.pgaw:Schema.c bbox tab$it]
.pgaw:Schema.c create rectangle [lindex $reg 0] [lindex $reg 1] [lindex $reg 2] [lindex $reg 3] -fill #EEEEEE -tags [subst {rect outer tab$it}]
.pgaw:Schema.c create line [lindex $reg 0] [expr [lindex $reg 1]+15] [lindex $reg 2] [expr [lindex $reg 1]+15] -tags [subst {rect tab$it}]
.pgaw:Schema.c lower tab$it
.pgaw:Schema.c lower rect
set reg [.pgaw:Schema.c bbox tab$it]


set nexty [lindex $reg 1]
set nextx [expr 20+[lindex $reg 2]]
if {$nextx > [winfo width .pgaw:Schema.c] } {
	set nextx 10
	set allbox [.pgaw:Schema.c bbox rect]
	set nexty [expr 20 + [lindex $allbox 3]]
}
set PgAcVar(schema,nextx) $nextx
set PgAcVar(schema,nexty) $nexty

}

proc {deleteObject} {} {
global PgAcVar
# Checking if there 
set objs [.pgaw:Schema.c find withtag hili]
set numobj [llength $objs]
if {$numobj == 0 } return
# Is object a link ?
foreach obj $objs {
	if {[getTagInfo $obj link]=="s"} {
		if {[tk_messageBox -title [intlmsg Warning] -icon question -parent .pgaw:Schema -message [intlmsg "Remove link ?"] -type yesno -default no]=="no"} return
		set linkid [getTagInfo $obj lkid]
		set PgAcVar(schema,links) [lreplace $PgAcVar(schema,links) $linkid $linkid]
		.pgaw:Schema.c delete links
		drawLinks
		return
	}
	# Is object a table ?
	set tablealias [getTagInfo $obj tab]
	set tablename $PgAcVar(schema,tablename$tablealias)
	if {"$tablename"==""} return
	if {[tk_messageBox -title [intlmsg Warning] -icon question -parent .pgaw:Schema -message [format [intlmsg "Remove table %s from schema?"] $tablename] -type yesno -default no]=="no"} return
	for {set i [expr [llength $PgAcVar(schema,links)]-1]} {$i>=0} {incr i -1} {
		set thelink [lindex $PgAcVar(schema,links) $i]
		if {($tablename==[lindex $thelink 0]) || ($tablename==[lindex $thelink 2])} {
			set PgAcVar(schema,links) [lreplace $PgAcVar(schema,links) $i $i]
		}
	}
	for {set i 0} {$i<$PgAcVar(schema,ntables)} {incr i} {
		set temp {}
		catch {set temp $PgAcVar(schema,tablename$i)}
		if {"$temp"=="$tablename"} {
			unset PgAcVar(schema,tablename$i)
			unset PgAcVar(schema,tablestruct$i)
			break
		}
	}
	#incr PgAcVar(schema,ntables) -1
	.pgaw:Schema.c delete tab$tablealias
	.pgaw:Schema.c delete links
	drawLinks
	}
}


proc {dragMove} {w x y} {
global PgAcVar
	if {"$PgAcVar(draginfo,obj)" == ""} {return}
	set dx [expr $x - $PgAcVar(draginfo,x)]
	set dy [expr $y - $PgAcVar(draginfo,y)]
	if {$PgAcVar(draginfo,is_a_table)} {
		$w move dragme $dx $dy
		drawLinks
	} else {
		$w move $PgAcVar(draginfo,obj) $dx $dy
	}
	set PgAcVar(draginfo,x) $x
	set PgAcVar(draginfo,y) $y
}


proc {dragStart} {w x y state} {
global PgAcVar
PgAcVar:clean draginfo,*
set PgAcVar(draginfo,obj) [$w find closest $x $y]
if {[getTagInfo $PgAcVar(draginfo,obj) r]=="ect"} {
	# If it'a a rectangle, exit
	set PgAcVar(draginfo,obj) {}
	return
}
.pgaw:Schema configure -cursor hand1
.pgaw:Schema.c raise $PgAcVar(draginfo,obj)
set PgAcVar(draginfo,table) 0
if {[getTagInfo $PgAcVar(draginfo,obj) table]=="header"} {
	set PgAcVar(draginfo,is_a_table) 1
	set taglist [.pgaw:Schema.c gettags $PgAcVar(draginfo,obj)]
	set PgAcVar(draginfo,tabletag) [lindex $taglist [lsearch -regexp $taglist "^tab\[0-9\]*"]]
	.pgaw:Schema.c raise $PgAcVar(draginfo,tabletag)
	if {$state == 0} {
		.pgaw:Schema.c itemconfigure hili -fill black
		.pgaw:Schema.c dtag hili
		.pgaw:Schema.c dtag dragme
	}
	.pgaw:Schema.c addtag dragme withtag $PgAcVar(draginfo,tabletag)
	.pgaw:Schema.c addtag hili withtag $PgAcVar(draginfo,obj)
	.pgaw:Schema.c itemconfigure hili -fill blue
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
# when click Close, schema window is destroyed but event ButtonRelease-1 is fired
if {![winfo exists .pgaw:Schema]} return;
.pgaw:Schema configure -cursor left_ptr
set este {}
catch {set este $PgAcVar(draginfo,obj)}
if {$este==""} return
# Re-establish the normal paint order so
# information won't be overlapped by table rectangles
# or link lines
if {$PgAcVar(draginfo,is_a_table)} {
	.pgaw:Schema.c lower $PgAcVar(draginfo,tabletag)
} else {
	.pgaw:Schema.c lower $PgAcVar(draginfo,obj)
}
.pgaw:Schema.c lower rect
.pgaw:Schema.c lower links
set PgAcVar(schema,panstarted) 0
if {$PgAcVar(draginfo,is_a_table)} {
	set tabnum [getTagInfo $PgAcVar(draginfo,obj) tab]
	foreach w [.pgaw:Schema.c find withtag $PgAcVar(draginfo,tabletag)] {
		if {[lsearch [.pgaw:Schema.c gettags $w] outer] != -1} {
			foreach [list PgAcVar(schema,tablex$tabnum) PgAcVar(schema,tabley$tabnum) x1 y1] [.pgaw:Schema.c coords $w] {}
			break
		}
	}
	set PgAcVar(draginfo,obj) {}
	.pgaw:Schema.c delete links
	drawLinks
	return
} 
# not a table
.pgaw:Schema.c move $PgAcVar(draginfo,obj) [expr $PgAcVar(draginfo,sx)-$x] [expr $PgAcVar(draginfo,sy)-$y]
set droptarget [.pgaw:Schema.c find overlapping $x $y $x $y]
set targettable {}
foreach item $droptarget {
	set targettable $PgAcVar(schema,tablename[getTagInfo $item tab])
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
	set sourcetable $PgAcVar(schema,tablename[getTagInfo $PgAcVar(draginfo,obj) tab])
	if {$sourcetable!=""} {
		# Source has also a tab .. tag
		set sourcefield [getTagInfo $PgAcVar(draginfo,obj) f-]
		if {$sourcetable!=$targettable} {
			lappend PgAcVar(schema,links) [list $sourcetable $sourcefield $targettable $targetfield]
			drawLinks
		}
	}
}
# Erase information about object beeing dragged
set PgAcVar(draginfo,obj) {}
}

proc {drawLinks} {} {
global PgAcVar
.pgaw:Schema.c delete links
set i 0
foreach link $PgAcVar(schema,links) {
	set sourcenum -1
	set targetnum -1
	# Compute the source and destination right edge
	foreach t [array names PgAcVar schema,tablename*] {
		if {[regexp "^$PgAcVar($t)$" [lindex $link 0] ]} {
			set sourcenum [string range $t 16 end]
		} elseif {[regexp "^$PgAcVar($t)$" [lindex $link 2] ]} {
			set targetnum [string range $t 16 end]
		} 
	}	
	set sb [findField $sourcenum [lindex $link 1]]
	set db [findField $targetnum [lindex $link 3]]
	if {($sourcenum == -1 )||($targetnum == -1)||($sb ==-1)||($db==-1)} { 
		set PgAcVar(schema,links) [lreplace $PgAcVar(schema,links) $i $i]
		showError "Link from [lindex $link 0].[lindex $link 1] to [lindex $link 2].[lindex $link 3] not found!"
	} else {

		set sre [lindex [.pgaw:Schema.c bbox tab$sourcenum] 2]
		set dre [lindex [.pgaw:Schema.c bbox tab$targetnum] 2]
		# Compute field bound boxes
		set sbbox [.pgaw:Schema.c bbox $sb]
		set dbbox [.pgaw:Schema.c bbox $db]
		# Compute the auxiliary lines
		if {[lindex $sbbox 2] < [lindex $dbbox 0]} {
			# Source object is on the left of target object
			set x1 $sre
			set y1 [expr ([lindex $sbbox 1]+[lindex $sbbox 3])/2]
			set x2 [lindex $dbbox 0]
			set y2 [expr ([lindex $dbbox 1]+[lindex $dbbox 3])/2]
			.pgaw:Schema.c create line $x1 $y1 [expr $x1+10] $y1 \
					[expr $x1+10] $y1 [expr $x2-10] $y2 \
					[expr $x2-10] $y2 $x2 $y2 \
					-tags [subst {links lkid$i}] -width 2
		} else {
			# source object is on the right of target object
			set x1 [lindex $sbbox 0]
			set y1 [expr ([lindex $sbbox 1]+[lindex $sbbox 3])/2]
			set x2 $dre
			set y2 [expr ([lindex $dbbox 1]+[lindex $dbbox 3])/2]
			.pgaw:Schema.c create line $x1 $y1 [expr $x1-10] $y1 \
					[expr $x1-10] $y1 [expr $x2+10] $y2 \
					$x2 $y2 [expr $x2+10] $y2 \
					-tags [subst {links lkid$i}] -width 2
		}
		incr i
	}
}
.pgaw:Schema.c lower links
.pgaw:Schema.c bind links <Button-1> {Schema::linkClick %x %y}
}


proc {getSchemaTabless} {} {
global PgAcVar
	set tablelist {}
	foreach key [array names PgAcVar schema,tablename*] {
		regsub schema,tablename $key "" num
		lappend tablelist $PgAcVar($key) $PgAcVar(schema,tablex$num) $PgAcVar(schema,tabley$num)
	}
	return $tablelist
}


proc {findField} {alias field} {
foreach obj [.pgaw:Schema.c find withtag f-${field}] {
	if {[lsearch [.pgaw:Schema.c gettags $obj] tab$alias] != -1} {return $obj}
	}
return -1
}


proc {addLink} {sourcetable sourcefield targettable targetfield} {
global PgAcVar
	lappend PgAcVar(schema,links) [list $sourcetable $sourcefield $targettable $targetfield]
}


proc {getTagInfo} {obj prefix} {
	set taglist [.pgaw:Schema.c gettags $obj]
	set tagpos [lsearch -regexp $taglist "^$prefix"]
	if {$tagpos==-1} {return ""}
	set thattag [lindex $taglist $tagpos]
	return [string range $thattag [string length $prefix] end]
}


proc {init} {} {
global PgAcVar
	PgAcVar:clean schema,*
	set PgAcVar(schema,nexty) 10
	set PgAcVar(schema,nextx) 10
	set PgAcVar(schema,links) {}
	set PgAcVar(schema,ntables) 0
	set PgAcVar(schema,newtablename) {}
}


proc {linkClick} {x y} {
global PgAcVar
	set obj [.pgaw:Schema.c find closest $x $y 1 links]
	if {[getTagInfo $obj link]!="s"} return
	.pgaw:Schema.c itemconfigure hili -fill black
	.pgaw:Schema.c dtag hili
	.pgaw:Schema.c addtag hili withtag $obj
	.pgaw:Schema.c itemconfigure $obj -fill blue
}


proc {canvasPanning} {x y} {
global PgAcVar
	set panstarted 0
	catch {set panstarted $PgAcVar(schema,panstarted) }
	if {!$panstarted} return
	set dx [expr $x-$PgAcVar(schema,panstartx)]
	set dy [expr $y-$PgAcVar(schema,panstarty)]
	set PgAcVar(schema,panstartx) $x
	set PgAcVar(schema,panstarty) $y
	if {$PgAcVar(schema,panobject)=="tables"} {
		.pgaw:Schema.c move mov $dx $dy
		.pgaw:Schema.c move links $dx $dy
		.pgaw:Schema.c move rect $dx $dy
	} else {
		.pgaw:Schema.c move resp $dx 0
		.pgaw:Schema.c move resgrid $dx 0
		.pgaw:Schema.c raise reshdr
	}
}


proc print {c} {
	set types {
		{{Postscript Files}	{.ps}}
		{{All Files}	*}
	}
	if {[catch {tk_getSaveFile -defaultextension .ps -filetypes $types \
				-title "Print to Postscript"} fn] || [string match {} $fn]} return
	if {[catch {::open $fn "w" } fid]} {
		return -code error "Save Error: Unable to open '$fn' for writing\n$fid"
	}
	puts $fid [$c postscript -rotate 1]
	close $fid
}


proc {canvasClick} {x y w} {
global PgAcVar
set PgAcVar(schema,panstarted) 0
if {$w==".pgaw:Schema.c"} {
	set canpan 1
	if {[llength [.pgaw:Schema.c find overlapping $x $y $x $y]]!=0} {set canpan 0}
	set PgAcVar(schema,panobject) tables
	if {$canpan} {
		if {[.pgaw:Schema.c find withtag hili]!=""} {
			.pgaw:Schema.c itemconfigure hili -fill black
			.pgaw:Schema.c dtag hili
		}

		.pgaw:Schema configure -cursor hand1
		set PgAcVar(schema,panstartx) $x
		set PgAcVar(schema,panstarty) $y
		set PgAcVar(schema,panstarted) 1
	}
}
}

}

proc vTclWindow.pgaw:Schema {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:Schema
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 759x530+10+13
	wm maxsize $base [winfo screenwidth .] [winfo screenheight .]
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 1 1
	wm title $base [intlmsg "Visual schema designer"]
	bind $base <B1-Motion> {
		Schema::canvasPanning %x %y
	}
	bind $base <Button-1> {
		Schema::canvasClick %x %y %W
	}
	bind $base <ButtonRelease-1> {
		Schema::dragStop %x %y
	}
	bind $base <Key-Delete> {
		Schema::deleteObject
	}
	canvas $base.c  -background #fefefe -borderwidth 2 -height 207 -relief ridge  -takefocus 0 -width 295 
	frame $base.f \
        -height 75 -relief groove -width 125 
	label $base.f.l -text [intlmsg {Add table}]
	entry $base.f.e \
        -background #fefefe -borderwidth 1 
	bind $base.f.e <Key-Return> {
		Schema::addNewTable
    }
	button $base.f.bdd \
        -image dnarw \
        -command {if {[winfo exists .pgaw:Schema.ddf]} {
	destroy .pgaw:Schema.ddf
} else {
	create_drop_down .pgaw:Schema 70 27 200
	focus .pgaw:Schema.ddf.sb
	foreach tbl [Database::getTablesList] {.pgaw:Schema.ddf.lb insert end $tbl}
	bind .pgaw:Schema.ddf.lb <ButtonRelease-1> {
		set i [.pgaw:Schema.ddf.lb curselection]
		if {$i!=""} {
			set PgAcVar(schema,newtablename) [.pgaw:Schema.ddf.lb get $i]
			Schema::addNewTable
		}
		destroy .pgaw:Schema.ddf
		break
	}
}} \
        -padx 1 -pady 1 
	button $base.f.btnclose \
		-command {Schema::init
Window destroy .pgaw:Schema} -padx 2 -pady 3 -text [intlmsg Close]
	button $base.f.printbtn \
		-command {Schema::print .pgaw:Schema.c} -padx 1 -pady 3 -text [intlmsg Print]
	button $base.f.btnsave \
		-command {if {$PgAcVar(schema,name)==""} then {
	showError [intlmsg "You have to supply a name for this schema!"]
	focus .pgaw:Schema.f.esn
} else {
	setCursor CLOCK
	set tables [Schema::getSchemaTabless]
	if {$PgAcVar(schema,oid)==0} then {
		set pgres [wpg_exec $CurrentDB "insert into pga_schema values ('$PgAcVar(schema,name)','$tables','$PgAcVar(schema,links)')"]
	} else {
		set pgres [wpg_exec $CurrentDB "update pga_schema set schemaname='$PgAcVar(schema,name)',schematables='$tables',schemalinks='$PgAcVar(schema,links)' where oid=$PgAcVar(schema,oid)"]
	}
	setCursor DEFAULT
	if {$PgAcVar(pgsql,status)!="PGRES_COMMAND_OK"} then {
		showError "[intlmsg {Error executing query}]\n$PgAcVar(pgsql,errmsg)"
	} else {
		Mainlib::tab_click Schema
		if {$PgAcVar(schema,oid)==0} {set PgAcVar(schema,oid) [pg_result $pgres -oid]}
	}
	catch {pg_result $pgres -clear}
}} \
		-padx 2 -pady 3 -text [intlmsg {Save schema}]
	label $base.f.ls1 -text {  } 
	entry $base.f.esn \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(schema,name) 
	label $base.f.lsn -text [intlmsg {Schema name}]
	pack $base.f.l \
		-in .pgaw:Schema.f -anchor center -expand 0 -fill none -side left 
	pack $base.f.e \
		-in .pgaw:Schema.f -anchor center -expand 0 -fill none -side left 
	pack $base.f.bdd \
		-in .pgaw:Schema.f -anchor center -expand 0 -fill none -side left 
	pack $base.f.btnclose \
		-in .pgaw:Schema.f -anchor center -expand 0 -fill none -side right 
	pack $base.f.printbtn \
		-in .pgaw:Schema.f -anchor center -expand 0 -fill none -side right 
	pack $base.f.btnsave \
		-in .pgaw:Schema.f -anchor center -expand 0 -fill none -side right 
	pack $base.f.ls1 \
		-in .pgaw:Schema.f -anchor center -expand 0 -fill none -side right 
	pack $base.f.esn \
		-in .pgaw:Schema.f -anchor center -expand 0 -fill none -side right 
	pack $base.f.lsn \
		-in .pgaw:Schema.f -anchor center -expand 0 -fill none -side right 

	pack $base.f -side top -anchor ne -expand 0 -fill x
	pack $base.c -side bottom -fill both -expand 1
}


