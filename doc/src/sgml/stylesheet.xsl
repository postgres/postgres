<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version='1.0'
                xmlns="http://www.w3.org/TR/xhtml1/transitional"
                exclude-result-prefixes="#default">

<xsl:import href="http://docbook.sourceforge.net/release/xsl/snapshot/html/chunk.xsl"/>

<!-- Parameters -->

<xsl:param name="autotoc.label.separator" select="'. '"/>
<xsl:param name="callout.graphics" select="'0'"></xsl:param>
<xsl:param name="toc.section.depth">3</xsl:param>
<xsl:param name="linenumbering.extension" select="'0'"></xsl:param>
<xsl:param name="preface.autolabel" select="1"></xsl:param>
<xsl:param name="section.autolabel" select="1"></xsl:param>
<xsl:param name="section.label.includes.component.label" select="1"></xsl:param>
<xsl:param name="html.stylesheet" select="'stylesheet.css'"></xsl:param>
<xsl:param name="use.id.as.filename" select="'1'"></xsl:param>
<xsl:param name="make.valid.html" select="1"></xsl:param>
<xsl:param name="generate.id.attributes" select="0"></xsl:param> <!-- ? -->
<xsl:param name="generate.legalnotice.link" select="1"></xsl:param>
<xsl:param name="link.mailto.url">pgsql-docs@postgresql.org</xsl:param>
<xsl:param name="formal.procedures" select="0"></xsl:param>
<xsl:param name="punct.honorific" select="''"></xsl:param>
<xsl:param name="chunker.output.doctype-public" select="''"/> <!-- ? -->
<xsl:param name="chunker.output.indent" select="'no'"/> <!-- ? --> 
<xsl:param name="chunker.output.omit-xml-declaration" select="'no'"/>
<xsl:param name="html.extra.head.links" select="0"></xsl:param>
<xsl:param name="chunk.quietly" select="0"></xsl:param>

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

</xsl:stylesheet>
