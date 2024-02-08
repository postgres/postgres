<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version="1.0">

<!--
  This file contains XSLT stylesheet customizations that are common to
  all output formats (HTML, XSL-FO, man, etc.).
  -->

<xsl:include href="stylesheet-speedup-common.xsl" />

<!-- Parameters -->

<!--
<xsl:param name="draft.mode">
  <xsl:choose>
    <xsl:when test="contains($pg.version, 'devel')">yes</xsl:when>
    <xsl:otherwise>no</xsl:otherwise>
  </xsl:choose>
</xsl:param>
-->

<xsl:param name="show.comments">
  <xsl:choose>
    <xsl:when test="contains($pg.version, 'devel')">1</xsl:when>
    <xsl:otherwise>0</xsl:otherwise>
  </xsl:choose>
</xsl:param>

<xsl:param name="callout.graphics" select="'0'"></xsl:param>
<xsl:param name="glossentry.show.acronym">yes</xsl:param>
<xsl:param name="toc.section.depth">2</xsl:param>
<xsl:param name="linenumbering.extension" select="'0'"></xsl:param>
<xsl:param name="section.autolabel" select="1"></xsl:param>
<xsl:param name="section.label.includes.component.label" select="1"></xsl:param>
<xsl:param name="refentry.generate.name" select="0"></xsl:param>
<xsl:param name="refentry.generate.title" select="1"></xsl:param>
<xsl:param name="refentry.xref.manvolnum" select="0"/>
<xsl:param name="formal.procedures" select="0"></xsl:param>
<xsl:param name="generate.consistent.ids" select="1"/>
<xsl:param name="punct.honorific" select="''"></xsl:param>
<xsl:param name="variablelist.term.break.after">1</xsl:param>
<xsl:param name="variablelist.term.separator"></xsl:param>
<xsl:param name="xref.with.number.and.title" select="0"></xsl:param>

<!--
  This is the default setting, but putting it here makes sure the variable
  exists even with older (<1.79) stylesheet versions, because it is used in
  our customization layer.
-->
<xsl:param name="autolink.index.see" select="1"/>


<!-- Change display of some elements -->

<xsl:template match="productname">
  <xsl:call-template name="inline.charseq"/>
</xsl:template>

<!-- Render <returnvalue> with a right arrow then the type name -->
<!-- Avoid adding unnecessary white space in this template! -->
<xsl:template match="returnvalue">&#x2192; <xsl:call-template name="inline.monoseq"/></xsl:template>

<xsl:template match="structfield">
  <xsl:call-template name="inline.monoseq"/>
</xsl:template>

<xsl:template match="structname">
  <xsl:call-template name="inline.monoseq"/>
</xsl:template>

<xsl:template match="symbol">
  <xsl:call-template name="inline.monoseq"/>
</xsl:template>

<xsl:template match="systemitem">
  <xsl:call-template name="inline.charseq"/>
</xsl:template>

<xsl:template match="token">
  <xsl:call-template name="inline.monoseq"/>
</xsl:template>

<xsl:template match="type">
  <xsl:call-template name="inline.monoseq"/>
</xsl:template>

<xsl:template match="programlisting/emphasis">
  <xsl:call-template name="inline.boldseq"/>
</xsl:template>


<!-- Special support for Tcl synopses -->

<xsl:template match="optional[@role='tcl']">
  <xsl:text>?</xsl:text>
  <xsl:call-template name="inline.charseq"/>
  <xsl:text>?</xsl:text>
</xsl:template>


<!-- Support for generating xref link text to additional elements -->

<xsl:template match="command" mode="xref-to">
  <xsl:apply-templates select="." mode="xref"/>
</xsl:template>

<xsl:template match="function" mode="xref-to">
  <xsl:apply-templates select="." mode="xref"/>
</xsl:template>


<!--
  Support for copying images to the output directory, so the output is self
  contained.
-->
<xsl:template name="write-image">
 <xsl:variable name="input_filename">
   <xsl:value-of select="imagedata/@fileref"/>
 </xsl:variable>

 <!-- references images directly, without images/ -->
 <xsl:variable name="output_filename">
   <xsl:value-of select="concat($chunk.base.dir, substring-after($input_filename, '/'))"/>
 </xsl:variable>

 <xsl:call-template name="write.chunk">
  <xsl:with-param name="filename" select="$output_filename"/>
  <xsl:with-param name="content" select="document($input_filename)"/>
 </xsl:call-template>
</xsl:template>


</xsl:stylesheet>
