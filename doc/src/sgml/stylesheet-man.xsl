<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version="1.0">

<xsl:import href="http://docbook2x.sourceforge.net/latest/xslt/man/docbook.xsl"/>

<!--
  Man pages don't really support a third section level, but this
  makes our man pages work OK and matches the behavior of the sgmlspl
  style.
 -->
<xsl:template match="refsect3">
  <xsl:call-template name="SS-section" />
</xsl:template>

</xsl:stylesheet>
