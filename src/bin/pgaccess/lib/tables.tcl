namespace eval Tables {


proc {new} {} {
	PgAcVar:clean nt,*
	Window show .pgaw:NewTable
	focus .pgaw:NewTable.etabn
}


proc {open} {tablename {filter ""} {order ""}} {
global PgAcVar
	set wn [getNewWindowName]
	createWindow
	set PgAcVar(mw,$wn,tablename) $tablename
	loadLayout $wn $tablename
	set PgAcVar(mw,$wn,sortfield) $order
	set PgAcVar(mw,$wn,filter) $filter
	set PgAcVar(mw,$wn,query) "select oid,\"$tablename\".* from \"$tablename\""
	set PgAcVar(mw,$wn,updatable) 1
	set PgAcVar(mw,$wn,isaquery) 0
	initVariables $wn
	refreshRecords $wn
	catch {wm title $wn "$tablename"}
}


proc {design} {tablename} {
global PgAcVar CurrentDB
	if {$CurrentDB==""} return;
	set PgAcVar(tblinfo,tablename) $tablename
	refreshTableInformation
}


proc {refreshTableInformation} {} {
global PgAcVar CurrentDB
	Window show .pgaw:TableInfo
	wm title .pgaw:TableInfo "[intlmsg {Table information}] : $PgAcVar(tblinfo,tablename)"
	.pgaw:TableInfo.f1.lb delete 0 end
	.pgaw:TableInfo.f2.fl.ilb delete 0 end
	.pgaw:TableInfo.f2.fr.lb delete 0 end
	.pgaw:TableInfo.f3.plb delete 0 end
	set PgAcVar(tblinfo,isunique) {}
	set PgAcVar(tblinfo,isclustered) {}
	set PgAcVar(tblinfo,indexfields) {}
	wpg_select $CurrentDB "select attnum,attname,typname,attlen,attnotnull,atttypmod,usename,usesysid,pg_class.oid,relpages,reltuples,relhaspkey,relhasrules,relacl from pg_class,pg_user,pg_attribute,pg_type where (pg_class.relname='$PgAcVar(tblinfo,tablename)') and (pg_class.oid=pg_attribute.attrelid) and (pg_class.relowner=pg_user.usesysid) and (pg_attribute.atttypid=pg_type.oid) order by attnum" rec {
		set fsize $rec(attlen)
		set fsize1 $rec(atttypmod)
		set ftype $rec(typname)
		if { $fsize=="-1" && $fsize1!="-1" } {
			set fsize $rec(atttypmod)
			incr fsize -4
		}
		if { $fsize1=="-1" && $fsize=="-1" } {
			set fsize ""
		}
		if {$rec(attnotnull) == "t"} {
			set notnull "NOT NULL"
		} else {
			set notnull {}
		}
		if {$rec(attnum)>0} {.pgaw:TableInfo.f1.lb insert end [format "%-33.33s %-14.14s %6.6s    %-8.8s" $rec(attname) $ftype $fsize $notnull]}
		set PgAcVar(tblinfo,owner) $rec(usename)
		set PgAcVar(tblinfo,tableoid) $rec(oid)
		set PgAcVar(tblinfo,ownerid) $rec(usesysid)
		set PgAcVar(tblinfo,f$rec(attnum)) $rec(attname)
		set PgAcVar(tblinfo,numtuples) $rec(reltuples)
		set PgAcVar(tblinfo,numpages) $rec(relpages)
		set PgAcVar(tblinfo,permissions) $rec(relacl)
		if {$rec(relhaspkey)=="t"} {
			set PgAcVar(tblinfo,hasprimarykey) [intlmsg Yes]
		} else {
			set PgAcVar(tblinfo,hasprimarykey) [intlmsg No]
		}
		if {$rec(relhasrules)=="t"} {
			set PgAcVar(tblinfo,hasrules) [intlmsg Yes]
		} else {
			set PgAcVar(tblinfo,hasrules) [intlmsg No]
		}
	}
	set PgAcVar(tblinfo,indexlist) {}
	wpg_select $CurrentDB "select oid,indexrelid from pg_index where (pg_class.relname='$PgAcVar(tblinfo,tablename)') and (pg_class.oid=pg_index.indrelid)" rec {
		lappend PgAcVar(tblinfo,indexlist) $rec(oid)
		wpg_select $CurrentDB "select relname from pg_class where oid=$rec(indexrelid)" rec1 {
			.pgaw:TableInfo.f2.fl.ilb insert end $rec1(relname)
		}
	}
	#
	# showing permissions
	set temp $PgAcVar(tblinfo,permissions)
	regsub "^\{" $temp {} temp
	regsub "\}$" $temp {} temp
	regsub -all "\"" $temp {} temp
	foreach token [split $temp ,] {
		set oli [split $token =]
		set uname [lindex $oli 0]
		set rights [lindex $oli 1]
		if {$uname == ""} {set uname PUBLIC}
		set r_select " "
		set r_update " "
		set r_insert " "
		set r_rule   " "
		if {[string first r $rights] != -1} {set r_select x}
		if {[string first w $rights] != -1} {set r_update x}
		if {[string first a $rights] != -1} {set r_insert x}
		if {[string first R $rights] != -1} {set r_rule   x}
		#
		# changing the format of the following line can affect the loadPermissions procedure
		# see below
		.pgaw:TableInfo.f3.plb insert end [format "%-23.23s %11s %11s %11s %11s" $uname $r_select $r_update $r_insert $r_rule]
		
	}
}

proc {loadPermissions} {} {
global PgAcVar
	set sel [.pgaw:TableInfo.f3.plb curselection]
	if {$sel == ""} {
		bell
		return
	}
	set line [.pgaw:TableInfo.f3.plb get $sel]
	set uname [string trim [string range $line 0 22]]
	Window show .pgaw:Permissions
	wm transient .pgaw:Permissions .pgaw:TableInfo
	set PgAcVar(permission,username) $uname
	set PgAcVar(permission,select) [expr {"x"==[string range $line 34 34]}]
	set PgAcVar(permission,update) [expr {"x"==[string range $line 46 46]}]
	set PgAcVar(permission,insert) [expr {"x"==[string range $line 58 58]}]
	set PgAcVar(permission,rule)   [expr {"x"==[string range $line 70 70]}]
	focus .pgaw:Permissions.f1.ename
}


proc {newPermissions} {} {
global PgAcVar
	PgAcVar:clean permission,*
	Window show .pgaw:Permissions
	wm transient .pgaw:Permissions .pgaw:TableInfo
	focus .pgaw:Permissions.f1.ename
}


proc {savePermissions} {} {
global PgAcVar
	if {$PgAcVar(permission,username) == ""} {
		showError [intlmsg "User without name?"]
		return
	}
	if {$PgAcVar(permission,username)=="PUBLIC"} {
		set usrname PUBLIC
	} else {
		set usrname "\"$PgAcVar(permission,username)\""
	}
	sql_exec noquiet "revoke all on \"$PgAcVar(tblinfo,tablename)\" from $usrname"
	if {$PgAcVar(permission,select)} {
		sql_exec noquiet "GRANT SELECT on \"$PgAcVar(tblinfo,tablename)\" to $usrname"
	}
	if {$PgAcVar(permission,insert)} {
		sql_exec noquiet "GRANT INSERT on \"$PgAcVar(tblinfo,tablename)\" to $usrname"
	}
	if {$PgAcVar(permission,update)} {
		sql_exec noquiet "GRANT UPDATE on \"$PgAcVar(tblinfo,tablename)\" to $usrname"
	}
	if {$PgAcVar(permission,rule)} {
		sql_exec noquiet "GRANT RULE on \"$PgAcVar(tblinfo,tablename)\" to $usrname"
	}
	refreshTableInformation
}


proc {clusterIndex} {} {
global PgAcVar
	set sel [.pgaw:TableInfo.f2.fl.ilb curselection]
	if {$sel == ""} {
		showError [intlmsg "You have to select an index!"]
		return
	}
	bell
	if {[tk_messageBox -title [intlmsg Warning] -parent .pgaw:TableInfo -message [format [intlmsg "You choose to cluster index\n\n %s \n\nAll other indices will be lost!\nProceed?"] [.pgaw:TableInfo.f2.fl.ilb get $sel]] -type yesno -default no]=="no"} {return}
	if {[sql_exec noquiet "cluster \"[.pgaw:TableInfo.f2.fl.ilb get $sel]\" on \"$PgAcVar(tblinfo,tablename)\""]} {
		refreshTableInformation
	}		
}


proc {get_tag_info} {wn itemid prefix} {
	set taglist [$wn.c itemcget $itemid -tags]
	set i [lsearch -glob $taglist $prefix*]
	set thetag [lindex $taglist $i]
	return [string range $thetag 1 end]
}


proc {dragMove} {w x y} {
global PgAcVar
	set dlo ""
	catch { set dlo $PgAcVar(draglocation,obj) }
	if {$dlo != ""} {
		set dx [expr $x - $PgAcVar(draglocation,x)]
		set dy [expr $y - $PgAcVar(draglocation,y)]
		$w move $dlo $dx $dy
		set PgAcVar(draglocation,x) $x
		set PgAcVar(draglocation,y) $y
	}
}


proc {dragStart} {wn w x y} {
global PgAcVar
	PgAcVar:clean draglocation,*
	set object [$w find closest $x $y]
	if {[lsearch [$wn.c gettags $object] movable]==-1} return;
	$wn.c bind movable <Leave> {}
	set PgAcVar(draglocation,obj) $object
	set PgAcVar(draglocation,x) $x
	set PgAcVar(draglocation,y) $y
	set PgAcVar(draglocation,start) $x
}


proc {dragStop} {wn w x y} {
global PgAcVar CurrentDB
	set dlo ""
	catch { set dlo $PgAcVar(draglocation,obj) }
	if {$dlo != ""} {
		$wn.c bind movable <Leave> "$wn configure -cursor left_ptr"
		$wn configure -cursor left_ptr
		set ctr [get_tag_info $wn $PgAcVar(draglocation,obj) v]
		set diff [expr $x-$PgAcVar(draglocation,start)]
		if {$diff==0} return;
		set newcw {}
		for {set i 0} {$i<$PgAcVar(mw,$wn,colcount)} {incr i} {
			if {$i==$ctr} {
		lappend newcw [expr [lindex $PgAcVar(mw,$wn,colwidth) $i]+$diff]
			} else {
		lappend newcw [lindex $PgAcVar(mw,$wn,colwidth) $i]
			}
		}
		set PgAcVar(mw,$wn,colwidth) $newcw
		$wn.c itemconfigure c$ctr -width [expr [lindex $PgAcVar(mw,$wn,colwidth) $ctr]-5]
		drawHeaders $wn
		drawHorizontalLines $wn
		if {$PgAcVar(mw,$wn,crtrow)!=""} {showRecord $wn $PgAcVar(mw,$wn,crtrow)}
		for {set i [expr $ctr+1]} {$i<$PgAcVar(mw,$wn,colcount)} {incr i} {
			$wn.c move c$i $diff 0
		}
		setCursor CLOCK
		sql_exec quiet "update pga_layout set colwidth='$PgAcVar(mw,$wn,colwidth)' where tablename='$PgAcVar(mw,$wn,layout_name)'"
		setCursor DEFAULT
	}
}


proc {canvasClick} {wn x y} {
global PgAcVar
	if {![finishEdit $wn]} return
	# Determining row
	for {set row 0} {$row<$PgAcVar(mw,$wn,nrecs)} {incr row} {
		if {[lindex $PgAcVar(mw,$wn,rowy) $row]>$y} break
	}
	incr row -1
	if {$y>[lindex $PgAcVar(mw,$wn,rowy) $PgAcVar(mw,$wn,last_rownum)]} {set row $PgAcVar(mw,$wn,last_rownum)}
	if {$row<0} return
	set PgAcVar(mw,$wn,row_edited) $row
	set PgAcVar(mw,$wn,crtrow) $row
	showRecord $wn $row
	if {$PgAcVar(mw,$wn,errorsavingnew)} return
	# Determining column
	set posx [expr -$PgAcVar(mw,$wn,leftoffset)]
	set col 0
	foreach cw $PgAcVar(mw,$wn,colwidth) {
		incr posx [expr $cw+2]
		if {$x<$posx} break
		incr col
	}
	set itlist [$wn.c find withtag r$row]
	foreach item $itlist {
		if {[get_tag_info $wn $item c]==$col} {
			startEdit $wn $item $x $y
			break
		}
	}
}


proc {deleteRecord} {wn} {
global PgAcVar CurrentDB
	if {!$PgAcVar(mw,$wn,updatable)} return;
	if {![finishEdit $wn]} return;
	set taglist [$wn.c gettags hili]
	if {[llength $taglist]==0} return;
	set rowtag [lindex $taglist [lsearch -regexp $taglist "^r"]]
	set row [string range $rowtag 1 end]
	set oid [lindex $PgAcVar(mw,$wn,keylist) $row]
	if {[tk_messageBox -title [intlmsg "FINAL WARNING"] -icon question -parent $wn -message [intlmsg "Delete current record ?"] -type yesno -default no]=="no"} return
	if {[sql_exec noquiet "delete from \"$PgAcVar(mw,$wn,tablename)\" where oid=$oid"]} {
		$wn.c delete hili
	}
}


proc {drawHeaders} {wn} {
global PgAcVar
	$wn.c delete header
	set posx [expr 5-$PgAcVar(mw,$wn,leftoffset)]
	for {set i 0} {$i<$PgAcVar(mw,$wn,colcount)} {incr i} {
		set xf [expr $posx+[lindex $PgAcVar(mw,$wn,colwidth) $i]]
		$wn.c create rectangle $posx 1 $xf 22 -fill #CCCCCC -outline "" -width 0 -tags header
		$wn.c create text [expr $posx+[lindex $PgAcVar(mw,$wn,colwidth) $i]*1.0/2] 14 -text [lindex $PgAcVar(mw,$wn,colnames) $i] -tags header -fill navy -font $PgAcVar(pref,font_normal)
		$wn.c create line $posx 22 [expr $xf-1] 22 -fill #AAAAAA -tags header
		$wn.c create line [expr $xf-1] 5 [expr $xf-1] 22 -fill #AAAAAA -tags header
		$wn.c create line [expr $xf+1] 5 [expr $xf+1] 22 -fill white -tags header
		$wn.c create line $xf -15000 $xf 15000 -fill #CCCCCC -tags [subst {header movable v$i}]
		set posx [expr $xf+2]
	}
	set PgAcVar(mw,$wn,r_edge) $posx
	$wn.c bind movable <Button-1> "Tables::dragStart $wn %W %x %y"
	$wn.c bind movable <B1-Motion> {Tables::dragMove %W %x %y}
	$wn.c bind movable <ButtonRelease-1> "Tables::dragStop $wn %W %x %y"
	$wn.c bind movable <Enter> "$wn configure -cursor left_side"
	$wn.c bind movable <Leave> "$wn configure -cursor left_ptr"
}


proc {drawHorizontalLines} {wn} {
global PgAcVar
	$wn.c delete hgrid
	set posx 10
	for {set j 0} {$j<$PgAcVar(mw,$wn,colcount)} {incr j} {
		set ledge($j) $posx
		incr posx [expr [lindex $PgAcVar(mw,$wn,colwidth) $j]+2]
		set textwidth($j) [expr [lindex $PgAcVar(mw,$wn,colwidth) $j]-5]
	}
	incr posx -6
	for {set i 0} {$i<$PgAcVar(mw,$wn,nrecs)} {incr i} {
		$wn.c create line [expr -$PgAcVar(mw,$wn,leftoffset)] [lindex $PgAcVar(mw,$wn,rowy) [expr $i+1]] [expr $posx-$PgAcVar(mw,$wn,leftoffset)] [lindex $PgAcVar(mw,$wn,rowy) [expr $i+1]] -fill gray -tags [subst {hgrid g$i}]
	}
	if {$PgAcVar(mw,$wn,updatable)} {
		set i $PgAcVar(mw,$wn,nrecs)
		set posy [expr 14+[lindex $PgAcVar(mw,$wn,rowy) $PgAcVar(mw,$wn,nrecs)]]
		$wn.c create line [expr -$PgAcVar(mw,$wn,leftoffset)] $posy [expr $posx-$PgAcVar(mw,$wn,leftoffset)] $posy -fill gray -tags [subst {hgrid g$i}]
	}
}


proc {drawNewRecord} {wn} {
global PgAcVar
	set posx [expr 10-$PgAcVar(mw,$wn,leftoffset)]
	set posy [lindex $PgAcVar(mw,$wn,rowy) $PgAcVar(mw,$wn,last_rownum)]
	if {$PgAcVar(pref,tvfont)=="helv"} {
		set tvfont $PgAcVar(pref,font_normal)
	} else {
		set tvfont $PgAcVar(pref,font_fix)
	}
	if {$PgAcVar(mw,$wn,updatable)} {
	  for {set j 0} {$j<$PgAcVar(mw,$wn,colcount)} {incr j} {
		$wn.c create text $posx $posy -text * -tags [subst {r$PgAcVar(mw,$wn,nrecs) c$j q new unt}]  -anchor nw -font $tvfont -width [expr [lindex $PgAcVar(mw,$wn,colwidth) $j]-5]
		incr posx [expr [lindex $PgAcVar(mw,$wn,colwidth) $j]+2]
	  }
	  incr posy 14
	  $wn.c create line [expr -$PgAcVar(mw,$wn,leftoffset)] $posy [expr $PgAcVar(mw,$wn,r_edge)-$PgAcVar(mw,$wn,leftoffset)] $posy -fill gray -tags [subst {hgrid g$PgAcVar(mw,$wn,nrecs)}]
	}
}


proc {editMove} { wn {distance 1} {position end} } {
	global PgAcVar 

	# This routine moves the cursor some relative distance
	# from one cell being editted to another cell in the table.
	# Typical distances are 1, +1, $PgAcVar(mw,$wn,colcount), and 
	# -$PgAcVar(mw,$wn,colcount).  Position is where
	# the cursor will be placed within the cell.  The valid
	# positions are 0 and end.

	# get the current row and column
	set current_cell_id $PgAcVar(mw,$wn,id_edited)
	set tags [$wn.c gettags $current_cell_id] 
	regexp {r([0-9]+)} $tags match crow
	regexp {c([0-9]+)} $tags match ccol


	# calculate next row and column
	set colcount $PgAcVar(mw,$wn,colcount)
	set ccell [expr ($crow * $colcount) + $ccol]
	set ncell [expr $ccell + $distance]
	set nrow [expr $ncell / $colcount]
	set ncol [expr $ncell % $colcount]


	# find the row of the next cell
	if {$distance < 0} {
		set row_increment -1
	} else {
		set row_increment 1
	}
	set id_tuple [$wn.c find withtag r$nrow] 
	# skip over deleted rows...
	while {[llength $id_tuple] == 0} {
		# case above first row of table
		if {$nrow < 0} {
			return
		# case at or beyond last row of table
		} elseif {$nrow >= $PgAcVar(mw,$wn,nrecs)} {
			if {![insertNewRecord $wn]} {
		   set PgAcVar(mw,$wn,errorsavingnew) 1
			return
		  }
		  set id_tuple [$wn.c find withtag r$nrow] 
		  break
		}
	incr nrow $row_increment
		set id_tuple [$wn.c find withtag r$nrow] 
	}

	# find the widget id of the next cell
	set next_cell_id [lindex [lsort -integer $id_tuple] $ncol]
	if {[string compare $next_cell_id {}] == 0} {
		set next_cell_id [$wn.c find withtag $current_cell_id]
	}

	# make sure that the new cell is in the visible window
	set toprec $PgAcVar(mw,$wn,toprec)
	set numscreenrecs [getVisibleRecordsCount $wn]
	if {$nrow < $toprec} {
	   # case nrow above visable window
	   scrollWindow $wn moveto \
		[expr $nrow *[recordSizeInScrollbarUnits $wn]]
	} elseif {$nrow > ($toprec + $numscreenrecs - 1)} {
	   # case nrow below visable window
		scrollWindow $wn moveto \
		[expr ($nrow - $numscreenrecs + 2) * [recordSizeInScrollbarUnits $wn]]
	}
	# I need to find a better way to pan -kk
	foreach {x1 y1 x2 y2}  [$wn.c bbox $next_cell_id] {break}
	while {$x1 <= $PgAcVar(mw,$wn,leftoffset)} {
		panRight $wn
		foreach {x1 y1 x2 y2}  [$wn.c bbox $next_cell_id] {break}
	}
	set rightedge [expr $x1 + [lindex $PgAcVar(mw,$wn,colwidth) $ncol]]
	while {$rightedge > ($PgAcVar(mw,$wn,leftoffset) + [winfo width $wn.c])} {
		panLeft $wn
	}

	# move to the next cell
	foreach {x1 y1 x2 y2}  [$wn.c bbox $next_cell_id] {break}
	switch -exact -- $position {
		0 {
			canvasClick $wn [incr x1  ] [incr y1 ]
	}
	end -
		default {
			canvasClick $wn [incr x2  -1] [incr y2 -1]
	}
	}
}


proc {editText} {wn c k} {
global PgAcVar
set bbin [$wn.c bbox r$PgAcVar(mw,$wn,row_edited)]
switch $k {
	BackSpace { set dp [expr [$wn.c index $PgAcVar(mw,$wn,id_edited) insert]-1];if {$dp>=0} {$wn.c dchars $PgAcVar(mw,$wn,id_edited) $dp $dp; set PgAcVar(mw,$wn,dirtyrec) 1}}
	Home {$wn.c icursor $PgAcVar(mw,$wn,id_edited) 0}
	End {$wn.c icursor $PgAcVar(mw,$wn,id_edited) end}
		Left {
			set position [expr [$wn.c index $PgAcVar(mw,$wn,id_edited) insert]-1]
			if {$position < 0} {
		editMove $wn -1 end
		return
			}
			$wn.c icursor $PgAcVar(mw,$wn,id_edited) $position
		}
	Delete {}
		Right {
			set position [expr [$wn.c index $PgAcVar(mw,$wn,id_edited) insert]+1]
			if {$position > [$wn.c index $PgAcVar(mw,$wn,id_edited) end] } {
		editMove $wn 1 0
		return
			}
			$wn.c icursor $PgAcVar(mw,$wn,id_edited) $position
		}
		Return -
		Tab {editMove $wn; return}
		ISO_Left_Tab {editMove $wn -1; return}
		Up  {editMove $wn -$PgAcVar(mw,$wn,colcount); return }
		Down  {editMove $wn $PgAcVar(mw,$wn,colcount); return }
	Escape {set PgAcVar(mw,$wn,dirtyrec) 0; $wn.c itemconfigure $PgAcVar(mw,$wn,id_edited) -text $PgAcVar(mw,$wn,text_initial_value); $wn.c focus {}}
	default {if {[string compare $c " "]>-1} {$wn.c insert $PgAcVar(mw,$wn,id_edited) insert $c;set PgAcVar(mw,$wn,dirtyrec) 1}}
}
set bbout [$wn.c bbox r$PgAcVar(mw,$wn,row_edited)]
set dy [expr [lindex $bbout 3]-[lindex $bbin 3]]
if {$dy==0} return
set re $PgAcVar(mw,$wn,row_edited)
$wn.c move g$re 0 $dy
for {set i [expr 1+$re]} {$i<=$PgAcVar(mw,$wn,nrecs)} {incr i} {
	$wn.c move r$i 0 $dy
	$wn.c move g$i 0 $dy
	set rh [lindex $PgAcVar(mw,$wn,rowy) $i]
	incr rh $dy
	set PgAcVar(mw,$wn,rowy) [lreplace $PgAcVar(mw,$wn,rowy) $i $i $rh]
}
showRecord $wn $PgAcVar(mw,$wn,row_edited)
# Delete is trapped by window interpreted as record delete
#    Delete {$wn.c dchars $PgAcVar(mw,$wn,id_edited) insert insert; set PgAcVar(mw,$wn,dirtyrec) 1}
}


proc {finishEdit} {wn} {
global PgAcVar CurrentDB
# User has edited the text ?
if {!$PgAcVar(mw,$wn,dirtyrec)} {
	# No, unfocus text
	$wn.c focus {}
	# For restoring * to the new record position
	if {$PgAcVar(mw,$wn,id_edited)!=""} {
		if {[lsearch [$wn.c gettags $PgAcVar(mw,$wn,id_edited)] new]!=-1} {
			$wn.c itemconfigure $PgAcVar(mw,$wn,id_edited) -text $PgAcVar(mw,$wn,text_initial_value)
		}
	}
	set PgAcVar(mw,$wn,id_edited) {};set PgAcVar(mw,$wn,text_initial_value) {}
	return 1
}
# Trimming the spaces
set fldval [string trim [$wn.c itemcget $PgAcVar(mw,$wn,id_edited) -text]]
$wn.c itemconfigure $PgAcVar(mw,$wn,id_edited) -text $fldval
if {[string compare $PgAcVar(mw,$wn,text_initial_value) $fldval]==0} {
	set PgAcVar(mw,$wn,dirtyrec) 0
	$wn.c focus {}
	set PgAcVar(mw,$wn,id_edited) {};set PgAcVar(mw,$wn,text_initial_value) {}
	return 1
}
setCursor CLOCK
set oid [lindex $PgAcVar(mw,$wn,keylist) $PgAcVar(mw,$wn,row_edited)]
set fld [lindex $PgAcVar(mw,$wn,colnames) [get_tag_info $wn $PgAcVar(mw,$wn,id_edited) c]]
set fillcolor black
if {$PgAcVar(mw,$wn,row_edited)==$PgAcVar(mw,$wn,last_rownum)} {
	set fillcolor red
	set sfp [lsearch $PgAcVar(mw,$wn,newrec_fields) "\"$fld\""]
	if {$sfp>-1} {
		set PgAcVar(mw,$wn,newrec_fields) [lreplace $PgAcVar(mw,$wn,newrec_fields) $sfp $sfp]
		set PgAcVar(mw,$wn,newrec_values) [lreplace $PgAcVar(mw,$wn,newrec_values) $sfp $sfp]
	}			
	lappend PgAcVar(mw,$wn,newrec_fields) "\"$fld\""
	lappend PgAcVar(mw,$wn,newrec_values) '$fldval'
	# Remove the untouched tag from the object
	$wn.c dtag $PgAcVar(mw,$wn,id_edited) unt
		$wn.c itemconfigure $PgAcVar(mw,$wn,id_edited) -fill red
	set retval 1
} else {
	set PgAcVar(mw,$wn,msg) "Updating record ..."
	after 1000 "set PgAcVar(mw,$wn,msg) {}"
	regsub -all ' $fldval  \\' sqlfldval

#FIXME rjr 4/29/1999 special case null so it can be entered into tables
#really need to write a tcl sqlquote proc which quotes the string only
#if necessary, so it can be used all over pgaccess, instead of explicit 's

	if {$sqlfldval == "null"} {
		set retval [sql_exec noquiet "update \"$PgAcVar(mw,$wn,tablename)\" \
		set \"$fld\"= null where oid=$oid"]
	} else {
		set retval [sql_exec noquiet "update \"$PgAcVar(mw,$wn,tablename)\" \
		set \"$fld\"='$sqlfldval' where oid=$oid"]
	}
}
setCursor DEFAULT
if {!$retval} {
	set PgAcVar(mw,$wn,msg) ""
	focus $wn.c
	return 0
}
set PgAcVar(mw,$wn,dirtyrec) 0
$wn.c focus {}
set PgAcVar(mw,$wn,id_edited) {};set PgAcVar(mw,$wn,text_initial_value) {}
return 1
}

proc {loadLayout} {wn layoutname} {
global PgAcVar CurrentDB
	setCursor CLOCK
	set PgAcVar(mw,$wn,layout_name) $layoutname
	catch {unset PgAcVar(mw,$wn,colcount) PgAcVar(mw,$wn,colnames) PgAcVar(mw,$wn,colwidth)}
	set PgAcVar(mw,$wn,layout_found) 0
	set pgres [wpg_exec $CurrentDB "select *,oid from pga_layout where tablename='$layoutname' order by oid desc"]
	set pgs [pg_result $pgres -status]
	if {$pgs!="PGRES_TUPLES_OK"} {
		# Probably table pga_layout isn't yet defined
		sql_exec noquiet "create table pga_layout (tablename varchar(64),nrcols int2,colnames text,colwidth text)"
		sql_exec quiet "grant ALL on pga_layout to PUBLIC"
	} else {
		set nrlay [pg_result $pgres -numTuples]
		if {$nrlay>=1} {
			set layoutinfo [pg_result $pgres -getTuple 0]
			set PgAcVar(mw,$wn,colcount) [lindex $layoutinfo 1]
			set PgAcVar(mw,$wn,colnames)  [lindex $layoutinfo 2]
			set PgAcVar(mw,$wn,colwidth) [lindex $layoutinfo 3]
			set goodoid [lindex $layoutinfo 4]
			set PgAcVar(mw,$wn,layout_found) 1
		}
		if {$nrlay>1} {
			showError "Multiple ($nrlay) layout info found\n\nPlease report the bug!"
			sql_exec quiet "delete from pga_layout where (tablename='$PgAcVar(mw,$wn,tablename)') and (oid<>$goodoid)"
		}
	}
	pg_result $pgres -clear
}


proc {panLeft} {wn } {
global PgAcVar
	if {![finishEdit $wn]} return;
	if {$PgAcVar(mw,$wn,leftcol)==[expr $PgAcVar(mw,$wn,colcount)-1]} return;
	set diff [expr 2+[lindex $PgAcVar(mw,$wn,colwidth) $PgAcVar(mw,$wn,leftcol)]]
	incr PgAcVar(mw,$wn,leftcol)
	incr PgAcVar(mw,$wn,leftoffset) $diff
	$wn.c move header -$diff 0
	$wn.c move q -$diff 0
	$wn.c move hgrid -$diff 0
}


proc {panRight} {wn} {
global PgAcVar
	if {![finishEdit $wn]} return;
	if {$PgAcVar(mw,$wn,leftcol)==0} return;
	incr PgAcVar(mw,$wn,leftcol) -1
	set diff [expr 2+[lindex $PgAcVar(mw,$wn,colwidth) $PgAcVar(mw,$wn,leftcol)]]
	incr PgAcVar(mw,$wn,leftoffset) -$diff
	$wn.c move header $diff 0
	$wn.c move q $diff 0
	$wn.c move hgrid $diff 0
}


proc {insertNewRecord} {wn} {
global PgAcVar CurrentDB
	if {![finishEdit $wn]} {return 0}
	if {$PgAcVar(mw,$wn,newrec_fields)==""} {return 1}
	set PgAcVar(mw,$wn,msg) "Saving new record ..."
	after 1000 "set PgAcVar(mw,$wn,msg) {}"
	set pgres [wpg_exec $CurrentDB "insert into \"$PgAcVar(mw,$wn,tablename)\" ([join $PgAcVar(mw,$wn,newrec_fields) ,]) values ([join $PgAcVar(mw,$wn,newrec_values) ,])" ]
	if {[pg_result $pgres -status]!="PGRES_COMMAND_OK"} {
		set errmsg [pg_result $pgres -error]
		showError "[intlmsg {Error inserting new record}]\n\n$errmsg"
		return 0
	}
	set oid [pg_result $pgres -oid]
	lappend PgAcVar(mw,$wn,keylist) $oid
	pg_result $pgres -clear
	# Get bounds of the last record
	set lrbb [$wn.c bbox new]
	lappend PgAcVar(mw,$wn,rowy) [lindex $lrbb 3]
	$wn.c itemconfigure new -fill black
	$wn.c dtag q new
	# Replace * from untouched new row elements with "  "
	foreach item [$wn.c find withtag unt] {
		$wn.c itemconfigure $item -text "  "
	}
	$wn.c dtag q unt
	incr PgAcVar(mw,$wn,last_rownum)
	incr PgAcVar(mw,$wn,nrecs)
	drawNewRecord $wn
	set PgAcVar(mw,$wn,newrec_fields) {}
	set PgAcVar(mw,$wn,newrec_values) {}
	return 1
}


proc {scrollWindow} {wn par1 args} {
global PgAcVar
	if {![finishEdit $wn]} return;
	if {$par1=="scroll"} {
		set newtop $PgAcVar(mw,$wn,toprec)
		if {[lindex $args 1]=="units"} {
			incr newtop [lindex $args 0]
		} else {
			incr newtop [expr [lindex $args 0]*25]
			if {$newtop<0} {set newtop 0}
			if {$newtop>=[expr $PgAcVar(mw,$wn,nrecs)-1]} {set newtop [expr $PgAcVar(mw,$wn,nrecs)-1]}
		}
	} elseif {$par1=="moveto"} {
		set newtop [expr int([lindex $args 0]*$PgAcVar(mw,$wn,nrecs))]
	} else {
		return
	}
	if {$newtop<0} return;
	if {$newtop>=[expr $PgAcVar(mw,$wn,nrecs)-1]} return;
	set dy [expr [lindex $PgAcVar(mw,$wn,rowy) $PgAcVar(mw,$wn,toprec)]-[lindex $PgAcVar(mw,$wn,rowy) $newtop]]
	$wn.c move q 0 $dy
	$wn.c move hgrid 0 $dy
	set newrowy {}
	foreach y $PgAcVar(mw,$wn,rowy) {lappend newrowy [expr $y+$dy]}
	set PgAcVar(mw,$wn,rowy) $newrowy
	set PgAcVar(mw,$wn,toprec) $newtop
	setScrollbar $wn
}


proc {initVariables} {wn} {
global PgAcVar
	set PgAcVar(mw,$wn,newrec_fields) {}
	set PgAcVar(mw,$wn,newrec_values) {}
}

proc {selectRecords} {wn sql} {
global PgAcVar CurrentDB
if {![finishEdit $wn]} return;
initVariables $wn
$wn.c delete q
$wn.c delete header
$wn.c delete hgrid
$wn.c delete new
set PgAcVar(mw,$wn,leftcol) 0
set PgAcVar(mw,$wn,leftoffset) 0
set PgAcVar(mw,$wn,crtrow) {}
set PgAcVar(mw,$wn,msg) [intlmsg "Accessing data. Please wait ..."]
catch {$wn.f1.b1 configure -state disabled}
setCursor CLOCK
set is_error 1
if {[sql_exec noquiet "BEGIN"]} {
	if {[sql_exec noquiet "declare mycursor cursor for $sql"]} {
		set pgres [wpg_exec $CurrentDB "fetch $PgAcVar(pref,rows) in mycursor"]
		if {$PgAcVar(pgsql,status)=="PGRES_TUPLES_OK"} {
			set is_error 0
		}
	}
}
if {$is_error} {
	sql_exec quiet "END"
	set PgAcVar(mw,$wn,msg) {}
	catch {$wn.f1.b1 configure -state normal}
	setCursor DEFAULT
	set PgAcVar(mw,$wn,msg) "Error executing : $sql"
	return
}
if {$PgAcVar(mw,$wn,updatable)} then {set shift 1} else {set shift 0}
#
# checking at least the numer of fields
set attrlist [pg_result $pgres -lAttributes]
if {$PgAcVar(mw,$wn,layout_found)} then {
	if {  ($PgAcVar(mw,$wn,colcount) != [expr [llength $attrlist]-$shift]) ||
		  ($PgAcVar(mw,$wn,colcount) != [llength $PgAcVar(mw,$wn,colnames)]) ||
		  ($PgAcVar(mw,$wn,colcount) != [llength $PgAcVar(mw,$wn,colwidth)]) } then {
		# No. of columns don't match, something is wrong
		# tk_messageBox -title [intlmsg Information] -message "Layout info changed !\nRescanning..."
		set PgAcVar(mw,$wn,layout_found) 0
		sql_exec quiet "delete from pga_layout where tablename='$PgAcVar(mw,$wn,layout_name)'"
	}
}
# Always take the col. names from the result
set PgAcVar(mw,$wn,colcount) [llength $attrlist]
if {$PgAcVar(mw,$wn,updatable)} then {incr PgAcVar(mw,$wn,colcount) -1}
set PgAcVar(mw,$wn,colnames) {}
# In defPgAcVar(mw,$wn,colwidth) prepare PgAcVar(mw,$wn,colwidth) (in case that not layout_found)
set defPgAcVar(mw,$wn,colwidth) {}
for {set i 0} {$i<$PgAcVar(mw,$wn,colcount)} {incr i} {
	lappend PgAcVar(mw,$wn,colnames) [lindex [lindex $attrlist [expr {$i+$shift}]] 0]
	lappend defPgAcVar(mw,$wn,colwidth) 150
}
if {!$PgAcVar(mw,$wn,layout_found)} {
	set PgAcVar(mw,$wn,colwidth) $defPgAcVar(mw,$wn,colwidth)
	sql_exec quiet "insert into pga_layout values ('$PgAcVar(mw,$wn,layout_name)',$PgAcVar(mw,$wn,colcount),'$PgAcVar(mw,$wn,colnames)','$PgAcVar(mw,$wn,colwidth)')"
	set PgAcVar(mw,$wn,layout_found) 1
}
set PgAcVar(mw,$wn,nrecs) [pg_result $pgres -numTuples]
if {$PgAcVar(mw,$wn,nrecs)>$PgAcVar(pref,rows)} {
	set PgAcVar(mw,$wn,msg) "Only first $PgAcVar(pref,rows) records from $PgAcVar(mw,$wn,nrecs) have been loaded"
	set PgAcVar(mw,$wn,nrecs) $PgAcVar(pref,rows)
}
set tagoid {}
if {$PgAcVar(pref,tvfont)=="helv"} {
	set tvfont $PgAcVar(pref,font_normal)
} else {
	set tvfont $PgAcVar(pref,font_fix)
}
# Computing column's left edge
set posx 10
for {set j 0} {$j<$PgAcVar(mw,$wn,colcount)} {incr j} {
	set ledge($j) $posx
	incr posx [expr {[lindex $PgAcVar(mw,$wn,colwidth) $j]+2}]
	set textwidth($j) [expr {[lindex $PgAcVar(mw,$wn,colwidth) $j]-5}]
}
incr posx -6
set posy 24
drawHeaders $wn
set PgAcVar(mw,$wn,updatekey) oid
set PgAcVar(mw,$wn,keylist) {}
set PgAcVar(mw,$wn,rowy) {24}
set PgAcVar(mw,$wn,msg) "Loading maximum $PgAcVar(pref,rows) records ..."
set wupdatable $PgAcVar(mw,$wn,updatable)
for {set i 0} {$i<$PgAcVar(mw,$wn,nrecs)} {incr i} {
	set curtup [pg_result $pgres -getTuple $i]
	if {$wupdatable} then {lappend PgAcVar(mw,$wn,keylist) [lindex $curtup 0]}
	for {set j 0} {$j<$PgAcVar(mw,$wn,colcount)} {incr j} {
		$wn.c create text $ledge($j) $posy -text [lindex $curtup [expr {$j+$shift}]] -tags [subst {r$i c$j q}] -anchor nw -font $tvfont -width $textwidth($j) -fill black
	}
	set bb [$wn.c bbox r$i]
	incr posy [expr {[lindex $bb 3]-[lindex $bb 1]}]
	lappend PgAcVar(mw,$wn,rowy) $posy
	$wn.c create line 0 [lindex $bb 3] $posx [lindex $bb 3] -fill gray -tags [subst {hgrid g$i}]
	if {$i==25} {update; update idletasks}
}
after 3000 "set PgAcVar(mw,$wn,msg) {}"
set PgAcVar(mw,$wn,last_rownum) $i
# Defining position for input data
drawNewRecord $wn
pg_result $pgres -clear
sql_exec quiet "END"
set PgAcVar(mw,$wn,toprec) 0
setScrollbar $wn
if {$PgAcVar(mw,$wn,updatable)} then {
	$wn.c bind q <Key> "Tables::editText $wn %A %K"
} else {
	$wn.c bind q <Key> {}
}
set PgAcVar(mw,$wn,dirtyrec) 0
$wn.c raise header
catch {$wn.f1.b1 configure -state normal}
setCursor DEFAULT
}


proc recordSizeInScrollbarUnits {wn} {
	# record size in scrollbar units
	global PgAcVar
	return [expr 1.0/$PgAcVar(mw,$wn,nrecs)]
}


proc getVisibleRecordsCount {wn} {
	# number of records that fit in the window at its current size
	expr [winfo height $wn.c]/14
}


proc {setScrollbar} {wn} {
global PgAcVar
	if {$PgAcVar(mw,$wn,nrecs)==0} return;
	# Fixes problem of window resizing messing up the scrollbar size.
	set record_size [recordSizeInScrollbarUnits $wn];
	$wn.sb set [expr $PgAcVar(mw,$wn,toprec)*$record_size] \
	[expr ($PgAcVar(mw,$wn,toprec)+[getVisibleRecordsCount $wn])*$record_size]
}


proc {refreshRecords} {wn} {
global PgAcVar
	set nq $PgAcVar(mw,$wn,query)
	if {($PgAcVar(mw,$wn,isaquery)) && ("$PgAcVar(mw,$wn,filter)$PgAcVar(mw,$wn,sortfield)"!="")} {
		showError [intlmsg "Sorting and filtering not (yet) available from queries!\n\nPlease enter them in the query definition!"]
		set PgAcVar(mw,$wn,sortfield) {}
		set PgAcVar(mw,$wn,filter) {}
	} else {
		if {$PgAcVar(mw,$wn,filter)!=""} {
			set nq "$PgAcVar(mw,$wn,query) where ($PgAcVar(mw,$wn,filter))"
		} else {
			set nq $PgAcVar(mw,$wn,query)
		}
		if {$PgAcVar(mw,$wn,sortfield)!=""} {
			set nq "$nq order by $PgAcVar(mw,$wn,sortfield)"
		}
	}
	if {[insertNewRecord $wn]} {selectRecords $wn $nq}
}


proc {showRecord} {wn row} {
global PgAcVar
	set PgAcVar(mw,$wn,errorsavingnew) 0
	if {$PgAcVar(mw,$wn,newrec_fields)!=""} {
		if {$row!=$PgAcVar(mw,$wn,last_rownum)} {
			if {![insertNewRecord $wn]} {
		set PgAcVar(mw,$wn,errorsavingnew) 1
		return
			}
		}
	}
	set y1 [lindex $PgAcVar(mw,$wn,rowy) $row]
	set y2 [lindex $PgAcVar(mw,$wn,rowy) [expr $row+1]]
	if {$y2==""} {set y2 [expr $y1+14]}
	$wn.c dtag hili hili
	$wn.c addtag hili withtag r$row
	# Making a rectangle arround the record
	set x 3
	foreach wi $PgAcVar(mw,$wn,colwidth) {incr x [expr $wi+2]}
	$wn.c delete crtrec
	$wn.c create rectangle [expr -1-$PgAcVar(mw,$wn,leftoffset)] $y1 [expr $x-$PgAcVar(mw,$wn,leftoffset)] $y2 -fill #EEEEEE -outline {} -tags {q crtrec}
	$wn.c lower crtrec
}


proc {startEdit} {wn id x y} {
global PgAcVar
	if {!$PgAcVar(mw,$wn,updatable)} return
	set PgAcVar(mw,$wn,id_edited) $id
	set PgAcVar(mw,$wn,dirtyrec) 0
	set PgAcVar(mw,$wn,text_initial_value) [$wn.c itemcget $id -text]
	focus $wn.c
	$wn.c focus $id
	$wn.c icursor $id @$x,$y
	if {$PgAcVar(mw,$wn,row_edited)==$PgAcVar(mw,$wn,nrecs)} {
		if {[$wn.c itemcget $id -text]=="*"} {
			$wn.c itemconfigure $id -text ""
			$wn.c icursor $id 0
		}
	}
}


proc {canvasPaste} {wn x y} {
global PgAcVar
	$wn.c insert $PgAcVar(mw,$wn,id_edited) insert [selection get]
	set PgAcVar(mw,$wn,dirtyrec) 1
}

proc {getNewWindowName} {} {
global PgAcVar
	incr PgAcVar(mwcount)
	return .pgaw:$PgAcVar(mwcount)
}



proc {createWindow} {{base ""}} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:$PgAcVar(mwcount)
		set included 0
	} else {
		set included 1
	}
	set wn $base
	set PgAcVar(mw,$wn,dirtyrec) 0
	set PgAcVar(mw,$wn,id_edited) {}
	set PgAcVar(mw,$wn,filter) {}
	set PgAcVar(mw,$wn,sortfield) {}
	if {! $included} {
		if {[winfo exists $base]} {
			wm deiconify $base; return
		}
		toplevel $base -class Toplevel
		wm focusmodel $base passive
		wm geometry $base 650x400
		wm maxsize $base 1280 1024
		wm minsize $base 650 400
		wm overrideredirect $base 0
		wm resizable $base 1 1
		wm deiconify $base
		wm title $base [intlmsg "Table"]
	}
	bind $base <Key-Delete> "Tables::deleteRecord $wn"
	bind $base <Key-F1> "Help::load tables"
	if {! $included} {
		frame $base.f1  -borderwidth 2 -height 75 -relief groove -width 125 
		label $base.f1.l1  -borderwidth 0 -text [intlmsg {Sort field}]
		entry $base.f1.e1  -background #fefefe -borderwidth 1 -width 14  -highlightthickness 1 -textvariable PgAcVar(mw,$wn,sortfield)
		bind $base.f1.e1 <Key-Return> "Tables::refreshRecords $wn"	
		bind $base.f1.e1 <Key-KP_Enter> "Tables::refreshRecords $wn"	
		label $base.f1.lb1  -borderwidth 0 -text {     } 
		label $base.f1.l2  -borderwidth 0 -text [intlmsg {Filter conditions}]
		entry $base.f1.e2  -background #fefefe -borderwidth 1  -highlightthickness 1 -textvariable PgAcVar(mw,$wn,filter)
		bind $base.f1.e2 <Key-Return> "Tables::refreshRecords $wn"	
		bind $base.f1.e2 <Key-KP_Enter> "Tables::refreshRecords $wn"	
		button $base.f1.b1  -borderwidth 1 -text [intlmsg Close] -command "
		if {\[Tables::insertNewRecord $wn\]} {
			$wn.c delete rows
			$wn.c delete header
			Window destroy $wn
			PgAcVar:clean mw,$wn,*
		}"
		button $base.f1.b2  -borderwidth 1 -text [intlmsg Reload] -command "Tables::refreshRecords $wn"
	}
	frame $base.frame20  -borderwidth 2 -height 75 -relief groove -width 125 
	button $base.frame20.01  -borderwidth 1 -text < -command "Tables::panRight $wn"
	label $base.frame20.02  -anchor w -borderwidth 1 -height 1  -relief sunken -text {} -textvariable PgAcVar(mw,$wn,msg) 
	button $base.frame20.03  -borderwidth 1 -text > -command "Tables::panLeft $wn"
	canvas $base.c  -background #fefefe -borderwidth 2 -height 207 -highlightthickness 0  -relief ridge -selectborderwidth 0 -takefocus 1 -width 295 
	scrollbar $base.sb  -borderwidth 1 -orient vert -width 12  -command "Tables::scrollWindow $wn"
	bind $base.c <Button-1> "Tables::canvasClick $wn %x %y"
	bind $base.c <Button-2> "Tables::canvasPaste $wn %x %y"
	bind $base.c <Button-3> "if {[Tables::finishEdit $wn]} \"Tables::insertNewRecord $wn\""

	# Prevent Tab from moving focus out of canvas widget
	bind $base.c <Tab> break

	if {! $included} {
		pack $base.f1  -in $wn -anchor center -expand 0 -fill x -side top 
		pack $base.f1.l1  -in $wn.f1 -anchor center -expand 0 -fill none -side left 
		pack $base.f1.e1  -in $wn.f1 -anchor center -expand 0 -fill none -side left 
		pack $base.f1.lb1  -in $wn.f1 -anchor center -expand 0 -fill none -side left 
		pack $base.f1.l2  -in $wn.f1 -anchor center -expand 0 -fill none -side left 
		pack $base.f1.e2  -in $wn.f1 -anchor center -expand 0 -fill none -side left 
		pack $base.f1.b1  -in $wn.f1 -anchor center -expand 0 -fill none -side right 
		pack $base.f1.b2  -in $wn.f1 -anchor center -expand 0 -fill none -side right 
	}
	pack $base.frame20  -in $wn -anchor s -expand 0 -fill x -side bottom 
	pack $base.frame20.01  -in $wn.frame20 -anchor center -expand 0 -fill none -side left 
	pack $base.frame20.02  -in $wn.frame20 -anchor center -expand 1 -fill x -side left 
	pack $base.frame20.03  -in $wn.frame20 -anchor center -expand 0 -fill none -side right 
	pack $base.c -in $wn -anchor w -expand 1 -fill both -side left 
	pack $base.sb -in $wn -anchor e -expand 0 -fill y -side right
}


