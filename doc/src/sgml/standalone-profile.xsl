<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version="1.0">

<!--
This is a preprocessing layer to convert the installation instructions into a
variant without links and references to the main documentation.

- To omit something in the stand-alone INSTALL file, give the element a
  condition="standalone-ignore" attribute.

- If there is no element that exactly covers what you want to change, wrap it
  in a <phrase> element, which otherwise does nothing.

- Otherwise, write a custom rule below.
-->

<xsl:output
    doctype-public="-//OASIS//DTD DocBook XML V4.5//EN"
    doctype-system="http://www.oasis-open.org/docbook/xml/4.5/docbookx.dtd"/>

<!-- copy everything by default -->

<xsl:template match="@*|node()">
  <xsl:copy>
    <xsl:apply-templates select="@*|node()" />
  </xsl:copy>
</xsl:template>

<!-- particular conversions -->

<xsl:template match="*[@condition='standalone-ignore']">
</xsl:template>

<xsl:template match="phrase/text()['chapter']">
  <xsl:text>document</xsl:text>
</xsl:template>

<xsl:template match="phrase[@id='install-ldap-links']">
  <xsl:text>the documentation about client authentication and libpq</xsl:text>
</xsl:template>

<xsl:template match="xref[@linkend='docguide-toolsets']">
  <xsl:text>the main documentation's appendix on documentation</xsl:text>
</xsl:template>

<xsl:template match="xref[@linkend='dynamic-trace']">
  <xsl:text>the documentation</xsl:text>
</xsl:template>

<xsl:template match="xref[@linkend='install-windows']">
  <xsl:text>the documentation</xsl:text>
</xsl:template>

<xsl:template match="xref[@linkend='pgcrypto']">
  <xsl:text>pgcrypto</xsl:text>
</xsl:template>

<xsl:template match="xref[@linkend='plpython-python23']">
  <xsl:text>the </xsl:text><application>PL/Python</application><xsl:text> documentation</xsl:text>
</xsl:template>

<xsl:template match="xref[@linkend='regress']">
  <xsl:text>the file </xsl:text>
  <filename>src/test/regress/README</filename>
  <xsl:text> and the documentation</xsl:text>
</xsl:template>

<xsl:template match="xref[@linkend='upgrading']">
  <xsl:text>the documentation</xsl:text>
</xsl:template>

<xsl:template match="xref[@linkend='uuid-ossp']">
  <xsl:text>uuid-ossp</xsl:text>
</xsl:template>

<xsl:template match="xref[@linkend='xml2']">
  <xsl:text>xml2</xsl:text>
</xsl:template>

</xsl:stylesheet>
