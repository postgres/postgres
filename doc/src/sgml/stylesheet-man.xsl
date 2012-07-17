<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:exsl="http://exslt.org/common"
                version='1.0'
                exclude-result-prefixes="exsl">

<xsl:import href="http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl"/>
<xsl:import href="stylesheet-common.xsl" />


<!-- Parameters -->

<xsl:param name="man.authors.section.enabled">0</xsl:param>
<xsl:param name="man.copyright.section.enabled">0</xsl:param>
<xsl:param name="man.output.base.dir"></xsl:param>
<xsl:param name="man.output.in.separate.dir" select="1"></xsl:param>
<xsl:param name="refentry.meta.get.quietly" select="0"></xsl:param>
<xsl:param name="man.th.title.max.length">32</xsl:param> <!-- enough room for "CREATE TEXT SEARCH CONFIGURATION" -->
<xsl:param name="man.th.extra3.max.length">40</xsl:param> <!-- enough room for "PostgreSQL X.Ydevel Documentation" -->
<xsl:param name="refentry.xref.manvolnum" select="1"/> <!-- overridden from stylesheet-common.xsl -->

<!-- Fixup for apostrophe groff output.  See the following references:
     <http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=457839>
     <https://sourceforge.net/tracker/?func=detail&aid=2412738&group_id=21935&atid=373747>
 -->
<xsl:param name="man.string.subst.map.local.post">
  <substitution oldstring="\'" newstring="\(aq"></substitution>
</xsl:param>


<!-- Custom templates -->

<!-- Improve output of email element.  See also
     <https://sourceforge.net/tracker/?func=detail&aid=3524417&group_id=21935&atid=373747>
 -->
<xsl:template match="email">
  <xsl:text>&lt;</xsl:text>
  <xsl:apply-templates/>
  <xsl:text>&gt;</xsl:text>
</xsl:template>


<xsl:template match="refentry" mode="xref-to">
  <xsl:param name="referrer"/>
  <xsl:param name="xrefstyle"/>

  <xsl:choose>
    <!-- If the refname contains a space, we construct a reference
         like CREATE DATABASE (CREATE_DATABASE(7)), so the reader
         knows both the command name being referred to and the name of
         the man page to read about it. -->
    <xsl:when test="contains(refnamediv/refname[1],' ')">
      <xsl:variable name="mangled.title">
        <xsl:value-of select="translate(refnamediv/refname[1],' ','_')"/>
      </xsl:variable>
      <xsl:apply-templates select="refnamediv/refname[1]"/>
      <xsl:text> (</xsl:text>
      <xsl:call-template name="bold">
        <xsl:with-param name="node" select="exsl:node-set($mangled.title)"/>
        <xsl:with-param name="context" select="."/>
      </xsl:call-template>
      <xsl:apply-templates select="refmeta/manvolnum"/>
      <xsl:text>)</xsl:text>
    </xsl:when>

    <!-- This is the original case, except that boldness has been
         added, per the convention mentioned in man-pages(7). -->
    <xsl:otherwise>
      <xsl:choose>
        <xsl:when test="refmeta/refentrytitle">
          <xsl:call-template name="bold">
            <xsl:with-param name="node" select="refmeta/refentrytitle"/>
            <xsl:with-param name="context" select="."/>
          </xsl:call-template>
        </xsl:when>
        <xsl:otherwise>
          <xsl:call-template name="bold">
            <xsl:with-param name="node" select="refnamediv/refname[1]"/>
            <xsl:with-param name="context" select="."/>
          </xsl:call-template>
        </xsl:otherwise>
      </xsl:choose>
      <xsl:apply-templates select="refmeta/manvolnum"/>
    </xsl:otherwise>
  </xsl:choose>

</xsl:template>


<!-- For refentries we don't man to generate a man page for, leave out
     manvolnum, let it default to 0, and skip writing out man files
     with section 0. -->