proc {renameColumn} {} {
global PgAcVar CurrentDB
	if {[string length [string trim $PgAcVar(tblinfo,new_cn)]]==0} {
		showError [intlmsg "Field name not entered!"]
		return
	}
	set old_name [string trim [string range $PgAcVar(tblinfo,old_cn) 0 31]]
	set PgAcVar(tblinfo,new_cn) [string trim $PgAcVar(tblinfo,new_cn)]
	if {$old_name == $PgAcVar(tblinfo,new_cn)} {
		showError [intlmsg "New name is the same as the old one!"]
		return
	}
	foreach line [.pgaw:TableInfo.f1.lb get 0 end] {
		if {[string trim [string range $line 0 31]]==$PgAcVar(tblinfo,new_cn)} {
			showError [format [intlmsg {Column name '%s' already exists in this table!}] $PgAcVar(tblinfo,new_cn)]
			return
		}
	}
	if {[sql_exec noquiet "alter table \"$PgAcVar(tblinfo,tablename)\" rename column \"$old_name\" to \"$PgAcVar(tblinfo,new_cn)\""]} {
		refreshTableInformation
		Window destroy .pgaw:RenameField
	}
}



proc {addNewIndex} {} {
global PgAcVar
	set iflds [.pgaw:TableInfo.f1.lb curselection]
	if {$iflds==""} {
		showError [intlmsg "You have to select index fields!"]
		return
	}
	set ifldslist {}
	foreach i $iflds {lappend ifldslist "\"[string trim [string range [.pgaw:TableInfo.f1.lb get $i] 0 32]]\""}
	set PgAcVar(addindex,indexname) $PgAcVar(tblinfo,tablename)_[join $ifldslist _]
	# Replace the quotes with underlines
	regsub -all {"} $PgAcVar(addindex,indexname) {_} PgAcVar(addindex,indexname)
	# Replace the double underlines
	while {[regsub -all {__} $PgAcVar(addindex,indexname) {_} PgAcVar(addindex,indexname)]} {}
	# Replace the final underline
	regsub -all {_$} $PgAcVar(addindex,indexname) {} PgAcVar(addindex,indexname)
	set PgAcVar(addindex,indexfields) [join $ifldslist ,]
	Window show .pgaw:AddIndex
	wm transient .pgaw:AddIndex .pgaw:TableInfo
}

proc {deleteIndex} {} {
global PgAcVar
	set sel [.pgaw:TableInfo.f2.fl.ilb curselection]
	if {$sel == ""} {
		showError [intlmsg "You have to select an index!"]
		return
	}
	if {[tk_messageBox -title [intlmsg Warning] -parent .pgaw:TableInfo -message [format [intlmsg "You choose to delete index\n\n %s \n\nProceed?"] [.pgaw:TableInfo.f2.fl.ilb get $sel]] -type yesno -default no]=="no"} {return}
	if {[sql_exec noquiet "drop index \"[.pgaw:TableInfo.f2.fl.ilb get $sel]\""]} {
		refreshTableInformation
	}
}

proc {createNewIndex} {} {
global PgAcVar
	if {$PgAcVar(addindex,indexname)==""} {
		showError [intlmsg "Index name cannot be null!"]
		return
	}
	setCursor CLOCK
	if {[sql_exec noquiet "CREATE $PgAcVar(addindex,unique) INDEX \"$PgAcVar(addindex,indexname)\" on \"$PgAcVar(tblinfo,tablename)\" ($PgAcVar(addindex,indexfields))"]} {
		setCursor DEFAULT
		Window destroy .pgaw:AddIndex
		refreshTableInformation
	}
	setCursor DEFAULT
}


proc {showIndexInformation} {} {
global PgAcVar CurrentDB
set cs [.pgaw:TableInfo.f2.fl.ilb curselection]
if {$cs==""} return
set idxname [.pgaw:TableInfo.f2.fl.ilb get $cs]
wpg_select $CurrentDB "select pg_index.*,pg_class.oid from pg_index,pg_class where pg_class.relname='$idxname' and pg_class.oid=pg_index.indexrelid" rec {
	if {$rec(indisunique)=="t"} {
		set PgAcVar(tblinfo,isunique) [intlmsg Yes]
	} else {
		set PgAcVar(tblinfo,isunique) [intlmsg No]
	}
	if {$rec(indisclustered)=="t"} {
		set PgAcVar(tblinfo,isclustered) [intlmsg Yes]
	} else {
		set PgAcVar(tblinfo,isclustered) [intlmsg No]
	}
	set PgAcVar(tblinfo,indexfields) {}
	.pgaw:TableInfo.f2.fr.lb delete 0 end
	foreach field $rec(indkey) {
		if {$field!=0} {
#            wpg_select $CurrentDB "select attname from pg_attribute where attrelid=$PgAcVar(tblinfo,tableoid) and attnum=$field" rec1 {
#                set PgAcVar(tblinfo,indexfields) "$PgAcVar(tblinfo,indexfields) $rec1(attname)"
#            }
		set PgAcVar(tblinfo,indexfields) "$PgAcVar(tblinfo,indexfields) $PgAcVar(tblinfo,f$field)"
		.pgaw:TableInfo.f2.fr.lb insert end $PgAcVar(tblinfo,f$field)
		}

	}
}
set PgAcVar(tblinfo,indexfields) [string trim $PgAcVar(tblinfo,indexfields)]
}


proc {addNewColumn} {} {
global PgAcVar
	if {$PgAcVar(addfield,name)==""} {
		showError [intlmsg "Empty field name ?"]
		focus .pgaw:AddField.e1
		return
	}		
	if {$PgAcVar(addfield,type)==""} {
		showError [intlmsg "No field type ?"]
		focus .pgaw:AddField.e2
		return
	}
	if {![sql_exec quiet "alter table \"$PgAcVar(tblinfo,tablename)\" add column \"$PgAcVar(addfield,name)\" $PgAcVar(addfield,type)"]} {
		showError "[intlmsg {Cannot add column}]\n\n$PgAcVar(pgsql,errmsg)"
		return
	}
	Window destroy .pgaw:AddField
	sql_exec quiet "update pga_layout set colnames=colnames || ' {$PgAcVar(addfield,name)}', colwidth=colwidth || ' 150',nrcols=nrcols+1 where tablename='$PgAcVar(tblinfo,tablename)'"
	refreshTableInformation
}


proc {newtable:add_new_field} {} {
global PgAcVar
if {$PgAcVar(nt,fieldname)==""} {
	showError [intlmsg "Enter a field name"]
	focus .pgaw:NewTable.e2
	return
}
if {$PgAcVar(nt,fldtype)==""} {
	showError [intlmsg "The field type is not specified!"]
	return
}
if {($PgAcVar(nt,fldtype)=="varchar")&&($PgAcVar(nt,fldsize)=="")} {
	focus .pgaw:NewTable.e3
	showError [intlmsg "You must specify field size!"]
	return
}
if {$PgAcVar(nt,fldsize)==""} then {set sup ""} else {set sup "($PgAcVar(nt,fldsize))"}
if {[regexp $PgAcVar(nt,fldtype) "varchartextdatetime"]} {set supc "'"} else {set supc ""}
# Don't put the ' arround default value if it contains the now() function
if {([regexp $PgAcVar(nt,fldtype) "datetime"]) && ([regexp now $PgAcVar(nt,defaultval)])} {set supc ""}
# Clear the notnull attribute if field type is serial
if {$PgAcVar(nt,fldtype)=="serial"} {set PgAcVar(nt,notnull) " "}
if {$PgAcVar(nt,defaultval)==""} then {set sup2 ""} else {set sup2 " DEFAULT $supc$PgAcVar(nt,defaultval)$supc"}
# Checking for field name collision
set inspos end
for {set i 0} {$i<[.pgaw:NewTable.lb size]} {incr i} {
	set linie [.pgaw:NewTable.lb get $i]
	if {$PgAcVar(nt,fieldname)==[string trim [string range $linie 2 33]]} {
		if {[tk_messageBox -title [intlmsg Warning] -parent .pgaw:NewTable -message [format [intlmsg "There is another field with the same name: '%s'!\n\nReplace it ?"] $PgAcVar(nt,fieldname)] -type yesno -default yes]=="no"} return
		.pgaw:NewTable.lb delete $i
		set inspos $i
		break
	}	 
  }
.pgaw:NewTable.lb insert $inspos [format "%1s %-32.32s %-14s%-16s" $PgAcVar(nt,primarykey) $PgAcVar(nt,fieldname) $PgAcVar(nt,fldtype)$sup $sup2$PgAcVar(nt,notnull)]
focus .pgaw:NewTable.e2
set PgAcVar(nt,fieldname) {}
set PgAcVar(nt,fldsize) {}
set PgAcVar(nt,defaultval) {}
set PgAcVar(nt,primarykey) " "
}

proc {newtable:create} {} {
global PgAcVar CurrentDB
if {$PgAcVar(nt,tablename)==""} then {
	showError [intlmsg "You must supply a name for your table!"]
	focus .pgaw:NewTable.etabn
	return
}
if {([.pgaw:NewTable.lb size]==0) && ($PgAcVar(nt,inherits)=="")} then {
	showError [intlmsg "Your table has no columns!"]
	focus .pgaw:NewTable.e2
	return
}
set fl {}
set pkf {}
foreach line [.pgaw:NewTable.lb get 0 end] {
	set fldname "\"[string trim [string range $line 2 33]]\""
	lappend fl "$fldname [string trim [string range $line 35 end]]"
	if {[string range $line 0 0]=="*"} {
		lappend pkf "$fldname"
	}
}
set temp "create table \"$PgAcVar(nt,tablename)\" ([join $fl ,]"
if {$PgAcVar(nt,constraint)!=""} then {set temp "$temp, constraint \"$PgAcVar(nt,constraint)\""}
if {$PgAcVar(nt,check)!=""} then {set temp "$temp check ($PgAcVar(nt,check))"}
if {[llength $pkf]>0} then {set temp "$temp, primary key([join $pkf ,])"}
set temp "$temp)"
if {$PgAcVar(nt,inherits)!=""} then {set temp "$temp inherits ($PgAcVar(nt,inherits))"}
setCursor CLOCK
if {[sql_exec noquiet $temp]} {
	Window destroy .pgaw:NewTable
	Mainlib::cmd_Tables
}
setCursor DEFAULT
}

proc {tabSelect} {i} {
global PgAcVar
	set base .pgaw:TableInfo
	foreach tab {0 1 2 3} {
		if {$i == $tab} {
			place $base.l$tab -y 13
			place $base.f$tab -x 15 -y 45
			$base.l$tab configure -font $PgAcVar(pref,font_bold)
		} else {
			place $base.l$tab -y 15
			place $base.f$tab -x 15 -y 500
			$base.l$tab configure -font $PgAcVar(pref,font_normal)
		}
	}
	array set coord [place info $base.l$i]
	place $base.lline -x [expr {1+$coord(-x)}]
}


}

