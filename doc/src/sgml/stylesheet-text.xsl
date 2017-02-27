<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version='1.0'
                xmlns="http://www.w3.org/TR/xhtml1/transitional"
                exclude-result-prefixes="#default">

<xsl:import href="http://docbook.sourceforge.net/release/xsl/current/xhtml/docbook.xsl"/>
<xsl:import href="stylesheet-common.xsl" />

<!-- The customizations here are somewhat random in order to make the text
     output look good. -->

<!-- no section numbers or ToC -->
<xsl:param name="chapter.autolabel" select="0"/>
<xsl:param name="section.autolabel" select="0"/>
<xsl:param name="generate.toc"></xsl:param>

<!-- don't need them, and they mess up formatting -->
<xsl:template match="indexterm">
</xsl:template>

<xsl:template match="step">
  <li>
    <xsl:call-template name="common.html.attributes"/>
    <xsl:call-template name="id.attribute"/>
<!-- messes up formatting
    <xsl:call-template name="anchor"/>
-->
    <xsl:apply-templates/>
  </li>
</xsl:template>

<!-- produce "ASCII markup" for emphasis and such -->

<xsl:template match="emphasis">
  <xsl:text>*</xsl:text>
  <xsl:apply-templates/>
  <xsl:text>*</xsl:text>
</xsl:template>

<xsl:template match="para/command|para/filename|para/option|para/replaceable">
  <xsl:call-template name="gentext.startquote"/>
  <xsl:apply-templates/>
  <xsl:call-template name="gentext.endquote"/>
</xsl:template>

<xsl:template match="filename/replaceable|firstterm">
  <xsl:apply-templates/>
</xsl:template>

<!-- tweak formatting for note, warning, etc. -->
<xsl:template name="nongraphical.admonition">
  <div>
    <xsl:call-template name="common.html.attributes">
      <xsl:with-param name="inherit" select="1"/>
    </xsl:call-template>
    <xsl:call-template name="id.attribute"/>

    <xsl:if test="$admon.textlabel != 0 or title or info/title">
      <p>
        <b>
          <xsl:call-template name="anchor"/>
          <xsl:apply-templates select="." mode="object.title.markup"/>:
        </b>
      </p>
    </xsl:if>

    <xsl:apply-templates/>
  </div>
</xsl:template>

<!-- horizontal rules before titles (matches old DSSSL style) -->

<xsl:template match="sect1/title
                    |sect1/info/title
                    |sect1info/title"
              mode="titlepage.mode" priority="2">
  <hr/>
  <xsl:call-template name="section.title"/>
</xsl:template>

<xsl:template match="sect2/title
                    |sect2/info/title
                    |sect2info/title"
              mode="titlepage.mode" priority="2">
  <hr/>
  <xsl:call-template name="section.title"/>
</xsl:template>

<xsl:template match="sect3/title
                    |sect3/info/title
                    |sect3info/title"
              mode="titlepage.mode" priority="2">
  <hr/>
  <xsl:call-template name="section.title"/>
</xsl:template>

</xsl:stylesheet>
