<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE xsl:stylesheet [
<!ENTITY % common.entities SYSTEM "http://docbook.sourceforge.net/release/xsl/current/common/entities.ent">
%common.entities;
]>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                version="1.0"
                xmlns="http://www.w3.org/1999/xhtml">

<!--
  This file contains XSLT stylesheet customizations that are common to
  all HTML output variants (chunked and single-page).
  -->

<!-- Parameters -->
<xsl:param name="make.valid.html" select="1"></xsl:param>
<xsl:param name="generate.id.attributes" select="1"></xsl:param>
<xsl:param name="make.graphic.viewport" select="0"/>
<xsl:param name="link.mailto.url">pgsql-docs@lists.postgresql.org</xsl:param>
<xsl:param name="toc.max.depth">2</xsl:param>
<xsl:param name="website.stylesheet" select="0"/>
<xsl:param name="custom.css.source">
  <xsl:if test="$website.stylesheet = 0">stylesheet.css.xml</xsl:if>
</xsl:param>
<xsl:param name="html.stylesheet">
  <xsl:if test="not($website.stylesheet = 0)">https://www.postgresql.org/media/css/docs-complete.css</xsl:if>
</xsl:param>


<!--
  The below allows the stylesheets provided by the website to be applied fully
  to the generated HTML.
  -->
<xsl:template name="body.attributes">
  <xsl:attribute name="id">docContent</xsl:attribute>
  <xsl:attribute name="class">container-fluid col-10</xsl:attribute>
</xsl:template>

<!-- Change display of some elements -->

<xsl:template match="command">
  <xsl:call-template name="inline.monoseq"/>
</xsl:template>

<xsl:template match="confgroup" mode="bibliography.mode">
  <span>
    <xsl:call-template name="common.html.attributes"/>
    <xsl:call-template name="id.attribute"/>
    <xsl:apply-templates select="conftitle/text()" mode="bibliography.mode"/>
    <xsl:text>, </xsl:text>
    <xsl:apply-templates select="confdates/text()" mode="bibliography.mode"/>
    <xsl:copy-of select="$biblioentry.item.separator"/>
  </span>
</xsl:template>

<xsl:template match="isbn" mode="bibliography.mode">
  <span>
    <xsl:call-template name="common.html.attributes"/>
    <xsl:call-template name="id.attribute"/>
    <xsl:text>ISBN </xsl:text>
    <xsl:apply-templates mode="bibliography.mode"/>
    <xsl:copy-of select="$biblioentry.item.separator"/>
  </span>
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

<xsl:param name="generate.section.toc.level" select="1"></xsl:param>

<!-- include refentry under sect1 in tocs -->
<xsl:template match="sect1" mode="toc">
  <xsl:param name="toc-context" select="."/>
  <xsl:call-template name="subtoc">
    <xsl:with-param name="toc-context" select="$toc-context"/>
    <xsl:with-param name="nodes" select="sect2|refentry
                                         |bridgehead[$bridgehead.in.toc != 0]"/>
  </xsl:call-template>
</xsl:template>


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

  <!-- pgsql-docs: added xmlns:xlink, autoidx.xsl doesn't include xlink in
       exclude-result-prefixes. Without our customization that just leads to a
       single xmlns:xlink in this div, but because we emit it it otherwise
       gets pushed down to the elements output by autoidx.xsl -->
  <div class="index" xmlns:xlink="http://www.w3.org/1999/xlink">
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


<!-- upper case HTML anchors for backward compatibility -->

<xsl:template name="object.id">
  <xsl:param name="object" select="."/>
  <xsl:choose>
    <xsl:when test="$object/@id">
      <xsl:value-of select="translate($object/@id, &lowercase;, &uppercase;)"/>
    </xsl:when>
    <xsl:when test="$object/@xml:id">
      <xsl:value-of select="$object/@xml:id"/>
    </xsl:when>
    <xsl:when test="$generate.consistent.ids != 0">
      <!-- Make $object the current node -->
      <xsl:for-each select="$object">
        <xsl:text>id-</xsl:text>
        <xsl:number level="multiple" count="*"/>
      </xsl:for-each>
    </xsl:when>
    <xsl:otherwise>
      <xsl:value-of select="generate-id($object)"/>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>


<!-- Add an id link to each section heading. -->

