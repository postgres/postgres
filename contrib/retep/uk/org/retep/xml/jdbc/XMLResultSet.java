package uk.org.retep.xml.jdbc;

import java.io.IOException;
import java.io.Writer;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.util.Properties;
import uk.org.retep.xml.core.XMLFactory;
import uk.org.retep.xml.core.XMLFactoryException;

/**
 * This class takes a java.sql.ResultSet object and generates an XML stream
 * based on it's contents.
 *
 * $Id: XMLResultSet.java,v 1.1 2001/01/23 10:22:20 peter Exp $
 */
public class XMLResultSet
{
  /**
   * The current ResultSet to process
   */
  protected ResultSet rs;

  /**
   * The XMLFactory being used by this instance
   */
  protected XMLFactory factory;

  /**
   * The default properties used when none are supplied by the user
   */
  protected static Properties defaults;

  /**
   * The default property name for defining the tag name used to define a
   * ResultSet
   */
  public static String RESULTSET_NAME = "resultset.name";

  /**
   * The default tag name for a resultset
   */
  public static String DEFAULT_RESULTSET_NAME = "RESULTSET";

  /**
   * The default property name for defining the tag name used to define a row
   */
  public static String ROW_NAME = "row.name";

  /**
   * The default tag name for a row
   */
  public static String DEFAULT_ROW_NAME = "RECORD";

  /**
   * The default tag name for a resultset
   */
  public static String COLNAME = ".name";

  /**
   * The value of the property (named as its related column) used to define
   * how the column is generated. This indicates that the columns data is
   * enclosed within a pair of tags, ie: &lt;id&gt;1234&lt;/id&gt;
   */
  public static String CONTENT = "content";

  /**
   * The value of the property (named as its related column) used to define
   * how the column is generated. This indicates that the columns data is
   * an attribute in the columns tag. ie: <id value="1234" />
   */
  public static String ATTRIBUTE = "attribute";

  /**
   * This is the default attribute name used when the ATTRIBUTE option is set.
   */
  public static String DEFAULT_ATTRIBUTE = "VALUE";

  /**
   * The value of the property (named as its related column) used to define
   * how the column is generated. This indicates that the columns data is
   * an attribute in the parent's tag. ie: <row id="1234" />
   */
  public static String ROW_ATTRIBUTE = "row";

  /**
   * This property name marks the begining row number within the ResultSet to
   * start processing.
   */
  public static String FIRST_ROW = "row.first";

  /**
   * This property name marks the last row number within the ResultSet to
   * end processing.
   */
  public static String LAST_ROW = "row.last";

  /**
   * Constructor
   */
  public XMLResultSet()
  {
    factory = new XMLFactory();
  }

  /**
   * Constructor
   */
  public XMLResultSet(ResultSet rs)
  {
    this();
    setResultSet(rs);
  }

  /**
   * Sets the ResultSet to use
   * @param rs ResultSet
   */
  public void setResultSet(ResultSet rs)
  {
    this.rs=rs;
  }

  /**
   * @return the current ResultSet
   *
   */
  public ResultSet getResultSet()
  {
    return rs;
  }

  /**
   * Sets the Writer to send all output to
   * @param out Writer
   * @throws IOException from XMLFactory
   * @see XMLFactory.setWriter
   */
  public void setWriter(Writer out)
  throws IOException
  {
    factory.setWriter(out);
  }

  /**
   * @return Writer output is going to
   */
  public Writer getWriter()
  {
    return factory.getWriter();
  }

  /**
   * @return XMLFactory being used
   */
  public XMLFactory getXMLFactory()
  {
    return factory;
  }

  /**
   * Flushes all output to the Writer
   * @throw IOException from Writer
   * @throw XMLFactoryException from XMLFactory
   */
  public void close()
  throws IOException, XMLFactoryException
  {
    factory.close();
  }

  /**
   * Returns the default properties used by translate() and buildDTD()
   * @return Properties default property settings
   */
  public static Properties getDefaultProperties()
  {
    if(defaults==null) {
      defaults=new Properties();
      defaults.setProperty(RESULTSET_NAME,DEFAULT_RESULTSET_NAME);
      defaults.setProperty(ROW_NAME,DEFAULT_ROW_NAME);
    }
    return defaults;
  }

  /**
   * This generates an XML version of a ResultSet sending it to the supplied
   * Writer.
   * @param rs ResultSet to convert
   * @param p Properties for the conversion
   * @param out Writer to send output to (replaces existing one)
   * @throws XMLFactoryException from XMLFactory
   * @throws IOException from Writer
   * @throws SQLException from ResultSet
   */
  public void translate(ResultSet rs,Properties p,Writer out)
  throws XMLFactoryException, IOException, SQLException
  {
    factory.setWriter(out);
    translate(rs,p);
  }

