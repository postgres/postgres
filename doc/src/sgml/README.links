<!-- doc/src/sgml/README.links -->

Linking within DocBook documents can be confusing, so here is a summary:


Intra-document Linking
----------------------

<xref>
	use to get chapter/section number from the title of the target
	link, or xreflabel if defined at the target, or refentrytitle if target
        is a refentry;  has no close tag
	http://www.oasis-open.org/docbook/documentation/reference/html/xref.html

linkend=
	controls the target of the link/xref, required

endterm=
	for <xref>, allows the text of the link/xref to be taken from a
	different link target title

<link>
	use to supply text for the link, only uses linkend, requires </link>
	http://www.oasis-open.org/docbook/documentation/reference/html/link.html
	can be embedded inside of <command>, unlike <xref>


External Linking
----------------

<ulink>
	like <link>, but uses a URL (not a document target);  requires
	</ulink>; if no text is specified, the URL appears as the link
	text
	http://www.oasis-open.org/docbook/documentation/reference/html/ulink.html

url=
	used by <ulink> to specify the URL, required


Guidelines
----------

- For an internal link, if you want to supply text, use <link>, else
  <xref>.

- Specific nouns like GUC variables, SQL commands, and contrib modules
  usually have xreflabels.

- For an external link, use <ulink>, with or without link text.

- xreflabels added to tags prevent the chapter/section for id's from being
  referenced;  only the xreflabel is accessible.  Therefore, use xreflabels
  only when linking is common, and chapter/section information is unneeded.