<!-- from html/sections.xsl -->
<xsl:template name="section.heading">
  <xsl:param name="section" select="."/>
  <xsl:param name="level" select="1"/>
  <xsl:param name="allow-anchors" select="1"/>
  <xsl:param name="title"/>
  <xsl:param name="class" select="'title'"/>

  <xsl:variable name="id">
    <xsl:choose>
      <!-- Make sure the subtitle doesn't get the same id as the title -->
      <xsl:when test="self::subtitle">
        <xsl:call-template name="object.id">
          <xsl:with-param name="object" select="."/>
        </xsl:call-template>
      </xsl:when>
      <!-- if title is in an *info wrapper, get the grandparent -->
      <xsl:when test="contains(local-name(..), 'info')">
        <xsl:call-template name="object.id">
          <xsl:with-param name="object" select="../.."/>
        </xsl:call-template>
      </xsl:when>
      <xsl:otherwise>
        <xsl:call-template name="object.id">
          <xsl:with-param name="object" select=".."/>
        </xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:variable>

  <!-- HTML H level is one higher than section level -->
  <xsl:variable name="hlevel">
    <xsl:choose>
      <!-- highest valid HTML H level is H6; so anything nested deeper
           than 5 levels down just becomes H6 -->
      <xsl:when test="$level &gt; 5">6</xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="$level + 1"/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:variable>
  <xsl:element name="h{$hlevel}" namespace="http://www.w3.org/1999/xhtml">
    <xsl:attribute name="class"><xsl:value-of select="$class"/></xsl:attribute>
    <xsl:if test="$css.decoration != '0'">
      <xsl:if test="$hlevel&lt;3">
        <xsl:attribute name="style">clear: both</xsl:attribute>
      </xsl:if>
    </xsl:if>
    <xsl:if test="$allow-anchors != 0">
      <xsl:call-template name="anchor">
        <xsl:with-param name="node" select="$section"/>
        <xsl:with-param name="conditional" select="0"/>
      </xsl:call-template>
    </xsl:if>
    <xsl:copy-of select="$title"/>
    <!-- pgsql-docs: begin -->
    <xsl:call-template name="pg.id.link">
      <xsl:with-param name="object" select="$section"/>
    </xsl:call-template>
    <!-- pgsql-docs: end -->
  </xsl:element>
</xsl:template>


<!-- Add an id link after the last term of a varlistentry. -->

<!-- overrides html/lists.xsl -->
<xsl:template match="varlistentry/term">
  <xsl:apply-imports/>

  <!-- Add the link after the last term -->
  <xsl:if test="position() = last()">
    <xsl:call-template name="pg.id.link">
      <xsl:with-param name="object" select="parent::varlistentry"/>
    </xsl:call-template>
  </xsl:if>
</xsl:template>


