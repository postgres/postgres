<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version="1.0">

<!--
  This file contains XSLT stylesheet customizations that are common to
  all output formats (HTML, HTML Help, XSL-FO, etc.).
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
<xsl:param name="toc.section.depth">2</xsl:param>
<xsl:param name="linenumbering.extension" select="'0'"></xsl:param>
<xsl:param name="section.autolabel" select="1"></xsl:param>
<xsl:param name="section.label.includes.component.label" select="1"></xsl:param>
<xsl:param name="refentry.xref.manvolnum" select="0"/>
<xsl:param name="formal.procedures" select="0"></xsl:param>
<xsl:param name="punct.honorific" select="''"></xsl:param>
<xsl:param name="variablelist.term.break.after">1</xsl:param>
<xsl:param name="variablelist.term.separator"></xsl:param>


<!-- Change display of some elements -->

<xsl:template match="productname">
  <xsl:call-template name="inline.charseq"/>
</xsl:template>

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
  ?<xsl:call-template name="inline.charseq"/>?
</xsl:template>

</xsl:stylesheet>
