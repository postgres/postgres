<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:svg="http://www.w3.org/2000/svg"
                version="1.0">

<!--
Transform the SVG produced by various tools, applying assorted fixups.
-->

<!--
Add viewBox attribute to svg element if not already present.  This allows the
image to scale.
-->
<xsl:template match="svg:svg">
  <xsl:copy>
    <xsl:if test="not(@viewBox)">
      <xsl:attribute name="viewBox">
        <xsl:text>0 0 </xsl:text>
        <xsl:value-of select="@width"/>
        <xsl:text> </xsl:text>
        <xsl:value-of select="@height"/>
      </xsl:attribute>
    </xsl:if>
    <xsl:apply-templates select="@* | node()"/>
  </xsl:copy>
</xsl:template>

<!--
Fix stroke="transparent" attribute, which is invalid SVG.
-->
<xsl:template match="@stroke[.='transparent']">
  <xsl:attribute name="stroke">none</xsl:attribute>
</xsl:template>

<!--
copy everything else
-->
<xsl:template match="@* | node()">
  <xsl:copy>
    <xsl:apply-templates select="@* | node()"/>
  </xsl:copy>
</xsl:template>

</xsl:stylesheet>