<!-- Create a link pointing to an id within the document -->
<xsl:template name="pg.id.link">
  <xsl:param name="object" select="."/>
  <xsl:choose>
    <xsl:when test="$object/@id or $object/@xml:id">
      <xsl:text> </xsl:text>
      <a>
        <xsl:attribute name="href">
          <xsl:text>#</xsl:text>
          <xsl:call-template name="object.id">
            <xsl:with-param name="object" select="$object"/>
          </xsl:call-template>
        </xsl:attribute>
        <xsl:attribute name="class">
          <xsl:text>id_link</xsl:text>
        </xsl:attribute>
        <xsl:text>#</xsl:text>
      </a>
    </xsl:when>
    <xsl:otherwise>
      <!-- Only complain about varlistentries if at least one entry in
           the list has an id -->
      <xsl:if test="name($object) != 'varlistentry'
                    or $object/parent::variablelist/varlistentry[@id]">
        <xsl:message terminate="yes">
          <xsl:text>ERROR: id attribute missing on &lt;</xsl:text>
          <xsl:value-of select ="name($object)"/>
          <xsl:text>&gt; element under </xsl:text>
          <xsl:for-each select="$object/ancestor::*">
            <xsl:text>/</xsl:text>
            <xsl:value-of select ="name(.)"/>
            <xsl:if test="@id|@xml:id">
              <xsl:text>[@</xsl:text>
              <xsl:value-of select ="name(@id|@xml:id)"/>
              <xsl:text> = '</xsl:text>
              <xsl:value-of select ="@id"/>
              <xsl:text>']</xsl:text>
            </xsl:if>
          </xsl:for-each>
        </xsl:message>
      </xsl:if>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<!-- make generated ids reproducible
     (https://github.com/docbook/xslt10-stylesheets/issues/54) -->

<!-- from html/autoidx.xsl -->

<xsl:key name="primaryonly"
         match="indexterm"
         use="normalize-space(primary)"/>

<xsl:template match="indexterm" mode="index-primary">
  <xsl:param name="scope" select="."/>
  <xsl:param name="role" select="''"/>
  <xsl:param name="type" select="''"/>

  <xsl:variable name="key" select="&primary;"/>
  <xsl:variable name="refs" select="key('primary', $key)[&scope;]"/>
  <dt>
    <xsl:if test="$autolink.index.see != 0">
      <!-- add internal id attribute to form see and seealso links -->
      <xsl:attribute name="id">
        <!-- pgsql-docs: begin -->
        <xsl:text>ientry-</xsl:text>
        <xsl:call-template name="object.id"/>
        <!-- pgsql-docs: end -->
      </xsl:attribute>
    </xsl:if>
    <xsl:for-each select="$refs/primary">
      <xsl:if test="@id or @xml:id">
        <xsl:choose>
          <xsl:when test="$generate.id.attributes = 0">
            <a name="{(@id|@xml:id)[1]}"/>
          </xsl:when>
          <xsl:otherwise>
            <span>
              <xsl:call-template name="id.attribute"/>
            </span>
          </xsl:otherwise>
        </xsl:choose>
      </xsl:if>
    </xsl:for-each>
    <xsl:value-of select="primary"/>
    <xsl:choose>
      <xsl:when test="$index.links.to.section = 1">
        <xsl:for-each select="$refs[@zone != '' or generate-id() = generate-id(key('primary-section', concat($key, &sep;, &section.id;))[&scope;][1])]">
          <xsl:apply-templates select="." mode="reference">
            <xsl:with-param name="position" select="position()"/>
            <xsl:with-param name="scope" select="$scope"/>
            <xsl:with-param name="role" select="$role"/>
            <xsl:with-param name="type" select="$type"/>
          </xsl:apply-templates>
        </xsl:for-each>
      </xsl:when>
      <xsl:otherwise>
        <xsl:for-each select="$refs[not(see)
                              and not(secondary)][&scope;]">
          <xsl:apply-templates select="." mode="reference">
            <xsl:with-param name="position" select="position()"/>
            <xsl:with-param name="scope" select="$scope"/>
            <xsl:with-param name="role" select="$role"/>
            <xsl:with-param name="type" select="$type"/>
          </xsl:apply-templates>
        </xsl:for-each>
      </xsl:otherwise>
    </xsl:choose>

    <xsl:if test="$refs[not(secondary)]/*[self::see]">
      <xsl:apply-templates select="$refs[generate-id() = generate-id(key('see', concat(&primary;, &sep;, &sep;, &sep;, see))[&scope;][1])]"
                           mode="index-see">
        <xsl:with-param name="position" select="position()"/>
        <xsl:with-param name="scope" select="$scope"/>
        <xsl:with-param name="role" select="$role"/>
        <xsl:with-param name="type" select="$type"/>
        <xsl:sort select="translate(see, &lowercase;, &uppercase;)"/>
      </xsl:apply-templates>
    </xsl:if>
  </dt>
  <xsl:choose>
    <xsl:when test="$refs/secondary or $refs[not(secondary)]/*[self::seealso]">
      <dd>
        <dl>
          <xsl:apply-templates select="$refs[generate-id() = generate-id(key('see-also', concat(&primary;, &sep;, &sep;, &sep;, seealso))[&scope;][1])]"
                               mode="index-seealso">
            <xsl:with-param name="position" select="position()"/>
            <xsl:with-param name="scope" select="$scope"/>
            <xsl:with-param name="role" select="$role"/>
            <xsl:with-param name="type" select="$type"/>
            <xsl:sort select="translate(seealso, &lowercase;, &uppercase;)"/>
          </xsl:apply-templates>
          <xsl:apply-templates select="$refs[secondary and count(.|key('secondary', concat($key, &sep;, &secondary;))[&scope;][1]) = 1]"
                               mode="index-secondary">
            <xsl:with-param name="position" select="position()"/>
            <xsl:with-param name="scope" select="$scope"/>
            <xsl:with-param name="role" select="$role"/>
            <xsl:with-param name="type" select="$type"/>
            <xsl:sort select="translate(&secondary;, &lowercase;, &uppercase;)"/>
          </xsl:apply-templates>
        </dl>
      </dd>
    </xsl:when>
    <!-- HTML5 requires dd for each dt -->
    <xsl:when test="$div.element = 'section'">
      <dd></dd>
    </xsl:when>
  </xsl:choose>
</xsl:template>

<xsl:template match="indexterm" mode="index-see" xmlns:xlink='http://www.w3.org/1999/xlink'>
  <xsl:param name="scope" select="."/>
  <xsl:param name="role" select="''"/>
  <xsl:param name="type" select="''"/>

  <xsl:variable name="see" select="normalize-space(see)"/>

  <!-- can only link to primary, which should appear before comma
  in see "primary, secondary" entry -->
  <xsl:variable name="seeprimary">
    <xsl:choose>
      <xsl:when test="contains($see, ',')">
        <xsl:value-of select="substring-before($see, ',')"/>
      </xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="$see"/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:variable>

  <xsl:variable name="seetarget" select="key('primaryonly', $seeprimary)[1]"/>

  <xsl:variable name="linkend">
    <xsl:if test="$seetarget">
      <!-- pgsql-docs: begin -->
      <xsl:text>#ientry-</xsl:text>
      <xsl:call-template name="object.id">
        <xsl:with-param name="object" select="$seetarget"/>
      </xsl:call-template>
      <!-- pgsql-docs: end -->
    </xsl:if>
  </xsl:variable>

  <xsl:text> (</xsl:text>
  <xsl:call-template name="gentext">
    <xsl:with-param name="key" select="'see'"/>
  </xsl:call-template>
  <xsl:text> </xsl:text>
  <xsl:choose>
    <!-- manual links have precedence -->
    <xsl:when test="see/@linkend or see/@xlink:href">
      <xsl:call-template name="simple.xlink">
        <xsl:with-param name="node" select="see"/>
        <xsl:with-param name="content" select="$see"/>
      </xsl:call-template>
    </xsl:when>
    <xsl:when test="$autolink.index.see = 0">
      <xsl:value-of select="$see"/>
    </xsl:when>
    <xsl:when test="$seetarget">
      <a href="{$linkend}">
        <xsl:value-of select="$see"/>
      </a>
    </xsl:when>
    <xsl:otherwise>
      <xsl:value-of select="$see"/>
    </xsl:otherwise>
  </xsl:choose>
  <xsl:text>)</xsl:text>
</xsl:template>

<xsl:template match="indexterm" mode="index-seealso" xmlns:xlink='http://www.w3.org/1999/xlink'>
  <xsl:param name="scope" select="."/>
  <xsl:param name="role" select="''"/>
  <xsl:param name="type" select="''"/>

  <xsl:for-each select="seealso">
    <xsl:sort select="translate(., &lowercase;, &uppercase;)"/>

    <xsl:variable name="seealso" select="normalize-space(.)"/>

    <!-- can only link to primary, which should appear before comma
    in seealso "primary, secondary" entry -->
    <xsl:variable name="seealsoprimary">
      <xsl:choose>
        <xsl:when test="contains($seealso, ',')">
          <xsl:value-of select="substring-before($seealso, ',')"/>
        </xsl:when>
        <xsl:otherwise>
          <xsl:value-of select="$seealso"/>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:variable>

    <xsl:variable name="seealsotarget" select="key('primaryonly', $seealsoprimary)[1]"/>

    <xsl:variable name="linkend">
      <xsl:if test="$seealsotarget">
        <!-- pgsql-docs: begin -->
        <xsl:text>#ientry-</xsl:text>
        <xsl:call-template name="object.id">
          <xsl:with-param name="object" select="$seealsotarget"/>
        </xsl:call-template>
        <!-- pgsql-docs: end -->
      </xsl:if>
    </xsl:variable>

    <dt>
      <xsl:text>(</xsl:text>
      <xsl:call-template name="gentext">
        <xsl:with-param name="key" select="'seealso'"/>
      </xsl:call-template>
      <xsl:text> </xsl:text>
      <xsl:choose>
        <!-- manual links have precedence -->
        <xsl:when test="@linkend or see/@xlink:href">
          <xsl:call-template name="simple.xlink">
            <xsl:with-param name="node" select="."/>
            <xsl:with-param name="content" select="$seealso"/>
          </xsl:call-template>
        </xsl:when>
        <xsl:when test="$autolink.index.see = 0">
          <xsl:value-of select="$seealso"/>
        </xsl:when>
        <xsl:when test="$seealsotarget">
          <a href="{$linkend}">
            <xsl:value-of select="$seealso"/>
          </a>
        </xsl:when>
        <xsl:otherwise>
          <xsl:value-of select="$seealso"/>
        </xsl:otherwise>
      </xsl:choose>
      <xsl:text>)</xsl:text>
    </dt>

    <xsl:if test="$div.element = 'section'">
      <dd></dd>
    </xsl:if>
  </xsl:for-each>
</xsl:template>

</xsl:stylesheet>
