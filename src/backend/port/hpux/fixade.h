/*-------------------------------------------------------------------------
 *
 * fixade.h--
 *    compiler tricks to make things work while POSTGRES does non-native
 *    dereferences on PA-RISC.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fixade.h,v 1.1.1.1 1996/07/09 06:21:43 scrappy Exp $
 *
 *   NOTES
 *	This must be included in EVERY source file.
 *
 *-------------------------------------------------------------------------
 */
#ifndef	FIXADE_H
#define FIXADE_H

#if !defined(NOFIXADE)

#if defined(HP_S500_ALIGN)
/* ----------------
 *	This cheesy hack turns ON unaligned-access fixup on H-P PA-RISC;
 *	the resulting object files contain code that explicitly handles
 *	realignment on reference, so it slows memory access down by a 
 *	considerable factor.  It must be used in conjunction with the +u 
 *	flag to cc.  The #pragma is included in c.h to be safe since EVERY 
 *	source file that performs unaligned access must contain the #pragma.
 * ----------------
 */
#pragma HP_ALIGN HPUX_NATURAL_S500

#if defined(BROKEN_STRUCT_INIT)
/* ----------------
 *	This is so bogus.  The HP-UX 9.01 compiler has totally broken
 *	struct initialization code.  It actually length-checks ALL 
 *	array initializations within structs against the FIRST one that 
 *	it sees (when #pragma HP_ALIGN HPUX_NATURAL_S500 is defined).. 
 *	we have to throw in this unused structure before struct varlena
 *	is defined.
 *
 *	XXX guess you don't need the #pragma anymore after all :-)
 *	since no one looks at this except me i think i'll just leave
 *	this here for now..
 * ----------------
 */
struct HP_WAY_BOGUS {
	char	hpwb_bogus[8192];
};
struct HP_TOO_BOGUS {
	int	hptb_bogus[8192];
};
#endif /* BROKEN_STRUCT_INIT */
#endif /* HP_S500_ALIGN */

#if defined(WEAK_C_OPTIMIZER)
#pragma OPT_LEVEL 1
#endif /* WEAK_C_OPTIMIZER */

#endif /* !NOFIXADE */

#endif /* FIXADE_H */
