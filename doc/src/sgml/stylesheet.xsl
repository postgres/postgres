<?xml version='1.0'?>
<!DOCTYPE xsl:stylesheet [
<!ENTITY % common.entities SYSTEM "http://docbook.sourceforge.net/release/xsl/current/common/entities.ent">
%common.entities;
]>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version='1.0'
                xmlns="http://www.w3.org/TR/xhtml1/transitional"
                exclude-result-prefixes="#default">

<xsl:import href="http://docbook.sourceforge.net/release/xsl/current/xhtml/chunk.xsl"/>
<xsl:include href="stylesheet-common.xsl" />
<xsl:include href="stylesheet-speedup-xhtml.xsl" />


<!-- Parameters -->
<xsl:param name="base.dir" select="'html/'"></xsl:param>
<xsl:param name="use.id.as.filename" select="'1'"></xsl:param>
<xsl:param name="make.valid.html" select="1"></xsl:param>
<xsl:param name="generate.id.attributes" select="1"></xsl:param>
<xsl:param name="generate.legalnotice.link" select="1"></xsl:param>
<xsl:param name="link.mailto.url">pgsql-docs@postgresql.org</xsl:param>
<xsl:param name="chunk.first.sections" select="1"/>
<xsl:param name="chunk.quietly" select="1"></xsl:param>
<xsl:param name="toc.max.depth">2</xsl:param>

<xsl:param name="website.stylesheet" select="0"/>

<xsl:param name="html.stylesheet">
  <xsl:choose>
    <xsl:when test="$website.stylesheet = 0">stylesheet.css</xsl:when>
    <xsl:otherwise>http://www.postgresql.org/media/css/docs.css</xsl:otherwise>
  </xsl:choose>
</xsl:param>


<!-- Change display of some elements -->

<xsl:template match="command">
  <xsl:call-template name="inline.monoseq"/>
</xsl:template>


<!-- table of contents configuration -->

<xsl:param name="generate.toc">
appendix  toc,title
article/appendix  nop
article   toc,title
book      toc,title
chapter   toc,title
part      toc,title
preface   toc,title
qandadiv  toc
qandaset  toc
reference toc,title
sect1     toc
sect2     toc
sect3     toc
sect4     toc
sect5     toc
section   toc
set       toc,title
</xsl:param>


<!-- Put index "quicklinks" (A | B | C | ...) at the top of the bookindex page. -->

<!-- from html/autoidx.xsl -->

<xsl:template name="generate-basic-index">
  <xsl:param name="scope" select="NOTANODE"/>

  <xsl:variable name="role">
    <xsl:if test="$index.on.role != 0">
      <xsl:value-of select="@role"/>
    </xsl:if>
  </xsl:variable>

  <xsl:variable name="type">
    <xsl:if test="$index.on.type != 0">
      <xsl:value-of select="@type"/>
    </xsl:if>
  </xsl:variable>

  <xsl:variable name="terms"
                select="//indexterm
                        [count(.|key('letter',
                          translate(substring(&primary;, 1, 1),
                             &lowercase;,
                             &uppercase;))
                          [&scope;][1]) = 1
                          and not(@class = 'endofrange')]"/>

  <xsl:variable name="alphabetical"
                select="$terms[contains(concat(&lowercase;, &uppercase;),
                                        substring(&primary;, 1, 1))]"/>

  <xsl:variable name="others" select="$terms[not(contains(concat(&lowercase;,
                                                 &uppercase;),
                                             substring(&primary;, 1, 1)))]"/>

  <div class="index">
    <!-- pgsql-docs: begin added stuff -->
    <p class="indexdiv-quicklinks">
      <a href="#indexdiv-Symbols">
        <xsl:call-template name="gentext">
          <xsl:with-param name="key" select="'index symbols'"/>
        </xsl:call-template>
      </a>
      <xsl:apply-templates select="$alphabetical[count(.|key('letter',
                                   translate(substring(&primary;, 1, 1),
                                   &lowercase;,&uppercase;))[&scope;][1]) = 1]"
                           mode="index-div-quicklinks">
        <xsl:with-param name="position" select="position()"/>
        <xsl:with-param name="scope" select="$scope"/>
        <xsl:with-param name="role" select="$role"/>
        <xsl:with-param name="type" select="$type"/>
        <xsl:sort select="translate(&primary;, &lowercase;, &uppercase;)"/>
      </xsl:apply-templates>
    </p>
    <!-- pgsql-docs: end added stuff -->

    <xsl:if test="$others">
      <xsl:choose>
        <xsl:when test="normalize-space($type) != '' and
                        $others[@type = $type][count(.|key('primary', &primary;)[&scope;][1]) = 1]">
          <!-- pgsql-docs: added id attribute here for linking to it -->
          <div class="indexdiv" id="indexdiv-Symbols">
            <h3>
              <xsl:call-template name="gentext">
                <xsl:with-param name="key" select="'index symbols'"/>
              </xsl:call-template>
            </h3>
            <dl>
              <xsl:apply-templates select="$others[count(.|key('primary', &primary;)[&scope;][1]) = 1]"
                                   mode="index-symbol-div">
                <xsl:with-param name="position" select="position()"/>
                <xsl:with-param name="scope" select="$scope"/>
                <xsl:with-param name="role" select="$role"/>
                <xsl:with-param name="type" select="$type"/>
                <xsl:sort select="translate(&primary;, &lowercase;, &uppercase;)"/>
              </xsl:apply-templates>
            </dl>
          </div>
        </xsl:when>
        <xsl:when test="normalize-space($type) != ''">
          <!-- Output nothing, as there isn't a match for $other using this $type -->
        </xsl:when>
        <xsl:otherwise>
          <!-- pgsql-docs: added id attribute here for linking to it -->
          <div class="indexdiv" id="indexdiv-Symbols">
            <h3>
              <xsl:call-template name="gentext">
                <xsl:with-param name="key" select="'index symbols'"/>
              </xsl:call-template>
            </h3>
            <dl>
              <xsl:apply-templates select="$others[count(.|key('primary',
                                          &primary;)[&scope;][1]) = 1]"
                                  mode="index-symbol-div">
                <xsl:with-param name="position" select="position()"/>
                <xsl:with-param name="scope" select="$scope"/>
                <xsl:with-param name="role" select="$role"/>
                <xsl:with-param name="type" select="$type"/>
                <xsl:sort select="translate(&primary;, &lowercase;, &uppercase;)"/>
              </xsl:apply-templates>
            </dl>
          </div>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:if>

    <xsl:apply-templates select="$alphabetical[count(.|key('letter',
                                 translate(substring(&primary;, 1, 1),
                                           &lowercase;,&uppercase;))[&scope;][1]) = 1]"
                         mode="index-div-basic">
      <xsl:with-param name="position" select="position()"/>
      <xsl:with-param name="scope" select="$scope"/>
      <xsl:with-param name="role" select="$role"/>
      <xsl:with-param name="type" select="$type"/>
      <xsl:sort select="translate(&primary;, &lowercase;, &uppercase;)"/>
    </xsl:apply-templates>
  </div>
