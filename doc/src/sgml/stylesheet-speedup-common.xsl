<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version='1.0'>

<!-- Performance-optimized versions of some upstream templates from common/
     directory -->

<!-- from common/labels.xsl -->

<xsl:template match="chapter" mode="label.markup">
  <xsl:choose>
    <xsl:when test="@label">
      <xsl:value-of select="@label"/>
    </xsl:when>
    <xsl:when test="string($chapter.autolabel) != 0">
      <xsl:if test="$component.label.includes.part.label != 0 and
                      ancestor::part">
        <xsl:variable name="part.label">
          <xsl:apply-templates select="ancestor::part"
                               mode="label.markup"/>
        </xsl:variable>
        <xsl:if test="$part.label != ''">
          <xsl:value-of select="$part.label"/>
          <xsl:apply-templates select="ancestor::part"
                               mode="intralabel.punctuation">
            <xsl:with-param name="object" select="."/>
          </xsl:apply-templates>
        </xsl:if>
      </xsl:if>
      <xsl:variable name="format">
        <xsl:call-template name="autolabel.format">
          <xsl:with-param name="format" select="$chapter.autolabel"/>
        </xsl:call-template>
      </xsl:variable>
      <xsl:choose>
        <xsl:when test="$label.from.part != 0 and ancestor::part">
          <xsl:number from="part" count="chapter" format="{$format}" level="any"/>
        </xsl:when>
        <xsl:otherwise>
          <!-- Optimization for pgsql-docs: When counting to get label for
               this chapter, preceding chapters can only be our siblings or
               children of a preceding part, so only count those instead of
               scanning the entire node tree. -->
          <!-- <xsl:number from="book" count="chapter" format="{$format}" level="any"/> -->
          <xsl:number value="count(../preceding-sibling::part/chapter) + count(preceding-sibling::chapter) + 1" format="{$format}"/>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:when>
  </xsl:choose>
</xsl:template>

<xsl:template match="appendix" mode="label.markup">
  <xsl:choose>
    <xsl:when test="@label">
      <xsl:value-of select="@label"/>
    </xsl:when>
    <xsl:when test="string($appendix.autolabel) != 0">
      <xsl:if test="$component.label.includes.part.label != 0 and
                      ancestor::part">
        <xsl:variable name="part.label">
          <xsl:apply-templates select="ancestor::part"
                               mode="label.markup"/>
        </xsl:variable>
        <xsl:if test="$part.label != ''">
          <xsl:value-of select="$part.label"/>
          <xsl:apply-templates select="ancestor::part"
                               mode="intralabel.punctuation">
            <xsl:with-param name="object" select="."/>
          </xsl:apply-templates>
        </xsl:if>
      </xsl:if>
      <xsl:variable name="format">
        <xsl:call-template name="autolabel.format">
          <xsl:with-param name="format" select="$appendix.autolabel"/>
        </xsl:call-template>
      </xsl:variable>
      <xsl:choose>
        <xsl:when test="$label.from.part != 0 and ancestor::part">
          <xsl:number from="part" count="appendix" format="{$format}" level="any"/>
        </xsl:when>
        <xsl:otherwise>
          <!-- Optimization for pgsql-docs: When counting to get label for
               this appendix, preceding appendixes can only be our siblings or
               children of a preceding part, so only count those instead of
               scanning the entire node tree. -->
          <!-- <xsl:number from="book|article" count="appendix" format="{$format}" level="any"/> -->
          <xsl:number value="count(../preceding-sibling::part/appendix) + count(preceding-sibling::appendix) + 1" format="{$format}"/>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:when>
  </xsl:choose>
</xsl:template>

<!-- from common/l10n.xsl -->

<!-- Just hardcode the language for the whole document, to make it faster. -->

<xsl:template name="l10n.language">en</xsl:template>

</xsl:stylesheet>
