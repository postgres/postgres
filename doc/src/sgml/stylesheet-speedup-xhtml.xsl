<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns="http://www.w3.org/1999/xhtml"
                version='1.0'>

<!-- Performance-optimized versions of some upstream templates from xhtml/
     directory -->

<!-- from xhtml/autoidx.xsl -->

<xsl:template match="indexterm" mode="reference">
  <xsl:param name="scope" select="."/>
  <xsl:param name="role" select="''"/>
  <xsl:param name="type" select="''"/>
  <xsl:param name="position"/>
  <xsl:param name="separator" select="''"/>

  <xsl:variable name="term.separator">
    <xsl:call-template name="index.separator">
      <xsl:with-param name="key" select="'index.term.separator'"/>
    </xsl:call-template>
  </xsl:variable>

  <xsl:variable name="number.separator">
    <xsl:call-template name="index.separator">
      <xsl:with-param name="key" select="'index.number.separator'"/>
    </xsl:call-template>
  </xsl:variable>

  <xsl:variable name="range.separator">
    <xsl:call-template name="index.separator">
      <xsl:with-param name="key" select="'index.range.separator'"/>
    </xsl:call-template>
  </xsl:variable>

  <xsl:choose>
    <xsl:when test="$separator != ''">
      <xsl:value-of select="$separator"/>
    </xsl:when>
    <xsl:when test="$position = 1">
      <xsl:value-of select="$term.separator"/>
    </xsl:when>
    <xsl:otherwise>
      <xsl:value-of select="$number.separator"/>
    </xsl:otherwise>
  </xsl:choose>

  <xsl:choose>
    <xsl:when test="@zone and string(@zone)">
      <xsl:call-template name="reference">
        <xsl:with-param name="zones" select="normalize-space(@zone)"/>
        <xsl:with-param name="position" select="position()"/>
        <xsl:with-param name="scope" select="$scope"/>
        <xsl:with-param name="role" select="$role"/>
        <xsl:with-param name="type" select="$type"/>
      </xsl:call-template>
    </xsl:when>
    <xsl:otherwise>
      <a>
        <xsl:apply-templates select="." mode="class.attribute"/>
        <xsl:variable name="title">
          <xsl:choose>
            <xsl:when test="$index.prefer.titleabbrev != 0">
              <xsl:apply-templates select="(ancestor-or-self::set|ancestor-or-self::book|ancestor-or-self::part|ancestor-or-self::reference|ancestor-or-self::partintro|ancestor-or-self::chapter|ancestor-or-self::appendix|ancestor-or-self::preface|ancestor-or-self::article|ancestor-or-self::section|ancestor-or-self::sect1|ancestor-or-self::sect2|ancestor-or-self::sect3|ancestor-or-self::sect4|ancestor-or-self::sect5|ancestor-or-self::refentry|ancestor-or-self::refsect1|ancestor-or-self::refsect2|ancestor-or-self::refsect3|ancestor-or-self::simplesect|ancestor-or-self::bibliography|ancestor-or-self::glossary|ancestor-or-self::index|ancestor-or-self::webpage|ancestor-or-self::topic)[last()]" mode="titleabbrev.markup"/>
            </xsl:when>
            <xsl:otherwise>
              <xsl:apply-templates select="(ancestor-or-self::set|ancestor-or-self::book|ancestor-or-self::part|ancestor-or-self::reference|ancestor-or-self::partintro|ancestor-or-self::chapter|ancestor-or-self::appendix|ancestor-or-self::preface|ancestor-or-self::article|ancestor-or-self::section|ancestor-or-self::sect1|ancestor-or-self::sect2|ancestor-or-self::sect3|ancestor-or-self::sect4|ancestor-or-self::sect5|ancestor-or-self::refentry|ancestor-or-self::refsect1|ancestor-or-self::refsect2|ancestor-or-self::refsect3|ancestor-or-self::simplesect|ancestor-or-self::bibliography|ancestor-or-self::glossary|ancestor-or-self::index|ancestor-or-self::webpage|ancestor-or-self::topic)[last()]" mode="title.markup"/>
            </xsl:otherwise>
          </xsl:choose>
        </xsl:variable>

        <xsl:attribute name="href">
          <xsl:choose>
            <xsl:when test="$index.links.to.section = 1">
              <xsl:call-template name="href.target">
                <xsl:with-param name="object" select="(ancestor-or-self::set|ancestor-or-self::book|ancestor-or-self::part|ancestor-or-self::reference|ancestor-or-self::partintro|ancestor-or-self::chapter|ancestor-or-self::appendix|ancestor-or-self::preface|ancestor-or-self::article|ancestor-or-self::section|ancestor-or-self::sect1|ancestor-or-self::sect2|ancestor-or-self::sect3|ancestor-or-self::sect4|ancestor-or-self::sect5|ancestor-or-self::refentry|ancestor-or-self::refsect1|ancestor-or-self::refsect2|ancestor-or-self::refsect3|ancestor-or-self::simplesect|ancestor-or-self::bibliography|ancestor-or-self::glossary|ancestor-or-self::index|ancestor-or-self::webpage|ancestor-or-self::topic)[last()]"/>
                <!-- Optimization for pgsql-docs: We only have an index as a
                     child of book, so look that up directly instead of
                     scanning the entire node tree.  Also, don't look for
                     setindex. -->
                <!-- <xsl:with-param name="context" select="(//index[count(ancestor::node()|$scope) = count(ancestor::node()) and ($role = @role or $type = @type or (string-length($role) = 0 and string-length($type) = 0))] | //setindex[count(ancestor::node()|$scope) = count(ancestor::node()) and ($role = @role or $type = @type or (string-length($role) = 0 and string-length($type) = 0))])[1]"/> -->
                <xsl:with-param name="context" select="(/book/index[count(ancestor::node()|$scope) = count(ancestor::node()) and ($role = @role or $type = @type or (string-length($role) = 0 and string-length($type) = 0))])[1]"/>
              </xsl:call-template>
            </xsl:when>
            <xsl:otherwise>
              <xsl:call-template name="href.target">
                <xsl:with-param name="object" select="."/>
                <xsl:with-param name="context" select="(//index[count(ancestor::node()|$scope) = count(ancestor::node()) and ($role = @role or $type = @type or (string-length($role) = 0 and string-length($type) = 0))] | //setindex[count(ancestor::node()|$scope) = count(ancestor::node()) and ($role = @role or $type = @type or (string-length($role) = 0 and string-length($type) = 0))])[1]"/>
              </xsl:call-template>
            </xsl:otherwise>
          </xsl:choose>

        </xsl:attribute>

        <xsl:value-of select="$title"/> <!-- text only -->
      </a>

      <xsl:variable name="id" select="(@id|@xml:id)[1]"/>
      <xsl:if test="key('endofrange', $id)[count(ancestor::node()|$scope) = count(ancestor::node()) and ($role = @role or $type = @type or (string-length($role) = 0 and string-length($type) = 0))]">
        <xsl:apply-templates select="key('endofrange', $id)[count(ancestor::node()|$scope) = count(ancestor::node()) and ($role = @role or $type = @type or (string-length($role) = 0 and string-length($type) = 0))][last()]" mode="reference">
          <xsl:with-param name="position" select="position()"/>
          <xsl:with-param name="scope" select="$scope"/>
          <xsl:with-param name="role" select="$role"/>
          <xsl:with-param name="type" select="$type"/>
          <xsl:with-param name="separator" select="$range.separator"/>
        </xsl:apply-templates>
      </xsl:if>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<xsl:template name="reference">
  <xsl:param name="scope" select="."/>
  <xsl:param name="role" select="''"/>
  <xsl:param name="type" select="''"/>
  <xsl:param name="zones"/>

  <xsl:choose>
    <xsl:when test="contains($zones, ' ')">
      <xsl:variable name="zone" select="substring-before($zones, ' ')"/>
      <xsl:variable name="target" select="key('sections', $zone)"/>

      <a>
        <xsl:apply-templates select="." mode="class.attribute"/>
