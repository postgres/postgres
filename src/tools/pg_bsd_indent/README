src/tools/pg_bsd_indent/README

This is a lightly modified version of the "indent" program maintained
by the FreeBSD project.  The modifications are mostly to make it portable
to non-BSD-ish platforms, though we do have one formatting switch we
couldn't convince upstream to take.

To build it, configure the surrounding Postgres source tree,
then run "make" in this directory.
Optionally, run "make test" for some simple tests.

You'll need to install pg_bsd_indent somewhere in your PATH before
using it.  Most likely, if you're a developer, you don't want to
put it in the same place as where the surrounding Postgres build
gets installed.  Therefore, do this part with something like

	make install prefix=/usr/local

If you are using Meson to build, the standard build targets will
build pg_bsd_indent and also test it, but there is not currently
provision for installing it anywhere.  Manually copy the built
executable from build/src/tools/pg_bsd_indent/pg_bsd_indent to
wherever you want to put it.


If you happen to be hacking upon the indent source code, the closest
approximation to the existing indentation style seems to be

	./pg_bsd_indent -i4 -l79 -di12 -nfc1 -nlp -sac somefile.c

although this has by no means been rigorously adhered to.
(What was that saw about the shoemaker's children?)
We're not planning to re-indent to Postgres style, because that
would make it difficult to compare to the FreeBSD sources.

----------

The FreeBSD originals of the files in this directory bear the
"4-clause" version of the BSD license.  We have removed the
"advertising" clauses, as per UC Berkeley's directive here:
ftp://ftp.cs.berkeley.edu/pub/4bsd/README.Impt.License.Change
which reads:

July 22, 1999

To All Licensees, Distributors of Any Version of BSD:

As you know, certain of the Berkeley Software Distribution ("BSD") source
code files require that further distributions of products containing all or
portions of the software, acknowledge within their advertising materials
that such products contain software developed by UC Berkeley and its
contributors.

Specifically, the provision reads:

"     * 3. All advertising materials mentioning features or use of this software
      *    must display the following acknowledgement:
      *    This product includes software developed by the University of
      *    California, Berkeley and its contributors."

Effective immediately, licensees and distributors are no longer required to
include the acknowledgement within advertising materials.  Accordingly, the
foregoing paragraph of those BSD Unix files containing it is hereby deleted
in its entirety.

William Hoskins
Director, Office of Technology Licensing
University of California, Berkeley

----------

What follows is the README file as maintained by FreeBSD indent.

----------

  $FreeBSD: head/usr.bin/indent/README 105244 2002-10-16 13:58:39Z charnier $

This is the C indenter, it originally came from the University of Illinois
via some distribution tape for PDP-11 Unix.  It has subsequently been
hacked upon by James Gosling @ CMU.  It isn't very pretty, and really needs
to be completely redone, but it is probably the nicest C pretty printer
around.

Further additions to provide "Kernel Normal Form" were contributed
by the folks at Sun Microsystems.

++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
> From mnetor!yunexus!oz@uunet.UU.NET Wed Mar  9 15:30:55 1988
> Date: Tue, 8 Mar 88 18:36:25 EST
> From: yunexus!oz@uunet.UU.NET (Ozan Yigit)
> To: bostic@okeeffe.berkeley.edu
> Cc: ccvaxa!willcox@uunet.UU.NET, jag@sun.com, rsalz@uunet.UU.NET
> In-Reply-To: Keith Bostic's message of Tue, 16 Feb 88 16:09:06 PST 
> Subject: Re: Indent...

Thank you for your response about indent. I was wrong in my original
observation (or mis-observation :-). UCB did keep the Illinois
copyright intact.

The issue still is whether we can distribute indent, and if we can, which
version. David Willcox (the author) states that:

| Several people have asked me on what basis I claim that indent is in
| the public domain.  I knew I would be sorry I made that posting.
| 
| Some history.  Way back in 1976, the project I worked on at the
| University of Illinois Center for Advanced Computation had a huge
| battle about how to format C code.  After about a week of fighting, I
| got disgusted and wrote a program, which I called indent, to reformat C
| code.  It had a bunch of different options that would let you format
| the output the way you liked.  In particular, all of the different
| formats being championed were supported.
| 
| It was my first big C program.  It was ugly.  It wasn't designed, it
| just sort of grew.  But it pretty much worked, and it stopped most of
| the fighting.
| 
| As a matter of form, I included a University of Illinois Copyright
| notice.  However, my understanding was that, since the work was done
| on an ARPA contract, it was in the public domain.
| 
| Time passed.  Some years later, indent showed up on one of the early
| emacs distributions.
| 
| Later still, someone from UC Berkeley called the UofI and asked if
| indent was in the public domain.  They wanted to include it in their
| UNIX distributions, along with the emacs stuff.  I was no longer at the
| UofI, but Rob Kolstad, who was, asked me about it.  I told him I didn't
| care if they used it, and since then it has been on the BSD distributions.
| 
| Somewhere along the way, several other unnamed people have had their
| hands in it.  It was converted to understand version 7 C.  (The
| original was version 6.)  It was converted from its original filter
| interface to its current "blow away the user's file" interface.
| The $HOME/.indent.pro file parsing was added.  Some more formatting
| options were added.
| 
| The source I have right now has two copyright notices.  One is the
| original from the UofI.  One is from Berkeley.
| 
| I am not a lawyer, and I certainly do not understand copyright law.  As
| far as I am concerned, the bulk of this program, everything covered by
| the UofI copyright, is in the public domain, and worth every penny.
| Berkeley's copyright probably should only cover their changes, and I
| don't know their feelings about sending it out.  

In any case, there appears to be none at UofI to clarify/and change
that copyright, but I am confident (based on the statements of its
author) that the code, as it stands with its copyright, is
distributable, and will not cause any legal problems.

Hence, the issue reduces to *which* one to distribute through
comp.sources.unix. I would suggest that with the permission of you
folks (given that you have parts copyrighted), we distribute the 4.3
version of indent, which appears to be the most up-to-date version. I
happen to have just about every known version of indent, including the
very original submission from the author to a unix tape, later the
G-Emacs version, any 4.n version, sun version and the Unipress
version.  I still think we should not have to "go-back-in-time" and
re-do all the work you people have done.

I hope to hear from you as to what you think about this. You may of
course send 4.3 version to the moderator directly, or you can let me
know of your permission, and I will send the sources, or you can let
me know that 4.3 version is off-limits, in which case we would probably
have to revert to an older version. One way or another, I hope to get
a version of indent to comp.sources.unix.

regards..	oz

cc: ccvaxa!willcox
    sun.com!jar
    uunet!rsalz