</xsl:template>

<xsl:template match="indexterm" mode="index-div-basic">
  <xsl:param name="scope" select="."/>
  <xsl:param name="role" select="''"/>
  <xsl:param name="type" select="''"/>

  <xsl:variable name="key"
                select="translate(substring(&primary;, 1, 1),
                         &lowercase;,&uppercase;)"/>

  <xsl:if test="key('letter', $key)[&scope;]
                [count(.|key('primary', &primary;)[&scope;][1]) = 1]">
    <div class="indexdiv">
      <!-- pgsql-docs: added id attribute here for linking to it -->
      <xsl:attribute name="id">
        <xsl:value-of select="concat('indexdiv-', $key)"/>
      </xsl:attribute>

      <xsl:if test="contains(concat(&lowercase;, &uppercase;), $key)">
        <h3>
          <xsl:value-of select="translate($key, &lowercase;, &uppercase;)"/>
        </h3>
      </xsl:if>
      <dl>
        <xsl:apply-templates select="key('letter', $key)[&scope;]
                                     [count(.|key('primary', &primary;)
                                     [&scope;][1])=1]"
                             mode="index-primary">
          <xsl:with-param name="position" select="position()"/>
          <xsl:with-param name="scope" select="$scope"/>
          <xsl:with-param name="role" select="$role"/>
          <xsl:with-param name="type" select="$type"/>
          <xsl:sort select="translate(&primary;, &lowercase;, &uppercase;)"/>
        </xsl:apply-templates>
      </dl>
    </div>
  </xsl:if>
</xsl:template>

<!-- pgsql-docs -->
<xsl:template match="indexterm" mode="index-div-quicklinks">
  <xsl:param name="scope" select="."/>
  <xsl:param name="role" select="''"/>
  <xsl:param name="type" select="''"/>

  <xsl:variable name="key"
                select="translate(substring(&primary;, 1, 1),
                        &lowercase;,&uppercase;)"/>

  <xsl:if test="key('letter', $key)[&scope;]
                [count(.|key('primary', &primary;)[&scope;][1]) = 1]">
    <xsl:if test="contains(concat(&lowercase;, &uppercase;), $key)">
      |
      <a>
        <xsl:attribute name="href">
          <xsl:value-of select="concat('#indexdiv-', $key)"/>
        </xsl:attribute>
        <xsl:value-of select="translate($key, &lowercase;, &uppercase;)"/>
      </a>
    </xsl:if>
  </xsl:if>
</xsl:template>

</xsl:stylesheet>