<!--    Optimization for pgsql-docs: this call adds nothing but fails with docbook-xsl 1.76 -->
<!--    <xsl:call-template name="id.attribute"/> -->
        <xsl:attribute name="href">
          <xsl:call-template name="href.target">
            <xsl:with-param name="object" select="$target[1]"/>
            <xsl:with-param name="context" select="//index[count(ancestor::node()|$scope) = count(ancestor::node()) and ($role = @role or $type = @type or (string-length($role) = 0 and string-length($type) = 0))][1]"/>
          </xsl:call-template>
        </xsl:attribute>
        <xsl:apply-templates select="$target[1]" mode="index-title-content"/>
      </a>
      <xsl:text>, </xsl:text>
      <xsl:call-template name="reference">
        <xsl:with-param name="zones" select="substring-after($zones, ' ')"/>
        <xsl:with-param name="position" select="position()"/>
        <xsl:with-param name="scope" select="$scope"/>
        <xsl:with-param name="role" select="$role"/>
        <xsl:with-param name="type" select="$type"/>
      </xsl:call-template>
    </xsl:when>
    <xsl:otherwise>
      <xsl:variable name="zone" select="$zones"/>
      <xsl:variable name="target" select="key('sections', $zone)"/>

      <a>
        <xsl:apply-templates select="." mode="class.attribute"/>
