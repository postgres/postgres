namespace eval Sequences {

proc {new} {} {
global PgAcVar
	Window show .pgaw:Sequence
	set PgAcVar(seq,name) {}
	set PgAcVar(seq,incr) 1
	set PgAcVar(seq,start) 1
	set PgAcVar(seq,minval) 1
	set PgAcVar(seq,maxval) 2147483647
	focus .pgaw:Sequence.f1.e1
}

proc {open} {seqname} {
global PgAcVar CurrentDB
Window show .pgaw:Sequence
set flag 1
wpg_select $CurrentDB "select * from \"$seqname\"" rec {
	set flag 0
	set PgAcVar(seq,name) $seqname
	set PgAcVar(seq,incr) $rec(increment_by)
	set PgAcVar(seq,start) $rec(last_value)
	.pgaw:Sequence.f1.l3 configure -text [intlmsg "Last value"]
	set PgAcVar(seq,minval) $rec(min_value)
	set PgAcVar(seq,maxval) $rec(max_value)
	.pgaw:Sequence.fb.btnsave configure -state disabled
}
if {$flag} {
	showError [format [intlmsg "Sequence '%s' not found!"] $seqname]
} else {
	for {set i 1} {$i<6} {incr i} {
		.pgaw:Sequence.f1.e$i configure -state disabled
	}
	focus .pgaw:Sequence.fb.btncancel
}
}

proc {save} {} {
global PgAcVar
	if {$PgAcVar(seq,name)==""} {
		showError [intlmsg "You should supply a name for this sequence"]
	} else {
		set s1 {};set s2 {};set s3 {};set s4 {};
		if {$PgAcVar(seq,incr)!=""} {set s1 "increment $PgAcVar(seq,incr)"};
		if {$PgAcVar(seq,start)!=""} {set s2 "start $PgAcVar(seq,start)"};
		if {$PgAcVar(seq,minval)!=""} {set s3 "minvalue $PgAcVar(seq,minval)"};
		if {$PgAcVar(seq,maxval)!=""} {set s4 "maxvalue $PgAcVar(seq,maxval)"};
		set sqlcmd "create sequence \"$PgAcVar(seq,name)\" $s1 $s2 $s3 $s4"
		if {[sql_exec noquiet $sqlcmd]} {
			Mainlib::cmd_Sequences
			tk_messageBox -title [intlmsg Information] -parent .pgaw:Sequence -message [intlmsg "Sequence created!"]
		}
	}
}

}

proc vTclWindow.pgaw:Sequence {base} {
	if {$base == ""} {
		set base .pgaw:Sequence
	}
	if {[winfo exists $base]} {
		wm deiconify $base; return
	}
	toplevel $base -class Toplevel
	wm focusmodel $base passive
	wm geometry $base 283x172+119+210
	wm maxsize $base 1009 738
	wm minsize $base 1 1
	wm overrideredirect $base 0
	wm resizable $base 0 0
	wm deiconify $base
	wm title $base [intlmsg "Sequence"]
	bind $base <Key-F1> "Help::load sequences"
	frame $base.f1 \
		-borderwidth 2 -height 75 -width 125 
	label $base.f1.l1 \
		-borderwidth 0 -relief raised -text [intlmsg {Sequence name}]
	entry $base.f1.e1 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(seq,name) -width 200 
	bind $base.f1.e1 <Key-KP_Enter> {
		focus .pgaw:Sequence.f1.e2
	}
	bind $base.f1.e1 <Key-Return> {
		focus .pgaw:Sequence.f1.e2
	}
	label $base.f1.l2 \
		-borderwidth 0 -relief raised -text [intlmsg Increment]
	entry $base.f1.e2 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(seq,incr) -width 200 
	bind $base.f1.e2 <Key-Return> {
		focus .pgaw:Sequence.f1.e3
	}
	label $base.f1.l3 \
		-borderwidth 0 -relief raised -text [intlmsg {Start value}]
	entry $base.f1.e3 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(seq,start) -width 200 
	bind $base.f1.e3 <Key-Return> {
		focus .pgaw:Sequence.f1.e4
	}
	label $base.f1.l4 \
		-borderwidth 0 -relief raised -text [intlmsg Minvalue]
	entry $base.f1.e4 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(seq,minval) \
		-width 200 
	bind $base.f1.e4 <Key-Return> {
		focus .pgaw:Sequence.f1.e5
	}
	label $base.f1.ls2 \
		-borderwidth 0 -relief raised -text { } 
	label $base.f1.l5 \
		-borderwidth 0 -relief raised -text [intlmsg Maxvalue]
	entry $base.f1.e5 \
		-background #fefefe -borderwidth 1 -textvariable PgAcVar(seq,maxval) \
		-width 200 
	bind $base.f1.e5 <Key-Return> {
		focus .pgaw:Sequence.fb.btnsave
	}
	frame $base.fb \
		-height 75 -relief groove -width 125 
	button $base.fb.btnsave \
		-borderwidth 1 -command Sequences::save \
		-padx 9 -pady 3 -text [intlmsg {Define sequence}]
	button $base.fb.btncancel \
		-borderwidth 1 -command {Window destroy .pgaw:Sequence} \
		-padx 9 -pady 3 -text [intlmsg Close]
	place $base.f1 \
		-x 9 -y 5 -width 265 -height 126 -anchor nw -bordermode ignore 
	grid columnconf $base.f1 2 -weight 1
	grid $base.f1.l1 \
		-in .pgaw:Sequence.f1 -column 0 -row 0 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f1.e1 \
		-in .pgaw:Sequence.f1 -column 2 -row 0 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.f1.l2 \
		-in .pgaw:Sequence.f1 -column 0 -row 2 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f1.e2 \
		-in .pgaw:Sequence.f1 -column 2 -row 2 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.f1.l3 \
		-in .pgaw:Sequence.f1 -column 0 -row 4 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f1.e3 \
		-in .pgaw:Sequence.f1 -column 2 -row 4 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.f1.l4 \
		-in .pgaw:Sequence.f1 -column 0 -row 6 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f1.e4 \
		-in .pgaw:Sequence.f1 -column 2 -row 6 -columnspan 1 -rowspan 1 -pady 2 
	grid $base.f1.ls2 \
		-in .pgaw:Sequence.f1 -column 1 -row 0 -columnspan 1 -rowspan 1 
	grid $base.f1.l5 \
		-in .pgaw:Sequence.f1 -column 0 -row 7 -columnspan 1 -rowspan 1 -sticky w 
	grid $base.f1.e5 \
		-in .pgaw:Sequence.f1 -column 2 -row 7 -columnspan 1 -rowspan 1 -pady 2 
	place $base.fb \
		-x 0 -y 135 -width 283 -height 40 -anchor nw -bordermode ignore 
	grid $base.fb.btnsave \
		-in .pgaw:Sequence.fb -column 0 -row 0 -columnspan 1 -rowspan 1 -padx 5 
	grid $base.fb.btncancel \
		-in .pgaw:Sequence.fb -column 1 -row 0 -columnspan 1 -rowspan 1 -padx 5 
}

