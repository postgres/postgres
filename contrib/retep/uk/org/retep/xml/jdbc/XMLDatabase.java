package uk.org.retep.xml.jdbc;

import java.io.IOException;
import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.ResultSet;
import java.sql.SQLException;
import uk.org.retep.xml.core.XMLFactory;
import uk.org.retep.xml.core.XMLFactoryException;

public class XMLDatabase
{
  /**
   * The XMLFactory being used by this instance
   */
  protected XMLFactory factory;

  /**
   * Constructor. setXMLFactory() must be called if this method is used.
   */
  public XMLDatabase()
  {
  }

  /**
   * Constructor
   * @param fac XMLFactory to use
   */
  public XMLDatabase(XMLFactory fac)
  {
    this();
    setXMLFactory(fac);
  }

  /**
   * Sets the factory to use.
   * @param factory XMLFactory to use
   */
  public void setXMLFactory(XMLFactory factory)
  {
    this.factory=factory;
  }

  /**
   * @return the XMLFactory being used.
   */
  public XMLFactory getXMLFactory()
  {
    return factory;
  }

  /**
   * Flushes all output to the Writer.
   * @throw IOException from Writer
   * @throw XMLFactoryException from XMLFactory
   */
  public void close()
  throws IOException, XMLFactoryException
  {
    factory.close();
  }

  /**
   * writes the schema of a table.
   * @param con Connection to database
   * @param table Table name
   * @throw IOException from Writer
   * @throw SQLException from JDBC
   * @throw XMLFactoryException from XMLFactory
   */
  public void writeTable(Connection con,String table)
  throws IOException,SQLException,XMLFactoryException
  {
    writeTable(con.getMetaData(),table);
  }

  /**
   * writes the schema of a table.
   * @param db DatabaseMetaData for the database
   * @param table Table name
   * @throw IOException from Writer
   * @throw SQLException from JDBC
   * @throw XMLFactoryException from XMLFactory
   */
  public void writeTable(DatabaseMetaData db,String table)
  throws IOException,SQLException,XMLFactoryException
  {
    writeTable(db,null,null,table);
  }

  /**
   * writes the schema of a table.
   * @param db DatabaseMetaData for the database
   * @param table Table name
   * @throw IOException from Writer
   * @throw SQLException from JDBC
   * @throw XMLFactoryException from XMLFactory
   */
  public void writeTable(DatabaseMetaData db,String cat,String schem,String table)
  throws IOException,SQLException,XMLFactoryException
  {
    ResultSet trs;

    factory.startTag("TABLE");
    factory.addAttribute("NAME",table);
    // fetch the remarks for this table (if any)
    trs = db.getTables(null,null,table,null);
    if(trs!=null) {
      if(trs.next()) {
        String rem = trs.getString(5);
        if(rem!=null)
          factory.addContent(rem);
      }
      trs.close();
    }

    trs = db.getColumns(null,null,table,"%");
    if(trs!=null) {
      while(trs.next()) {
        factory.startTag("COLUMN");
        factory.addAttribute("NAME",trs.getString(4));
        factory.addAttribute("TYPE",trs.getString(6));
        factory.addAttribute("COLUMN_SIZE",trs.getString(7));
        factory.addAttribute("DECIMAL_DIGITS",trs.getString(9));
        factory.addAttribute("NUM_PREC_RADIX",trs.getString(10));
        factory.addAttribute("NULLABLE",trs.getString(11));
        factory.addAttribute("COLUMN_DEF",trs.getString(13));
        factory.addAttribute("CHAR_OCTET_LENGTH",trs.getString(16));
        factory.addAttribute("ORDINAL_POSITION",trs.getString(17));
        factory.addAttribute("IS_NULLABLE",trs.getString(18));
        factory.addAttribute("TABLE_CAT",trs.getString(1));
        factory.addAttribute("TABLE_SCHEM",trs.getString(2));
        String rem = trs.getString(12);
        if(rem!=null)
          factory.addContent(rem);
        factory.endTag();
      }
      trs.close();
    }

    factory.endTag();
  }

  /**
   * This generates the schema of an entire database.
   * @param db Connection to database
   * @param table Table pattern
   * @throw IOException from Writer
   * @throw SQLException from JDBC
   * @throw XMLFactoryException from XMLFactory
   * @see java.sql.DatabaseMetaData.getTables()
   */
  public void writeDatabase(Connection db,String table)
  throws IOException, SQLException, XMLFactoryException
  {
    writeDatabase(db.getMetaData(),null,null,table);
  }

  /**
   * This generates the schema of an entire database.
   * @param db DatabaseMetaData of database
   * @param table Table pattern
   * @throw IOException from Writer
   * @throw SQLException from JDBC
   * @throw XMLFactoryException from XMLFactory
   * @see java.sql.DatabaseMetaData.getTables()
   */
  public void writeDatabase(DatabaseMetaData db,String table)
  throws IOException, SQLException, XMLFactoryException
  {
    writeDatabase(db,null,null,table);
  }

  /**
   * This generates the schema of an entire database.
   * @param db DatabaseMetaData of database
   * @param cat Catalog (may be null)
   * @param schem Schema (may be null)
   * @param table Table pattern
   * @throw IOException from Writer
   * @throw SQLException from JDBC
   * @throw XMLFactoryException from XMLFactory
   * @see java.sql.DatabaseMetaData.getTables()
   */
  public void writeDatabase(Connection db)
  throws IOException, SQLException, XMLFactoryException
  {
    writeDatabase(db.getMetaData(),null,null,"%");
  }

  /**
   * This generates the schema of an entire database.
   * @param db DatabaseMetaData of database
   * @param cat Catalog (may be null)
   * @param schem Schema (may be null)
   * @param table Table pattern
   * @throw IOException from Writer
   * @throw SQLException from JDBC
   * @throw XMLFactoryException from XMLFactory
   * @see java.sql.DatabaseMetaData.getTables()
   */
  public void writeDatabase(DatabaseMetaData db)
  throws IOException, SQLException, XMLFactoryException
  {
    writeDatabase(db,null,null,"%");
  }

  /**
   * This generates the schema of an entire database.
   * @param db DatabaseMetaData of database
   * @param cat Catalog (may be null)
   * @param schem Schema (may be null)
   * @param table Table pattern
   * @throw IOException from Writer
   * @throw SQLException from JDBC
   * @throw XMLFactoryException from XMLFactory
   * @see java.sql.DatabaseMetaData.getTables()
   */
  public void writeDatabase(DatabaseMetaData db,String cat,String schem,String table)
  throws IOException, SQLException, XMLFactoryException
  {
    ResultSet rs = db.getTables(cat,schem,table,null);
    if(rs!=null) {
      factory.startTag("DATABASE");
      factory.addAttribute("PRODUCT",db.getDatabaseProductName());
      factory.addAttribute("VERSION",db.getDatabaseProductVersion());

      while(rs.next()) {
        writeTable(db,rs.getString(1),rs.getString(2),rs.getString(3));
      }

      factory.endTag();
      rs.close();
    }
  }

}