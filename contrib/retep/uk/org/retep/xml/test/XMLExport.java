package uk.org.retep.xml.test;

import java.lang.Exception;
import java.io.*;
import java.sql.*;
import java.util.Properties;
import uk.org.retep.xml.core.XMLFactoryException;
import uk.org.retep.xml.jdbc.XMLDatabase;
import uk.org.retep.xml.jdbc.XMLResultSet;

/**
 * This "test" class is a fully functional tool in its own right. It utilises
 * the xml classes to query and export to XML, or to dump database structures
 * into XML.
 */

public class XMLExport
{
  /**
   * The current Database Connection
   */
  protected Connection conn;
  protected Statement stat;
  protected String drvr,url,table;

  protected XMLResultSet xrs;
  protected XMLDatabase xdb;
  protected Properties prop;
  protected boolean outXML;
  protected boolean outDTD;
  protected boolean outTAB;
  protected int maxRows=0;

  public XMLExport(String[] args)
  throws IOException,SQLException,XMLFactoryException,ClassNotFoundException
  {
    xrs = new XMLResultSet();
    xrs.setWriter(new OutputStreamWriter(System.out));
    //Properties p = new Properties(xrs.getDefaultProperties());
    prop = (Properties) xrs.getDefaultProperties().clone();

    xdb = new XMLDatabase(xrs.getXMLFactory());

    for(int i=0;i<args.length;i++) {
      String arg=args[i];
      if(arg.startsWith("-D")) {
        // Load JDBC Driver
        drvr=arg.substring(2);
        Class.forName(drvr);
        System.out.println("Now using JDBC Driver: "+drvr);
      } else if(arg.startsWith("-J")) {
        // Open a JDBC Connection (closing the existing one, if any)
        close();
        url  = arg.substring(2);
        conn = DriverManager.getConnection(url);
        System.out.println("Opened "+url);
        stat=null;
      } else if(arg.startsWith("-M")) {
        // Set the maximum number of rows to process (0=no limit)
        maxRows=Integer.parseInt(arg.substring(2));
        if(maxRows<0)
          maxRows=0;
        prop.setProperty(XMLResultSet.FIRST_ROW,"0");
        prop.setProperty(XMLResultSet.LAST_ROW,Integer.toString(maxRows));
      } else if(arg.startsWith("-O")) {
        // Set the output file for XML & DTD
        xrs.setWriter(new FileWriter(arg.substring(2)));
        System.out.println("XML/DTD Output now going to "+arg.substring(2));
      } else if(arg.startsWith("-P")) {
        // Set a parameter for XML & DTD
        int p = arg.indexOf('=');
        prop.setProperty(arg.substring(2,p),arg.substring(p+1));
      } else if(arg.startsWith("-S")) {
        // -Stable generate schema of just table
        // -S generate schema of entire database
        if(arg.length()>2) {
          String table=arg.substring(2);
          System.out.println("Generating XML Schema of table "+table);
          xdb.writeTable(conn,table);
          xdb.close();
        } else {
          System.out.println("Generating XML Schema of database");
          xdb.writeDatabase(conn);
          xdb.close();
        }
      } else if(arg.equals("-V")) {
        // Select table output
        outXML=outDTD=false;
      } else if(arg.equals("-X")) {
        // Select XML output
        outXML=true;
        outDTD=outTAB=false;
      } else if(arg.equals("-Y")) {
        // Select DTD output
        outXML=outTAB=false;
        outDTD=true;
      } else if(arg.startsWith("-")) {
        System.err.println("Unknown argument: "+arg);
        System.exit(1);
      } else {
        // Ok, anything not starting with "-" are queries
        if(stat==null)
          stat=conn.createStatement();

        System.out.println("Executing "+arg);
        ResultSet rs = stat.executeQuery(arg);
        if(rs!=null) {
          if(outXML) {
            xrs.translate(rs,prop);
            xrs.close();
          } else if(outDTD) {
            // Output the DTD
            xrs.buildDTD(rs,prop);
            xrs.close();
          } else {
            // Normal resultset output
            int rc=0;

            ResultSetMetaData rsmd = rs.getMetaData();
            int nc = rsmd.getColumnCount();
            boolean us=false;
            for(int c=0;c<nc;c++) {
              if(us)
                System.out.print("\t");
              System.out.print(rsmd.getColumnName(c+1));
              us=true;
            }
            System.out.println();

            while(rs.next() && (maxRows==0 || rc<maxRows)) {
              us=false;
              for(int c=0;c<nc;c++) {
                if(us)
                  System.out.print("\t");
                System.out.print(rs.getString(c+1));
                us=true;
              }
              System.out.println();
              rc++;
            }

            System.out.println("Returned "+rc+" rows.");
          }
          rs.close();
        }
      }
    }

    close();
  }

  public void close() throws SQLException
  {
    if(conn!=null) {
      conn.close();
      System.out.println("Closed "+url);
      conn=null;
      stat=null;
    }
  }

  public static void main(String[] args)
  {
    if(args.length==0) {
      System.out.println("Useage: java uk.org.retep.xml.test.XMLExport [args ...]\nwhere args are:\n"+
        "-Dclass.name  JDBC Driver Class\n"+
        "-Jurl         JDBC URL\n"+
        "-Mmax         Maximum number of rows to process\n"+
        "-Ofilename    Send all XML or DTD output to file\n"+
        "-Pkey=value   Property passed on to XMLResultSet\n"+
        "-S[table]     Write XML description of table. Whole DB if table left out.\n"+
        "-V            Default: Write result to System.out\n"+
        "-X            Write result in XML to System.out\n"+
        "-Y            Write DTD describing result to System.out\n"+
        "\nAny other argument not starting with - is treated as an SQL Query\n"+
        "\nFor example:\n"+
        "To dump the table structure of a database into db.xml, use\n   $ java uk.org.retep.xml.test.XMLExport -Doracle.jdbc.driver.OracleDriver -Jjdbc:oracle:thin:dbname/username@localhost:1521:ISORCL -Odb.xml -S\n"+
        "To dump the structure of a single table PRODUCTS and write into products.xml, use\n   $ clear;java uk.org.retep.xml.test.XMLExport -Doracle.jdbc.driver.OracleDriver-Jjdbc:oracle:thin:dbname/username@localhost:1521:ISORCL -Oproducts.xml -SPRODUCT\n"+
        "To query a table and write the results into standard out as XML, use\n   $ java uk.org.retep.xml.test.XMLExport -Doracle.jdbc.driver.OracleDriver -Jjdbc:oracle:thin:dbname/username@localhost:1521:ISORCL -M5 -PSKU=row -PIMAGE=attribute -X \"select sku,image,template from product\"\n"+
        "To query a table and write a DTD describing the ResultSet, use\n   $ java uk.org.retep.xml.test.XMLExport -Doracle.jdbc.driver.OracleDriver -Jjdbc:oracle:thin:dbname/username@localhost:1521:ISORCL -M5 -PSKU=row -PIMAGE=attribute -Y \"select sku,image,template from product\"\n"
      );
      System.exit(1);
    }

    try {
      XMLExport XMLExport1 = new XMLExport(args);
    } catch(Exception e) {
      e.printStackTrace();
    }
  }
}