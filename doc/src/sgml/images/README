Images
======

This directory contains images for use in the documentation.

Creating an image
-----------------

A variety of tools can be used to create an image.  The appropriate
choice depends on the nature of the image.  We prefer workflows that
involve diffable source files.

These tools are acceptable:

- Graphviz (https://graphviz.org/)
- Ditaa (http://ditaa.sourceforge.net/)

We use SVG as the format for integrating the image into the ultimate
output formats of the documentation, that is, HTML, PDF, and others.
Therefore, any tool used needs to be able to produce SVG.

This directory contains makefile rules to build SVG from common input
formats, using some common styling.

fixup-svg.xsl applies some postprocessing to the SVG files produced by
those external tools to address assorted issues.  See comments in
there, and adjust and expand as necessary.

Both the source and the SVG output file are committed in this
directory.  That way, we don't need all developers to have all the
tools installed.  While we accept that there could be some gratuitous
diffs in the SVG output depending the specific tool, let's keep an eye
on that and keep it to a minimum.

Using an image in DocBook
-------------------------

Here is an example for using an image in DocBook:

    <figure id="gin-internals-figure">
     <title>GIN Internals</title>
     <mediaobject>
      <imageobject>
       <imagedata fileref="images/gin.svg" format="SVG" width="100%"/>
      </imageobject>
     </mediaobject>
    </figure>

Notes:

- The real action is in the <mediaobject> element, but typically a
  <figure> should be wrapped around it and an <xref> to the figure
  should be put into the text somewhere.  Don't just put an image into
  the documentation without a link to it and an explanation of it.

- Things are set up so that we only need one <imagedata> element, even
  with different output formats.

- The attribute format="SVG" is required.  If you omit it, it will
  still appear to work, but the stylesheets do a better job if the
  image is declared as SVG explicitly.

- The width should be set to something.  This ensures that the image
  is scaled to fit the page in PDF output.  (Other widths than 100%
  might be appropriate.)