####################   END OF NAMESPACE TABLES ####################

proc vTclWindow.pgaw:NewTable {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:NewTable
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 634x392+78+181
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Create new table"]
	bind $base <Key-F1> "Help::load new_table"
	entry $base.etabn \
		-background #fefefe -borderwidth 1 -selectborderwidth 0 \
		-textvariable PgAcVar(nt,tablename) 
	bind $base.etabn <Key-Return> {
		focus .pgaw:NewTable.einh
	}
	label $base.li \
		-anchor w -borderwidth 0 -text [intlmsg Inherits]
	entry $base.einh \
		-background #fefefe -borderwidth 1 -selectborderwidth 0 \
		-textvariable PgAcVar(nt,inherits) 
	bind $base.einh <Key-Return> {
		focus .pgaw:NewTable.e2
	}
	button $base.binh \
		-borderwidth 1 \
		-command {if {[winfo exists .pgaw:NewTable.ddf]} {
	destroy .pgaw:NewTable.ddf
} else {
	create_drop_down .pgaw:NewTable 386 23 220
	focus .pgaw:NewTable.ddf.sb
	foreach tbl [Database::getTablesList] {.pgaw:NewTable.ddf.lb insert end $tbl}
	bind .pgaw:NewTable.ddf.lb <ButtonRelease-1> {
		set i [.pgaw:NewTable.ddf.lb curselection]
		if {$i!=""} {
			if {$PgAcVar(nt,inherits)==""} {
		set PgAcVar(nt,inherits) "\"[.pgaw:NewTable.ddf.lb get $i]\""
			} else {
		set PgAcVar(nt,inherits) "$PgAcVar(nt,inherits),\"[.pgaw:NewTable.ddf.lb get $i]\""
			}
		}
		if {$i!=""} {focus .pgaw:NewTable.e2}
		destroy .pgaw:NewTable.ddf
		break
	}
}} \
		-highlightthickness 0 -takefocus 0 -image dnarw
	entry $base.e2 \
		-background #fefefe -borderwidth 1 -selectborderwidth 0 \
		-textvariable PgAcVar(nt,fieldname) 
	bind $base.e2 <Key-Return> {
		focus .pgaw:NewTable.e1
	}
	entry $base.e1 \
		-background #fefefe -borderwidth 1 -selectborderwidth 0 \
		-textvariable PgAcVar(nt,fldtype) 
	bind $base.e1 <Key-Return> {
		focus .pgaw:NewTable.e5
	}
	entry $base.e3 \
		-background #fefefe -borderwidth 1 -selectborderwidth 0 \
		-textvariable PgAcVar(nt,fldsize) 
	bind $base.e3 <Key-Return> {
		focus .pgaw:NewTable.e5
	}
	entry $base.e5 \
		-background #fefefe -borderwidth 1 -selectborderwidth 0 \
		-textvariable PgAcVar(nt,defaultval) 
	bind $base.e5 <Key-Return> {
		focus .pgaw:NewTable.cb1
	}
	checkbutton $base.cb1 \
		-borderwidth 1 \
		-offvalue { } -onvalue { NOT NULL} -text [intlmsg {field cannot be null}] \
		-variable PgAcVar(nt,notnull) 
	label $base.lab1 \
		-borderwidth 0 -text [intlmsg type]
	label $base.lab2 \
		-borderwidth 0 -anchor w -text [intlmsg {field name}]
	label $base.lab3 \
		-borderwidth 0 -text [intlmsg size]
	label $base.lab4 \
		-borderwidth 0 -anchor w -text [intlmsg {Default value}]
	button $base.addfld \
		-borderwidth 1 -command Tables::newtable:add_new_field \
		-text [intlmsg {Add field}]
	button $base.delfld \
		-borderwidth 1 -command {catch {.pgaw:NewTable.lb delete [.pgaw:NewTable.lb curselection]}} \
		-text [intlmsg {Delete field}]
	button $base.emptb \
		-borderwidth 1 -command {.pgaw:NewTable.lb delete 0 [.pgaw:NewTable.lb size]} \
		-text [intlmsg {Delete all}]
	button $base.maketbl \
		-borderwidth 1 -command Tables::newtable:create \
		-text [intlmsg Create]
	listbox $base.lb \
		-background #fefefe -foreground #000000 -borderwidth 1 \
		-selectbackground #c3c3c3 -font $PgAcVar(pref,font_fix) \
		-selectborderwidth 0 -yscrollcommand {.pgaw:NewTable.sb set} 
	bind $base.lb <ButtonRelease-1> {
		if {[.pgaw:NewTable.lb curselection]!=""} {
	set fldname [string trim [lindex [split [.pgaw:NewTable.lb get [.pgaw:NewTable.lb curselection]]] 0]]
}
	}
	button $base.exitbtn \
		-borderwidth 1 -command {Window destroy .pgaw:NewTable} \
		-text [intlmsg Cancel]
	button $base.helpbtn \
		-borderwidth 1 -command {Help::load new_table} \
		-text [intlmsg Help]
	label $base.l1 \
		-anchor w -borderwidth 1 \
		-relief raised -text "       [intlmsg {field name}]"
	label $base.l2 \
		-borderwidth 1 \
		-relief raised -text [intlmsg type]
	label $base.l3 \
		-borderwidth 1 \
		-relief raised -text [intlmsg options]
	scrollbar $base.sb \
		-borderwidth 1 -command {.pgaw:NewTable.lb yview} -orient vert 
	label $base.l93 \
		-anchor w -borderwidth 0 -text [intlmsg {Table name}]
	button $base.mvup \
		-borderwidth 1 \
		-command {if {[.pgaw:NewTable.lb size]>1} {
	set i [.pgaw:NewTable.lb curselection]
	if {($i!="")&&($i>0)} {
		.pgaw:NewTable.lb insert [expr $i-1] [.pgaw:NewTable.lb get $i]
		.pgaw:NewTable.lb delete [expr $i+1]
		.pgaw:NewTable.lb selection set [expr $i-1]
	}
}} \
		-text [intlmsg {Move up}]
	button $base.mvdn \
		-borderwidth 1 \
		-command {if {[.pgaw:NewTable.lb size]>1} {
	set i [.pgaw:NewTable.lb curselection]
	if {($i!="")&&($i<[expr [.pgaw:NewTable.lb size]-1])} {
		.pgaw:NewTable.lb insert [expr $i+2] [.pgaw:NewTable.lb get $i]
		.pgaw:NewTable.lb delete $i
		.pgaw:NewTable.lb selection set [expr $i+1]
	}
}} \
		-text [intlmsg {Move down}]
	button $base.button17 \
		-borderwidth 1 \
		-command {
if {[winfo exists .pgaw:NewTable.ddf]} {
	destroy .pgaw:NewTable.ddf
} else {
	create_drop_down .pgaw:NewTable 291 80 97
	focus .pgaw:NewTable.ddf.sb
	.pgaw:NewTable.ddf.lb insert end char varchar text int2 int4 serial float4 float8 money abstime date datetime interval reltime time timespan timestamp boolean box circle line lseg path point polygon
	bind .pgaw:NewTable.ddf.lb <ButtonRelease-1> {
		set i [.pgaw:NewTable.ddf.lb curselection]
		if {$i!=""} {set PgAcVar(nt,fldtype) [.pgaw:NewTable.ddf.lb get $i]}
		destroy .pgaw:NewTable.ddf
		if {$i!=""} {
			if {[lsearch {char varchar} $PgAcVar(nt,fldtype)]==-1} {
		set PgAcVar(nt,fldsize) {}
		.pgaw:NewTable.e3 configure -state disabled
		focus .pgaw:NewTable.e5
			} else {
		.pgaw:NewTable.e3 configure -state normal
		focus .pgaw:NewTable.e3
			}
		}
		break
	}
}} \
		-highlightthickness 0 -takefocus 0 -image dnarw 
	label $base.lco \
		-borderwidth 0 -anchor w -text [intlmsg Constraint]
	entry $base.eco \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(nt,constraint) 
	label $base.lch \
		-borderwidth 0 -text [intlmsg check]
	entry $base.ech \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(nt,check) 
	label $base.ll \
		-borderwidth 1 \
		-relief raised 
	checkbutton $base.pk \
		-borderwidth 1 \
		-offvalue { } -onvalue * -text [intlmsg {primary key}] -variable PgAcVar(nt,primarykey) 
	label $base.lpk \
		-borderwidth 1 \
		-relief raised -text K 
	place $base.etabn \
		-x 105 -y 5 -width 136 -height 20 -anchor nw -bordermode ignore 
	place $base.li \
		-x 245 -y 7 -height 16 -anchor nw -bordermode ignore 
	place $base.einh \
		-x 300 -y 5 -width 308 -height 20 -anchor nw -bordermode ignore 
	place $base.binh \
		-x 590 -y 7 -width 16 -height 16 -anchor nw -bordermode ignore 
	place $base.e2 \
		-x 105 -y 60 -width 136 -height 20 -anchor nw -bordermode ignore 
	place $base.e1 \
		-x 291 -y 60 -width 98 -height 20 -anchor nw -bordermode ignore 
	place $base.e3 \
		-x 470 -y 60 -width 46 -height 20 -anchor nw -bordermode ignore 
	place $base.e5 \
		-x 105 -y 82 -width 136 -height 20 -anchor nw -bordermode ignore 
	place $base.cb1 \
		-x 245 -y 83 -height 20 -anchor nw -bordermode ignore 
	place $base.lab1 \
		-x 247 -y 62 -height 16 -anchor nw -bordermode ignore 
	place $base.lab2 \
		-x 4 -y 62 -height 16 -anchor nw -bordermode ignore 
	place $base.lab3 \
		-x 400 -y 62 -height 16 -anchor nw -bordermode ignore 
	place $base.lab4 \
		-x 5 -y 84 -height 16 -anchor nw -bordermode ignore 
	place $base.addfld \
		-x 530 -y 58 -width 100 -height 26 -anchor nw -bordermode ignore 
	place $base.delfld \
		-x 530 -y 190 -width 100 -height 26 -anchor nw -bordermode ignore 
	place $base.emptb \
		-x 530 -y 220 -width 100 -height 26 -anchor nw -bordermode ignore 
	place $base.maketbl \
		-x 530 -y 365 -width 100 -height 26 -anchor nw -bordermode ignore 
	place $base.lb \
		-x 4 -y 121 -width 506 -height 269 -anchor nw -bordermode ignore 
	place $base.helpbtn \
		-x 530 -y 305 -width 100 -height 26 -anchor nw -bordermode ignore 
	place $base.exitbtn \
		-x 530 -y 335 -width 100 -height 26 -anchor nw -bordermode ignore 
	place $base.l1 \
		-x 18 -y 105 -width 195 -height 18 -anchor nw -bordermode ignore 
	place $base.l2 \
		-x 213 -y 105 -width 88 -height 18 -anchor nw -bordermode ignore 
	place $base.l3 \
		-x 301 -y 105 -width 225 -height 18 -anchor nw -bordermode ignore 
	place $base.sb \
		-x 509 -y 121 -width 18 -height 269 -anchor nw -bordermode ignore 
	place $base.l93 \
		-x 4 -y 7 -height 16 -anchor nw -bordermode ignore 
	place $base.mvup \
		-x 530 -y 120 -width 100 -height 26 -anchor nw -bordermode ignore 
	place $base.mvdn \
		-x 530 -y 150 -width 100 -height 26 -anchor nw -bordermode ignore 
	place $base.button17 \
		-x 371 -y 62 -width 16 -height 16 -anchor nw -bordermode ignore 
	place $base.lco \
		-x 5 -y 28 -width 58 -height 16 -anchor nw -bordermode ignore 
	place $base.eco \
		-x 105 -y 27 -width 136 -height 20 -anchor nw -bordermode ignore 
	place $base.lch \
		-x 245 -y 30 -anchor nw -bordermode ignore 
	place $base.ech \
		-x 300 -y 27 -width 308 -height 22 -anchor nw -bordermode ignore 
	place $base.ll \
		-x 5 -y 53 -width 603 -height 2 -anchor nw -bordermode ignore 
	place $base.pk \
		-x 450 -y 83 -height 20 -anchor nw -bordermode ignore 
	place $base.lpk \
		-x 4 -y 105 -width 14 -height 18 -anchor nw -bordermode ignore 
}