<!--    Optimization for pgsql-docs: this call adds nothing but fails with docbook-xsl 1.76 -->
<!--    <xsl:call-template name="id.attribute"/> -->
        <xsl:attribute name="href">
          <xsl:call-template name="href.target">
            <xsl:with-param name="object" select="$target[1]"/>
            <!-- Optimization for pgsql-docs: Only look for index under book
                 instead of searching the whole node tree. -->
            <!-- <xsl:with-param name="context" select="//index[count(ancestor::node()|$scope) = count(ancestor::node()) and ($role = @role or $type = @type or (string-length($role) = 0 and string-length($type) = 0))][1]"/> -->
            <xsl:with-param name="context" select="/book/index[count(ancestor::node()|$scope) = count(ancestor::node()) and ($role = @role or $type = @type or (string-length($role) = 0 and string-length($type) = 0))][1]"/>
          </xsl:call-template>
        </xsl:attribute>
        <xsl:apply-templates select="$target[1]" mode="index-title-content"/>
      </a>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>


<!-- from xhtml/chunk-common.xsl -->

<xsl:template name="chunk-all-sections">
  <xsl:param name="content">
    <xsl:apply-imports/>
  </xsl:param>

  <!-- Optimization for pgsql-docs: Since we set a fixed $chunk.section.depth,
       we can do away with a bunch of complicated XPath searches for the
       previous and next sections at various levels. -->

  <xsl:if test="$chunk.section.depth != 1">
    <xsl:message terminate="yes">
      <xsl:text>Error: If you change $chunk.section.depth, then you must update the performance-optimized chunk-all-sections-template.</xsl:text>
    </xsl:message>
  </xsl:if>

  <xsl:variable name="prev"
    select="(preceding::book[1]
             |preceding::preface[1]
             |preceding::chapter[1]
             |preceding::appendix[1]
             |preceding::part[1]
             |preceding::reference[1]
             |preceding::refentry[1]
             |preceding::colophon[1]
             |preceding::article[1]
             |preceding::topic[1]
             |preceding::bibliography[parent::article or parent::book or parent::part][1]
             |preceding::glossary[parent::article or parent::book or parent::part][1]
             |preceding::index[$generate.index != 0]
                               [parent::article or parent::book or parent::part][1]
             |preceding::setindex[$generate.index != 0][1]
             |ancestor::set
             |ancestor::book[1]
             |ancestor::preface[1]
             |ancestor::chapter[1]
             |ancestor::appendix[1]
             |ancestor::part[1]
             |ancestor::reference[1]
             |ancestor::article[1]
             |ancestor::topic[1]
             |preceding::sect1[1]
             |ancestor::sect1[1])[last()]"/>

  <xsl:variable name="next"
    select="(following::book[1]
             |following::preface[1]
             |following::chapter[1]
             |following::appendix[1]
             |following::part[1]
             |following::reference[1]
             |following::refentry[1]
             |following::colophon[1]
             |following::bibliography[parent::article or parent::book or parent::part][1]
             |following::glossary[parent::article or parent::book or parent::part][1]
             |following::index[$generate.index != 0]
                               [parent::article or parent::book][1]
             |following::article[1]
             |following::topic[1]
             |following::setindex[$generate.index != 0][1]
             |descendant::book[1]
             |descendant::preface[1]
             |descendant::chapter[1]
             |descendant::appendix[1]
             |descendant::article[1]
             |descendant::topic[1]
             |descendant::bibliography[parent::article or parent::book][1]
             |descendant::glossary[parent::article or parent::book or parent::part][1]
             |descendant::index[$generate.index != 0]
                                [parent::article or parent::book][1]
             |descendant::colophon[1]
             |descendant::setindex[$generate.index != 0][1]
             |descendant::part[1]
             |descendant::reference[1]
             |descendant::refentry[1]
             |following::sect1[1]
             |descendant::sect1[1])[1]"/>

  <xsl:call-template name="process-chunk">
    <xsl:with-param name="prev" select="$prev"/>
    <xsl:with-param name="next" select="$next"/>
    <xsl:with-param name="content" select="$content"/>
  </xsl:call-template>
</xsl:template>

</xsl:stylesheet>