<!-- overridden from common/refentry.xsl -->
<xsl:template name="get.refentry.section">
  <xsl:choose>
    <xsl:when test="refmeta/manvolnum">
      <xsl:value-of select="refmeta/manvolnum"/>
    </xsl:when>
    <xsl:otherwise>
      <xsl:text>0</xsl:text>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<!-- overridden from manpages/other.xsl -->
  <xsl:template name="write.man.file">
    <xsl:param name="name"/>
    <xsl:param name="section"/>
    <xsl:param name="lang"/>
    <xsl:param name="content"/>
    <xsl:param name="filename">
      <xsl:call-template name="make.adjusted.man.filename">
        <xsl:with-param name="name" select="$name"/>
        <xsl:with-param name="section" select="$section"/>
        <xsl:with-param name="lang" select="$lang"/>
      </xsl:call-template>
    </xsl:param>
    <xsl:if test="$section != 0">
    <xsl:call-template name="write.text.chunk">
      <xsl:with-param name="filename" select="$filename"/>
      <xsl:with-param name="suppress-context-node-name" select="1"/>
      <xsl:with-param name="quiet" select="$man.output.quietly"/>
      <xsl:with-param
          name="message-prolog"
          >Note: </xsl:with-param>
      <xsl:with-param name="encoding" select="$man.output.encoding"/>
      <xsl:with-param name="content" select="$content"/>
    </xsl:call-template>
    </xsl:if>
  </xsl:template>


<!-- Overridden template as workaround for this problem:
     <https://sourceforge.net/tracker/?func=detail&aid=2831602&group_id=21935&atid=373747>
-->
  <xsl:template name="write.stubs">
    <xsl:param name="first.refname"/>
    <xsl:param name="section"/>
    <xsl:param name="lang"/>
    <xsl:for-each select="refnamediv/refname">
      <xsl:if test=". != $first.refname">
        <xsl:call-template name="write.text.chunk">
          <xsl:with-param name="filename">
            <xsl:call-template name="make.adjusted.man.filename">
              <xsl:with-param name="name" select="."/>
              <xsl:with-param name="section" select="$section"/>
              <xsl:with-param name="lang" select="$lang"/>
            </xsl:call-template>
          </xsl:with-param>
          <xsl:with-param name="quiet" select="$man.output.quietly"/>
          <xsl:with-param name="suppress-context-node-name" select="1"/>
          <xsl:with-param name="message-prolog">Note: </xsl:with-param>
          <xsl:with-param name="message-epilog"> (soelim stub)</xsl:with-param>
          <xsl:with-param name="content">
            <xsl:choose>
              <xsl:when test="$man.output.in.separate.dir = 0">
                <xsl:value-of select="concat('.so man', $section, '/')"/>
              </xsl:when>
              <xsl:otherwise>
                <xsl:value-of select="'.so '"/> <!-- added case -->
              </xsl:otherwise>
            </xsl:choose>
            <xsl:call-template name="make.adjusted.man.filename">
              <xsl:with-param name="name" select="$first.refname"/>
              <xsl:with-param name="section" select="$section"/>
            </xsl:call-template>
            <xsl:text>&#10;</xsl:text>
          </xsl:with-param>
        </xsl:call-template>
      </xsl:if>
    </xsl:for-each>
  </xsl:template>


  <!-- https://sourceforge.net/tracker/?func=detail&aid=3545150&group_id=21935&atid=373747 -->
  <xsl:template match="step/title">
    <xsl:apply-templates/>
    <xsl:text>: </xsl:text>
  </xsl:template>


<!-- Gentext customization -->

<!-- see http://www.sagehill.net/docbookxsl/CustomGentext.html -->
<xsl:param name="local.l10n.xml" select="document('')"/>
<l:i18n xmlns:l="http://docbook.sourceforge.net/xmlns/l10n/1.0">
  <l:l10n language="en">
    <!-- Use ISO 8601 date format. -->
    <l:context name="datetime">
      <l:template name="format" text="Y-m-d"/>
    </l:context>

    <!-- Slight rephrasing to indicate that missing sections are found
         in the documentation. -->
    <l:context name="xref-number-and-title">
      <l:template name="chapter" text="Chapter %n, %t, in the documentation"/>
      <l:template name="sect1" text="Section %n, “%t”, in the documentation"/>
      <l:template name="sect2" text="Section %n, “%t”, in the documentation"/>
      <l:template name="sect3" text="Section %n, “%t”, in the documentation"/>
      <l:template name="sect4" text="Section %n, “%t”, in the documentation"/>
      <l:template name="sect5" text="Section %n, “%t”, in the documentation"/>
    </l:context>
  </l:l10n>
</l:i18n>

</xsl:stylesheet>
