<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version='1.0'
                xmlns="http://www.w3.org/TR/xhtml1/transitional"
                exclude-result-prefixes="#default">

<xsl:import href="http://docbook.sourceforge.net/release/xsl/current/xhtml/chunk.xsl"/>

<!-- Parameters -->

<xsl:param name="pg.fast" select="'0'"/>

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
<xsl:param name="generate.index" select="1 - $pg.fast"></xsl:param>
<xsl:param name="preface.autolabel" select="1 - $pg.fast"></xsl:param>
<xsl:param name="section.autolabel" select="1 - $pg.fast"></xsl:param>
<xsl:param name="section.label.includes.component.label" select="1 - $pg.fast"></xsl:param>
<xsl:param name="html.stylesheet" select="'stylesheet.css'"></xsl:param>
<xsl:param name="use.id.as.filename" select="'1'"></xsl:param>
<xsl:param name="make.valid.html" select="1"></xsl:param>
<xsl:param name="generate.id.attributes" select="1"></xsl:param>
<xsl:param name="generate.legalnotice.link" select="1"></xsl:param>
<xsl:param name="refentry.xref.manvolnum" select="0"/>
<xsl:param name="link.mailto.url">pgsql-docs@postgresql.org</xsl:param>
<xsl:param name="formal.procedures" select="0"></xsl:param>
<xsl:param name="punct.honorific" select="''"></xsl:param>
<xsl:param name="chunker.output.indent" select="'yes'"/>
<xsl:param name="chunk.quietly" select="1"></xsl:param>


<!-- Change display of some elements -->

<xsl:template match="command">
  <xsl:call-template name="inline.monoseq"/>
</xsl:template>

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


<!--
  Format multiple terms in varlistentry vertically, instead
  of comma-separated.
 -->

<xsl:template match="varlistentry/term[position()!=last()]">
  <span class="term">
    <xsl:call-template name="anchor"/>
    <xsl:apply-templates/>
  </span><br/>
</xsl:template>

</xsl:stylesheet>