  /**
   * This generates an XML version of a ResultSet sending it to the supplied
   * Writer using a default tag struct
   * @param rs ResultSet to convert
   * @param out Writer to send output to (replaces existing one)
   * @throws XMLFactoryException from XMLFactory
   * @throws IOException from Writer
   * @throws SQLException from ResultSet
   */
  public void translate(ResultSet rs,Writer out)
  throws XMLFactoryException, IOException, SQLException
  {
    factory.setWriter(out);
    translate(rs,(Properties)null);
  }

  /**
   * This generates an XML version of a ResultSet sending it to the current
   * output stream using a default tag structure.
   * @param rs ResultSet to convert
   * @throws XMLFactoryException from XMLFactory
   * @throws IOException from Writer
   * @throws SQLException from ResultSet
   */
  public void translate(ResultSet rs)
  throws XMLFactoryException, IOException, SQLException
  {
    translate(rs,(Properties)null);
  }

  /**
   * This generates an XML version of a ResultSet sending it to the current
   * output stream.
   * @param rs ResultSet to convert
   * @param p Properties for the conversion
   * @throws XMLFactoryException from XMLFactory
   * @throws IOException from Writer
   * @throws SQLException from ResultSet
   */
  public void translate(ResultSet rs,Properties p)
  throws XMLFactoryException, IOException, SQLException
  {
    // if we don't pass any properties, create an empty one and cache it if
    // further calls do the same
    if(p==null) {
      p=getDefaultProperties();
    }

    // Fetch some common values
    String setName = p.getProperty(RESULTSET_NAME,DEFAULT_RESULTSET_NAME);
    String rowName = p.getProperty(ROW_NAME,DEFAULT_ROW_NAME);

    ResultSetMetaData rsmd = rs.getMetaData();
    int numcols = rsmd.getColumnCount();

    String colname[] = new String[numcols];   // field name cache
    int    coltype[] = new int[numcols];  // true to use attribute false content
    String colattr[] = new String[numcols];   // Attribute name

    // These deal with when an attribute is to go into the row's tag parameters
    int parentFields[] = getRowAttributes(numcols,colname,colattr,coltype,rsmd,p); // used to cache the id's
    int numParents= parentFields==null ? 0 : parentFields.length;            // number of parent fields
    boolean haveParent= numParents>0;                 // true only if we need to us these

    // This allows some limiting of the output result
    int firstRow = Integer.parseInt(p.getProperty(FIRST_ROW,"0"));
    int lastRow = Integer.parseInt(p.getProperty(LAST_ROW,"0"));
    int curRow=0;

    // Start the result set's tag
    factory.startTag(setName);

    while(rs.next()) {
     if(firstRow<=curRow && (lastRow==0 || curRow<lastRow)) {
      factory.startTag(rowName);

      if(haveParent) {
        // Add any ROW_ATTRIBUTE entries
        for(int i=0;i<numParents;i++)
          factory.addAttribute(colname[i],rs.getString(i+1));
      }

      // Process any CONTENT & ATTRIBUTE entries.
      // This skips if all the entries are ROW_ATTRIBUTE's
      if(numParents < numcols) {
        for(int i=1;i<=numcols;i++) {
          // Now do we write the value as an argument or as PCDATA?
          switch(coltype[i-1]) {
            case 1:
              factory.startTag(colname[i-1]);
              factory.addAttribute(colattr[i-1],rs.getString(i));
              factory.endTag();
              break;

            case 0:
              factory.startTag(colname[i-1]);
              factory.addContent(rs.getString(i));
              factory.endTag();
              break;

            default:
              // Unknown type. This should only be called for ROW_ATTRIBUTE which
              // is handled before this loop.
              break;
          }
        }
      }

      // End the row
      factory.endTag();
    }
   curRow++;

   } // check for firstRow <= curRow <= lastRow

   // Close the result set's tag
   factory.endTag();
  }

  /**
   * This method takes a ResultSet and writes it's DTD to the current writer
   * @param rs ResultSet
   */
  public void buildDTD(ResultSet rs)
  throws IOException, SQLException
  {
    buildDTD(rs,null,getWriter());
  }

  /**
   * This method takes a ResultSet and writes it's DTD to the current writer
   * @param rs ResultSet
   * @param out Writer to send output to
   */
  public void buildDTD(ResultSet rs,Writer out)
  throws IOException, SQLException
  {
    buildDTD(rs,null,out);
  }

  /**
   * This method takes a ResultSet and writes it's DTD to the current writer
   * @param rs ResultSet
   * @param out Writer to send output to
   */
  public void buildDTD(ResultSet rs,Properties p)
  throws IOException, SQLException
  {
    buildDTD(rs,p,getWriter());
  }

