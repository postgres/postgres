<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version='1.0'
                xmlns="http://www.w3.org/TR/xhtml1/transitional"
                exclude-result-prefixes="#default">

<xsl:import href="http://docbook.sourceforge.net/release/xsl/current/xhtml/docbook.xsl"/>
<xsl:include href="stylesheet-common.xsl" />
<xsl:include href="stylesheet-html-common.xsl" />
<xsl:include href="stylesheet-speedup-xhtml.xsl" />

<!-- embed SVG images into output file -->
<xsl:template match="imagedata[@format='SVG']">
  <xsl:variable name="filename">
    <xsl:call-template name="mediaobject.filename">
      <xsl:with-param name="object" select=".."/>
    </xsl:call-template>
  </xsl:variable>

  <xsl:copy-of select="document($filename)"/>
</xsl:template>

</xsl:stylesheet>