proc vTclWindow.pgaw:TableInfo {base} {
global PgAcVar
	if {$base == ""} {
		set base .pgaw:TableInfo
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel \
		-background #c7c3c7 
	wm focusmodel $base passive
	wm geometry $base 522x398+152+135
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Table information"]
	bind $base <Key-F1> "Help::load view_table_structure"
	label $base.l0 \
		-borderwidth 1 -font $PgAcVar(pref,font_bold) \
		-relief raised -text [intlmsg General]
	bind $base.l0 <Button-1> {
		Tables::tabSelect 0
    }
	label $base.l1 \
		-borderwidth 1 \
		-relief raised -text [intlmsg Columns]
	bind $base.l1 <Button-1> {
		Tables::tabSelect 1
    }
	label $base.l2 \
		-borderwidth 1 \
		-relief raised -text [intlmsg Indexes]
	bind $base.l2 <Button-1> {
		Tables::tabSelect 2
    }
	label $base.l3 \
		-borderwidth 1 \
		-relief raised -text [intlmsg Permissions]
	bind $base.l3 <Button-1> {
		Tables::tabSelect 3
    }
	label $base.l \
		-relief raised 
	button $base.btnclose \
		-borderwidth 1 -command {Window destroy .pgaw:TableInfo} \
		-highlightthickness 0 -padx 9 -pady 3 -text [intlmsg Close]
	frame $base.f1 \
		-borderwidth 2 -height 75 -relief groove -width 125 
	frame $base.f1.ft \
		-height 75 -relief groove -width 125 
	label $base.f1.ft.t1 \
		-relief groove -text [intlmsg {field name}]
	label $base.f1.ft.t2 \
		-relief groove -text [intlmsg type] -width 12 
	label $base.f1.ft.t3 \
		-relief groove -text [intlmsg size] -width 6 
	label $base.f1.ft.lnn \
		-relief groove -text [intlmsg {not null}] -width 18 
	label $base.f1.ft.ls \
		-borderwidth 0 \
		-relief raised -text {    } 
	frame $base.f1.fb \
		-height 75 -relief groove -width 125 
	button $base.f1.fb.addcolbtn \
		-borderwidth 1 \
		-command {Window show .pgaw:AddField
			set PgAcVar(addfield,name) {}
			set PgAcVar(addfield,type) {}
			wm transient .pgaw:AddField .pgaw:TableInfo
			focus .pgaw:AddField.e1} \
		 -padx 9 -pady 3 -text [intlmsg {Add new column}]
	button $base.f1.fb.rencolbtn \
		-borderwidth 1 \
		-command {
if {[set PgAcVar(tblinfo,col_id) [.pgaw:TableInfo.f1.lb curselection]]==""} then {
	bell
} else {
	set PgAcVar(tblinfo,old_cn) [.pgaw:TableInfo.f1.lb get [.pgaw:TableInfo.f1.lb curselection]]
	set PgAcVar(tblinfo,new_cn) {}
	Window show .pgaw:RenameField
	tkwait visibility .pgaw:RenameField
	wm transient .pgaw:RenameField .pgaw:TableInfo
	focus .pgaw:RenameField.e1
}
} \
		 -padx 9 -pady 3 -text [intlmsg {Rename column}]
	button $base.f1.fb.addidxbtn \
		-borderwidth 1 -command Tables::addNewIndex \
		 -padx 9 \
		-pady 3 -text [intlmsg {Add new index}]
	listbox $base.f1.lb \
		-background #fefefe -borderwidth 1 -font $PgAcVar(pref,font_fix) \
		-highlightthickness 0 -selectborderwidth 0 \
		-selectmode extended \
		-yscrollcommand {.pgaw:TableInfo.f1.vsb set} 
	scrollbar $base.f1.vsb \
		-borderwidth 1 -command {.pgaw:TableInfo.f1.lb yview} -orient vert -width 14 
	frame $base.f2 \
		-borderwidth 2 -height 75 -relief groove -width 125 
	frame $base.f2.fl \
		-height 75 -relief groove -width 182 
	label $base.f2.fl.t \
		-relief groove -text [intlmsg {Indexes defined}]
	button $base.f2.fl.delidxbtn \
		-borderwidth 1 -command Tables::deleteIndex \
		 -padx 9 \
		-pady 3 -text [intlmsg {Delete index}]
	listbox $base.f2.fl.ilb \
		-background #fefefe -borderwidth 1 \
		-highlightthickness 0 -selectborderwidth 0 -width 37 \
		-yscrollcommand {.pgaw:TableInfo.f2.fl.vsb set} 
	bind $base.f2.fl.ilb <ButtonRelease-1> {
		Tables::showIndexInformation
	}
	scrollbar $base.f2.fl.vsb \
		-borderwidth 1 -command {.pgaw:TableInfo.f2.fl.ilb yview} -orient vert -width 14 
	frame $base.f2.fr \
		-height 75 -relief groove -width 526 
	label $base.f2.fr.t \
		-relief groove -text [intlmsg {index properties}]
	button $base.f2.fr.clusterbtn \
		-borderwidth 1 -command Tables::clusterIndex \
		 -padx 9 -pady 3 -text [intlmsg {Cluster index}]
	frame $base.f2.fr.fp \
		-borderwidth 2 -height 75 -relief groove -width 125 
	label $base.f2.fr.fp.lu \
		-anchor w -borderwidth 0 \
		-relief raised -text [intlmsg {Is unique ?}]
	label $base.f2.fr.fp.vu \
		-borderwidth 0 -textvariable PgAcVar(tblinfo,isunique) \
		-foreground #000096 -relief raised -text {} 
	label $base.f2.fr.fp.lc \
		-borderwidth 0 \
		-relief raised -text [intlmsg {Is clustered ?}]
	label $base.f2.fr.fp.vc -textvariable PgAcVar(tblinfo,isclustered) \
		-borderwidth 0 \
		-foreground #000096 -relief raised -text {}
	label $base.f2.fr.lic \
		-relief groove -text [intlmsg {index columns}]
	listbox $base.f2.fr.lb \
		-background #fefefe -borderwidth 1 \
		-highlightthickness 0 -selectborderwidth 0 \
		-yscrollcommand {.pgaw:TableInfo.f2.fr.vsb set} 
	scrollbar $base.f2.fr.vsb \
		-borderwidth 1 -command {.pgaw:TableInfo.f2.fr.lb yview} -orient vert -width 14 
	frame $base.f3 \
		-borderwidth 2 -height 75 -relief groove -width 125 
	frame $base.f3.ft \
		-height 75 -relief groove -width 125 
	label $base.f3.ft.luser \
		-relief groove -text [intlmsg {User name}]
	label $base.f3.ft.lselect \
		-relief groove -text [intlmsg select] -width 10 
	label $base.f3.ft.lupdate \
		-relief groove -text [intlmsg update] -width 10 
	label $base.f3.ft.linsert \
		-relief groove -text [intlmsg insert] -width 10 
	label $base.f3.ft.lrule \
		-relief groove -text [intlmsg rule] -width 10 
	label $base.f3.ft.ls \
		-borderwidth 0 \
		-relief raised -text {    } 
	frame $base.f3.fb \
		-height 75 -relief groove -width 125 
	button $base.f3.fb.adduserbtn \
		-borderwidth 1 -command Tables::newPermissions \
		 -padx 9 -pady 3 -text [intlmsg {Add user}]
	button $base.f3.fb.chguserbtn -command Tables::loadPermissions \
		-borderwidth 1 -padx 9 -pady 3 -text [intlmsg {Change permissions}]
	listbox $base.f3.plb \
		-background #fefefe -borderwidth 1 -font $PgAcVar(pref,font_fix) \
		-highlightthickness 0 -selectborderwidth 0 \
		-yscrollcommand {.pgaw:TableInfo.f3.vsb set} 
	bind $base.f3.plb <Double-1> Tables::loadPermissions
	scrollbar $base.f3.vsb \
		-borderwidth 1 -command {.pgaw:TableInfo.f3.plb yview} -orient vert -width 14 
	label $base.lline \
		-borderwidth 0 \
		-relief raised -text {   } 
	frame $base.f0 \
		-borderwidth 2 -height 75 -relief groove -width 125 
	frame $base.f0.fi \
		-borderwidth 2 -height 75 -relief groove -width 125 
	label $base.f0.fi.l1 \
		-borderwidth 0 \
		-relief raised -text [intlmsg {Table name}]
	label $base.f0.fi.l2 \
		-anchor w -borderwidth 1 \
		-relief sunken -text {} -textvariable PgAcVar(tblinfo,tablename) \
		-width 200 
	label $base.f0.fi.l3 \
		-borderwidth 0 \
		-relief raised -text [intlmsg {Table OID}]
	label $base.f0.fi.l4 \
		-anchor w -borderwidth 1 \
		-relief sunken -text {} -textvariable PgAcVar(tblinfo,tableoid) \
		-width 200 
	label $base.f0.fi.l5 \
		-borderwidth 0 \
		-relief raised -text [intlmsg Owner]
	label $base.f0.fi.l6 \
		-anchor w -borderwidth 1 \
		-relief sunken -text {} -textvariable PgAcVar(tblinfo,owner) \
		-width 200 
	label $base.f0.fi.l7 \
		-borderwidth 0 \
		-relief raised -text [intlmsg {Owner ID}]
	label $base.f0.fi.l8 \
		-anchor w -borderwidth 1 \
		-relief sunken -text {} -textvariable PgAcVar(tblinfo,ownerid) \
		-width 200 
	label $base.f0.fi.l9 \
		-borderwidth 0 \
		-relief raised -text [intlmsg {Has primary key ?}]
	label $base.f0.fi.l10 \
		-anchor w -borderwidth 1 \
		-relief sunken -text {} \
		-textvariable PgAcVar(tblinfo,hasprimarykey) -width 200 
	label $base.f0.fi.l11 \
		-borderwidth 0 \
		-relief raised -text [intlmsg {Has rules ?}]
	label $base.f0.fi.l12 \
		-anchor w -borderwidth 1 \
		-relief sunken -text {} -textvariable PgAcVar(tblinfo,hasrules) \
		-width 200 
	label $base.f0.fi.last \
		-borderwidth 0 \
		-relief raised -text {         } 
	frame $base.f0.fs \
		-borderwidth 2 -height 75 -relief groove -width 125 
	label $base.f0.fs.l1 \
		-borderwidth 0 \
		-relief raised -text [intlmsg {Number of tuples}]
	label $base.f0.fs.l2 \
		-anchor e -borderwidth 1 \
		-relief sunken -text 0 -textvariable PgAcVar(tblinfo,numtuples) \
		-width 200 
	label $base.f0.fs.l3 \
		-borderwidth 0 \
		-relief raised -text [intlmsg {Number of pages}]
	label $base.f0.fs.l4 \
		-anchor e -borderwidth 1 \
		-relief sunken -text 0 -textvariable PgAcVar(tblinfo,numpages) \
		-width 200 
	label $base.f0.fs.last \
		-borderwidth 0 \
		-relief raised -text { } 
	label $base.f0.lstat \
		-borderwidth 0 -font $PgAcVar(pref,font_bold) -relief raised \
		-text " [intlmsg Statistics] "
	label $base.f0.lid \
		-borderwidth 0 -font $PgAcVar(pref,font_bold) -relief raised \
		-text " [intlmsg Identification] "
	place $base.l0 \
		-x 15 -y 13 -width 96 -height 23 -anchor nw -bordermode ignore 
	place $base.l1 \
		-x 111 -y 15 -width 96 -height 23 -anchor nw -bordermode ignore 
	place $base.l2 \
		-x 207 -y 15 -width 96 -height 23 -anchor nw -bordermode ignore 
	place $base.l3 \
		-x 303 -y 15 -width 96 -height 23 -anchor nw -bordermode ignore 
	place $base.l \
		-x 5 -y 35 -width 511 -height 357 -anchor nw -bordermode ignore 
	place $base.btnclose \
		-x 425 -y 5 -width 91 -height 26 -anchor nw -bordermode ignore 
	place $base.f1 \
		-x 15 -y 500 -width 490 -height 335 -anchor nw -bordermode ignore 
	pack $base.f1.ft \
		-in .pgaw:TableInfo.f1 -anchor center -expand 0 -fill x -side top 
	pack $base.f1.ft.t1 \
		-in .pgaw:TableInfo.f1.ft -anchor center -expand 1 -fill x -side left 
	pack $base.f1.ft.t2 \
		-in .pgaw:TableInfo.f1.ft -anchor center -expand 0 -fill none -side left 
	pack $base.f1.ft.t3 \
		-in .pgaw:TableInfo.f1.ft -anchor center -expand 0 -fill none -side left 
	pack $base.f1.ft.lnn \
		-in .pgaw:TableInfo.f1.ft -anchor center -expand 0 -fill none -side left 
	pack $base.f1.ft.ls \
		-in .pgaw:TableInfo.f1.ft -anchor center -expand 0 -fill none -side top 
	pack $base.f1.fb \
		-in .pgaw:TableInfo.f1 -anchor center -expand 0 -fill x -side bottom 
	grid $base.f1.fb.addcolbtn \
		-in .pgaw:TableInfo.f1.fb -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.f1.fb.rencolbtn \
		-in .pgaw:TableInfo.f1.fb -column 1 -row 0 -columnspan 1 -rowspan 1 
	grid $base.f1.fb.addidxbtn \
		-in .pgaw:TableInfo.f1.fb -column 2 -row 0 -columnspan 1 -rowspan 1 
	pack $base.f1.lb \
		-in .pgaw:TableInfo.f1 -anchor center -expand 1 -fill both -pady 1 -side left 
	pack $base.f1.vsb \
		-in .pgaw:TableInfo.f1 -anchor center -expand 0 -fill y -side right 
	place $base.f2 \
		-x 15 -y 500 -width 490 -height 335 -anchor nw -bordermode ignore 
	pack $base.f2.fl \
		-in .pgaw:TableInfo.f2 -anchor center -expand 0 -fill both -side left 
	pack $base.f2.fl.t \
		-in .pgaw:TableInfo.f2.fl -anchor center -expand 0 -fill x -pady 1 -side top 
	pack $base.f2.fl.delidxbtn \
		-in .pgaw:TableInfo.f2.fl -anchor center -expand 0 -fill none -side bottom 
	pack $base.f2.fl.ilb \
		-in .pgaw:TableInfo.f2.fl -anchor center -expand 1 -fill both -pady 1 -side left 
	pack $base.f2.fl.vsb \
		-in .pgaw:TableInfo.f2.fl -anchor center -expand 0 -fill y -side right 
	pack $base.f2.fr \
		-in .pgaw:TableInfo.f2 -anchor center -expand 1 -fill both -padx 1 -side right 
	pack $base.f2.fr.t \
		-in .pgaw:TableInfo.f2.fr -anchor center -expand 0 -fill x -pady 1 -side top 
	pack $base.f2.fr.clusterbtn \
		-in .pgaw:TableInfo.f2.fr -anchor center -expand 0 -fill none -side bottom 
	pack $base.f2.fr.fp \
		-in .pgaw:TableInfo.f2.fr -anchor center -expand 0 -fill x -pady 1 -side top 
	grid $base.f2.fr.fp.lu \
		-in .pgaw:TableInfo.f2.fr.fp -column 0 -row 0 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f2.fr.fp.vu \
		-in .pgaw:TableInfo.f2.fr.fp -column 1 -row 0 -columnspan 1 -rowspan 1 -padx 5 \
		-sticky w 
	grid $base.f2.fr.fp.lc \
		-in .pgaw:TableInfo.f2.fr.fp -column 0 -row 2 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f2.fr.fp.vc \
		-in .pgaw:TableInfo.f2.fr.fp -column 1 -row 2 -columnspan 1 -rowspan 1 -padx 5 \
		-sticky w 
	pack $base.f2.fr.lic \
		-in .pgaw:TableInfo.f2.fr -anchor center -expand 0 -fill x -side top 
	pack $base.f2.fr.lb \
		-in .pgaw:TableInfo.f2.fr -anchor center -expand 1 -fill both -pady 1 -side left 
	pack $base.f2.fr.vsb \
		-in .pgaw:TableInfo.f2.fr -anchor center -expand 0 -fill y -side right 
	place $base.f3 \
		-x 15 -y 500 -width 490 -height 335 -anchor nw -bordermode ignore 
	pack $base.f3.ft \
		-in .pgaw:TableInfo.f3 -anchor center -expand 0 -fill x -pady 1 -side top 
	pack $base.f3.ft.luser \
		-in .pgaw:TableInfo.f3.ft -anchor center -expand 1 -fill x -side left 
	pack $base.f3.ft.lselect \
		-in .pgaw:TableInfo.f3.ft -anchor center -expand 0 -fill none -side left 
	pack $base.f3.ft.lupdate \
		-in .pgaw:TableInfo.f3.ft -anchor center -expand 0 -fill none -side left 
	pack $base.f3.ft.linsert \
		-in .pgaw:TableInfo.f3.ft -anchor center -expand 0 -fill none -side left 
	pack $base.f3.ft.lrule \
		-in .pgaw:TableInfo.f3.ft -anchor center -expand 0 -fill none -side left 
	pack $base.f3.ft.ls \
		-in .pgaw:TableInfo.f3.ft -anchor center -expand 0 -fill none -side top 
	pack $base.f3.fb \
		-in .pgaw:TableInfo.f3 -anchor center -expand 0 -fill x -side bottom 
	grid $base.f3.fb.adduserbtn \
		-in .pgaw:TableInfo.f3.fb -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.f3.fb.chguserbtn \
		-in .pgaw:TableInfo.f3.fb -column 1 -row 0 -columnspan 1 -rowspan 1 
	pack $base.f3.plb \
		-in .pgaw:TableInfo.f3 -anchor center -expand 1 -fill both -pady 1 -side left 
	pack $base.f3.vsb \
		-in .pgaw:TableInfo.f3 -anchor center -expand 0 -fill y -side right 
	place $base.lline \
		-x 16 -y 32 -width 94 -height 6 -anchor nw -bordermode ignore 
	place $base.f0 \
		-x 15 -y 45 -width 490 -height 335 -anchor nw -bordermode ignore 
	place $base.f0.fi \
		-x 5 -y 15 -width 300 -height 140 -anchor nw -bordermode ignore 
	grid columnconf $base.f0.fi 1 -weight 1
	grid rowconf $base.f0.fi 6 -weight 1
	grid $base.f0.fi.l1 \
		-in .pgaw:TableInfo.f0.fi -column 0 -row 0 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f0.fi.l2 \
		-in .pgaw:TableInfo.f0.fi -column 1 -row 0 -columnspan 1 -rowspan 1 -padx 2 \
		-pady 2 
	grid $base.f0.fi.l3 \
		-in .pgaw:TableInfo.f0.fi -column 0 -row 1 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f0.fi.l4 \
		-in .pgaw:TableInfo.f0.fi -column 1 -row 1 -columnspan 1 -rowspan 1 -padx 2 \
		-pady 2 
	grid $base.f0.fi.l5 \
		-in .pgaw:TableInfo.f0.fi -column 0 -row 2 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f0.fi.l6 \
		-in .pgaw:TableInfo.f0.fi -column 1 -row 2 -columnspan 1 -rowspan 1 -padx 2 \
		-pady 2 
	grid $base.f0.fi.l7 \
		-in .pgaw:TableInfo.f0.fi -column 0 -row 3 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f0.fi.l8 \
		-in .pgaw:TableInfo.f0.fi -column 1 -row 3 -columnspan 1 -rowspan 1 -padx 2 \
		-pady 2 
	grid $base.f0.fi.l9 \
		-in .pgaw:TableInfo.f0.fi -column 0 -row 4 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f0.fi.l10 \
		-in .pgaw:TableInfo.f0.fi -column 1 -row 4 -columnspan 1 -rowspan 1 -padx 2 \
		-pady 2 
	grid $base.f0.fi.l11 \
		-in .pgaw:TableInfo.f0.fi -column 0 -row 5 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f0.fi.l12 \
		-in .pgaw:TableInfo.f0.fi -column 1 -row 5 -columnspan 1 -rowspan 1 -padx 2 \
		-pady 2 
	grid $base.f0.fi.last \
		-in .pgaw:TableInfo.f0.fi -column 0 -row 6 -columnspan 1 -rowspan 1 
	place $base.f0.fs \
		-x 310 -y 15 -width 175 -height 50 -anchor nw -bordermode ignore 
	grid columnconf $base.f0.fs 1 -weight 1
	grid rowconf $base.f0.fs 2 -weight 1
	grid $base.f0.fs.l1 \
		-in .pgaw:TableInfo.f0.fs -column 0 -row 0 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f0.fs.l2 \
		-in .pgaw:TableInfo.f0.fs -column 1 -row 0 -columnspan 1 -rowspan 1 -padx 2 \
		-pady 2 -sticky w 
	grid $base.f0.fs.l3 \
		-in .pgaw:TableInfo.f0.fs -column 0 -row 1 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f0.fs.l4 \
		-in .pgaw:TableInfo.f0.fs -column 1 -row 1 -columnspan 1 -rowspan 1 -padx 2 \
		-pady 2 -sticky w 
	grid $base.f0.fs.last \
		-in .pgaw:TableInfo.f0.fs -column 0 -row 2 -columnspan 1 -rowspan 1 
	place $base.f0.lstat \
		-x 315 -y 5 -height 18 -anchor nw -bordermode ignore 
	place $base.f0.lid \
		-x 10 -y 5 -height 16 -anchor nw -bordermode ignore 
}


proc vTclWindow.pgaw:AddIndex {base} {
	if {$base == ""} {
		set base .pgaw:AddIndex
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 334x203+265+266
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Add new index"]
	frame $base.f \
		-borderwidth 2 -height 75 -relief groove -width 125 
	frame $base.f.fin \
		-height 75 -relief groove -width 125 
	label $base.f.fin.lin \
		-borderwidth 0 -relief raised -text [intlmsg {Index name}]
	entry $base.f.fin.ein \
		-background #fefefe -borderwidth 1 -width 28 -textvariable PgAcVar(addindex,indexname) 
	checkbutton $base.f.cbunique -borderwidth 1 \
		-offvalue { } -onvalue unique -text [intlmsg {Is unique ?}] -variable PgAcVar(addindex,unique)
	label $base.f.ls1 \
		-anchor w -background #dfdbdf -borderwidth 0 -foreground #000086 \
		-justify left -relief raised -textvariable PgAcVar(addindex,indexfields) \
		-wraplength 300 
	label $base.f.lif \
		-borderwidth 0 -relief raised -text "[intlmsg {Index fields}]:"
	label $base.f.ls2 \
		-borderwidth 0 -relief raised -text { } 
	label $base.f.ls3 \
		-borderwidth 0 -relief raised -text { } 
	frame $base.fb \
		-height 75 -relief groove -width 125 
	button $base.fb.btncreate -command Tables::createNewIndex \
		-padx 9 -pady 3 -text [intlmsg Create]
	button $base.fb.btncancel \
		-command {Window destroy .pgaw:AddIndex} -padx 9 -pady 3 -text [intlmsg Cancel]
	pack $base.f \
		-in .pgaw:AddIndex -anchor center -expand 1 -fill both -side top 
	grid $base.f.fin \
		-in .pgaw:AddIndex.f -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.f.fin.lin \
		-in .pgaw:AddIndex.f.fin -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.f.fin.ein \
		-in .pgaw:AddIndex.f.fin -column 1 -row 0 -columnspan 1 -rowspan 1 
	grid $base.f.cbunique \
		-in .pgaw:AddIndex.f -column 0 -row 5 -columnspan 1 -rowspan 1 
	grid $base.f.ls1 \
		-in .pgaw:AddIndex.f -column 0 -row 3 -columnspan 1 -rowspan 1 
	grid $base.f.lif \
		-in .pgaw:AddIndex.f -column 0 -row 2 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f.ls2 \
		-in .pgaw:AddIndex.f -column 0 -row 1 -columnspan 1 -rowspan 1 
	grid $base.f.ls3 \
		-in .pgaw:AddIndex.f -column 0 -row 4 -columnspan 1 -rowspan 1 
	pack $base.fb \
		-in .pgaw:AddIndex -anchor center -expand 0 -fill x -side bottom 
	grid $base.fb.btncreate \
		-in .pgaw:AddIndex.fb -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.fb.btncancel \
		-in .pgaw:AddIndex.fb -column 1 -row 0 -columnspan 1 -rowspan 1 
}


proc vTclWindow.pgaw:AddField {base} {
	if {$base == ""} {
		set base .pgaw:AddField
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 302x114+195+175
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Add new column"]
	label $base.l1 \
		-borderwidth 0 -text [intlmsg {Field name}]
	entry $base.e1 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(addfield,name) 
	bind $base.e1 <Key-KP_Enter> {
		focus .pgaw:AddField.e2
	}
	bind $base.e1 <Key-Return> {
		focus .pgaw:AddField.e2
	}
	label $base.l2 \
		-borderwidth 0 \
		-text [intlmsg {Field type}]
	entry $base.e2 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(addfield,type) 
	bind $base.e2 <Key-KP_Enter> {
		Tables::addNewColumn
	}
	bind $base.e2 <Key-Return> {
		Tables::addNewColumn
	}
	button $base.b1 \
		-borderwidth 1 -command Tables::addNewColumn -text [intlmsg {Add field}]
	button $base.b2 \
		-borderwidth 1 -command {Window destroy .pgaw:AddField} -text [intlmsg Cancel]
	place $base.l1 \
		-x 25 -y 10 -anchor nw -bordermode ignore 
	place $base.e1 \
		-x 98 -y 7 -width 178 -height 22 -anchor nw -bordermode ignore 
	place $base.l2 \
		-x 25 -y 40 -anchor nw -bordermode ignore 
	place $base.e2 \
		-x 98 -y 37 -width 178 -height 22 -anchor nw -bordermode ignore 
	place $base.b1 \
		-x 70 -y 75 -anchor nw -bordermode ignore 
	place $base.b2 \
		-x 160 -y 75 -anchor nw -bordermode ignore 
}


proc vTclWindow.pgaw:RenameField {base} {
	if {$base == ""} {
		set base .pgaw:RenameField
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 215x75+258+213
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Rename column"]
	label $base.l1 \
		-borderwidth 0 -text [intlmsg {New name}]
	entry $base.e1 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(tblinfo,new_cn)
	bind $base.e1 <Key-KP_Enter> "Tables::renameColumn"
	bind $base.e1 <Key-Return> "Tables::renameColumn"
	frame $base.f \
		-height 75 -relief groove -width 147 
	button $base.f.b1 \
		-borderwidth 1 -command Tables::renameColumn -text [intlmsg Rename]
	button $base.f.b2 \
		-borderwidth 1 -command {Window destroy .pgaw:RenameField} -text [intlmsg Cancel]
	label $base.l2 -borderwidth 0 
	grid $base.l1 \
		-in .pgaw:RenameField -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.e1 \
		-in .pgaw:RenameField -column 1 -row 0 -columnspan 1 -rowspan 1 
	grid $base.f \
		-in .pgaw:RenameField -column 0 -row 4 -columnspan 2 -rowspan 1 
	grid $base.f.b1 \
		-in .pgaw:RenameField.f -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.f.b2 \
		-in .pgaw:RenameField.f -column 1 -row 0 -columnspan 1 -rowspan 1 
	grid $base.l2 \
		-in .pgaw:RenameField -column 0 -row 3 -columnspan 1 -rowspan 1 
}

proc vTclWindow.pgaw:Permissions {base} {
	if {$base == ""} {
		set base .pgaw:Permissions
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 273x147+256+266
	wm maxsize $base 1280 1024
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Permissions"]
	frame $base.f1 \
		-height 103 -relief groove -width 125 
	label $base.f1.l \
		-borderwidth 0 -relief raised -text [intlmsg {User name}]
	entry $base.f1.ename -textvariable PgAcVar(permission,username) \
		-background #fefefe -borderwidth 1 
	label $base.f1.l2 \
		-borderwidth 0 -relief raised -text { } 
	label $base.f1.l3 \
		-borderwidth 0 -relief raised -text { } 
	frame $base.f2 \
		-height 75 -relief groove -borderwidth 2 -width 125 
	checkbutton $base.f2.cb1 -borderwidth 1 -padx 4 -pady 4 \
		-text [intlmsg select] -variable PgAcVar(permission,select) 
	checkbutton $base.f2.cb2 -borderwidth 1 -padx 4 -pady 4 \
		-text [intlmsg update] -variable PgAcVar(permission,update)
	checkbutton $base.f2.cb3 -borderwidth 1 -padx 4 -pady 4 \
		-text [intlmsg insert] -variable PgAcVar(permission,insert)
	checkbutton $base.f2.cb4 -borderwidth 1 -padx 4 -pady 4 \
		-text [intlmsg rule] -variable PgAcVar(permission,rule)
	frame $base.fb \
		-height 75 -relief groove -width 125 
	button $base.fb.btnsave -command Tables::savePermissions \
		-padx 9 -pady 3 -text [intlmsg Save]
	button $base.fb.btncancel -command {Window destroy .pgaw:Permissions} \
		-padx 9 -pady 3 -text [intlmsg Cancel]
	pack $base.f1 \
		-in .pgaw:Permissions -anchor center -expand 0 -fill none -side top 
	grid $base.f1.l \
		-in .pgaw:Permissions.f1 -column 0 -row 1 -columnspan 1 -rowspan 1 
	grid $base.f1.ename \
		-in .pgaw:Permissions.f1 -column 1 -row 1 -columnspan 1 -rowspan 1 -padx 2 
	grid $base.f1.l2 \
		-in .pgaw:Permissions.f1 -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.f1.l3 \
		-in .pgaw:Permissions.f1 -column 0 -row 2 -columnspan 1 -rowspan 1 
	pack $base.f2 \
		-in .pgaw:Permissions -anchor center -expand 0 -fill none -side top 
	grid $base.f2.cb1 \
		-in .pgaw:Permissions.f2 -column 0 -row 1 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f2.cb2 \
		-in .pgaw:Permissions.f2 -column 1 -row 1 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f2.cb3 \
		-in .pgaw:Permissions.f2 -column 0 -row 2 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f2.cb4 \
		-in .pgaw:Permissions.f2 -column 1 -row 2 -columnspan 1 -rowspan 1 -sticky w 
	pack $base.fb \
		-in .pgaw:Permissions -anchor center -expand 0 -fill none -pady 3 -side bottom 
	grid $base.fb.btnsave \
		-in .pgaw:Permissions.fb -column 0 -row 0 -columnspan 1 -rowspan 1 
	grid $base.fb.btncancel \
		-in .pgaw:Permissions.fb -column 1 -row 0 -columnspan 1 -rowspan 1 
}