  /**
   * This method takes a ResultSet and writes it's DTD to the current a.
   *
   * <p>ToDo:<ol>
   * <li>Add ability to have NULLABLE columns appear as optional (ie instead of
   * x, have x? (DTD for Optional). Can't use + or * as that indicates more than
   * 1 instance).
   * </ol>
   *
   * @param rs ResultSet
   * @param p Properties defining tag types (as translate)
   * @param out Writer to send output to
   */
  public void buildDTD(ResultSet rs,Properties p,Writer out)
  throws IOException, SQLException
  {
    // if we don't pass any properties, create an empty one and cache it if
    // further calls do the same
    if(p==null) {
      p=getDefaultProperties();
    }

    // Fetch some common values
    String setName = p.getProperty(RESULTSET_NAME,DEFAULT_RESULTSET_NAME);
    String rowName = p.getProperty(ROW_NAME,DEFAULT_ROW_NAME);

    ResultSetMetaData rsmd = rs.getMetaData();
    int numcols = rsmd.getColumnCount();

    String colname[] = new String[numcols];   // field name cache
    int    coltype[] = new int[numcols];  // true to use attribute false content
    String colattr[] = new String[numcols];   // Attribute name

    // These deal with when an attribute is to go into the row's tag parameters
    int parentFields[] = getRowAttributes(numcols,colname,colattr,coltype,rsmd,p); // used to cache the id's
    int numParents= parentFields==null ? 0 : parentFields.length;            // number of parent fields
    boolean haveParent= numParents>0;                 // true only if we need to us these

    // Now the dtd defining the ResultSet
    out.write("<!ELEMENT ");
    out.write(setName);
    out.write(" (");
    out.write(rowName);
    out.write("*)>\n");

    // Now the dtd defining each row
    out.write("<!ELEMENT ");
    out.write(rowName);
    out.write(" (");
    boolean s=false;
    for(int i=0;i<numcols;i++) {
      if(coltype[i]!=2) { // not ROW_ATTRIBUTE
        if(s)
          out.write(",");
        out.write(colname[i]);
        s=true;
      }
    }
    out.write(")>\n");

    // Now handle any ROW_ATTRIBUTE's
    if(haveParent) {
      out.write("<!ATTLIST ");
      out.write(rowName);
      for(int i=0;i<numParents;i++) {
        out.write("\n ");
        out.write(colname[parentFields[i]]);
        out.write(" CDATA #IMPLIED");
      }
      out.write("\n>\n");
    }

    // Now add any CONTENT & ATTRIBUTE fields
    for(int i=0;i<numcols;i++) {
      if(coltype[i]!=2) {
        out.write("<!ELEMENT ");
        out.write(colname[i]);

        // CONTENT
        if(coltype[i]==0) {
          out.write(" (#PCDATA)");
        } else {
          out.write(" EMPTY");
        }

        out.write(">\n");

        // ATTRIBUTE
        if(coltype[i]==1) {
          out.write("<!ATTLIST ");
          out.write(colname[i]);
          out.write("\n ");
          out.write(colattr[i]);
          out.write(" CDATA #IMPLIED\n>\n");
        }
      }
    }
  }

   /**
    * Private method used by the core translate and buildDTD methods.
    * @param numcols Number of columns in ResultSet
    * @param colname Array of column names
    * @param colattr Array of column attribute names
    * @param coltype Array of column types
    * @param rsmd ResultSetMetaData for ResultSet
    * @param p Properties being used
    * @return array containing field numbers which should appear as attributes
    * within the rows tag.
    * @throws SQLException from JDBC
    */
  private int[] getRowAttributes(int numcols,
      String colname[],String colattr[],
      int coltype[],
      ResultSetMetaData rsmd,Properties p)
  throws SQLException
  {
    int pf[] = null;
    int nf = 0;

    // Now we put a columns value as an attribute if the property
    // fieldname=attribute (ie myname=attribute)
    // and if the fieldname.name property exists, use it as the attribute name
    for(int i=0;i<numcols;i++) {
      colname[i] = rsmd.getColumnName(i+1);
      colattr[i] = p.getProperty(colname[i]+COLNAME,DEFAULT_ATTRIBUTE);
      if(p.getProperty(colname[i],CONTENT).equals(ROW_ATTRIBUTE)) {
        // Ok, ROW_ATTRIBUTE's need to be cached, so add them in here
        coltype[i]=2;
        if(pf==null) {
          pf = new int[numcols]; // Max possible number of entries
        }
        pf[nf++] = i;
      } else {
        // Normal CONTENT or ATTRIBUTE entry
        coltype[i] = p.getProperty(colname[i],CONTENT).equals(ATTRIBUTE) ? 1 : 0;
      }
    }

    // Form an array exactly nf elements long
    if(nf>0) {
      int r[] = new int[nf];
      System.arraycopy(pf,0,r,0,nf);
      return r;
    }

    // Return null if no tags are to appear as attributes to the row's tag
    return null;
  }

}