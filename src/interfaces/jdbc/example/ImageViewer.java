package example;

import java.awt.*;
import java.awt.event.*;
import java.io.*;
import java.sql.*;
import org.postgresql.largeobject.*;

/**
 * This example is a small application that stores and displays images
 * held on a postgresql database.
 *
 * Before running this application, you need to create a database, and
 * on the first time you run it, select "Initialise" in the "PostgreSQL"
 * menu.
 *
 * Important note: You will notice we import the org.postgresql.largeobject
 * package, but don't import the org.postgresql package. The reason for this is
 * that importing postgresql can confuse javac (we have conflicting class names
 * in org.postgresql.* and java.sql.*). This doesn't cause any problems, as
 * long as no code imports org.postgresql.
 *
 * Under normal circumstances, code using any jdbc driver only needs to import
 * java.sql, so this isn't a problem.
 *
 * It's only if you use the non jdbc facilities, do you have to take this into
 * account.
 *
 * Note: For PostgreSQL 6.4, the driver is now Thread safe, so this example
 * application has been updated to use multiple threads, especially in the
 * image storing and retrieving methods.
 */

public class ImageViewer implements ItemListener
{
  Connection db;
  Statement stat;
  LargeObjectManager lom;
  Frame frame;
  Label label;		// Label used to display the current name
  List list;		// The list of available images
  imageCanvas canvas;	// Canvas used to display the image
  String currentImage;	// The current images name
  
  // This is a simple component to display our image
  public class imageCanvas extends Canvas
  {
    // holds the image
    private Image image;
    
    // holds the background buffer
    private Image bkg;
    
    // the size of the buffer
    private Dimension size;
    
    public imageCanvas()
    {
      image=null;
    }
    
    public void setImage(Image img)
    {
      image=img;
      repaint();
    }
    
    // This defines our minimum size
    public Dimension getMinimumSize()
    {
      return new Dimension(400,400);
    }
    
    public Dimension getPreferedSize()
    {
      return getMinimumSize();
    }
    
    public void update(Graphics g)
    {
      paint(g);
    }
    
    /**
     * Paints the image, using double buffering to prevent screen flicker
     */
    public void paint(Graphics gr)
    {
      Dimension s = getSize();
      
      if(size==null || bkg==null || !s.equals(size)) {
	size = s;
	bkg = createImage(size.width,size.height);
      }
      
      // now set the background
      Graphics g = bkg.getGraphics();
      g.setColor(Color.gray);
      g.fillRect(0,0,s.width,s.height);
      
      // now paint the image over the background
      if(image!=null)
	g.drawImage(image,0,0,this);
      
      // dispose the graphics instance
      g.dispose();
      
      // paint the image onto the component
      gr.drawImage(bkg,0,0,this);
      
    }
    
  }
  
  public ImageViewer(Frame f,String url,String user,String password) throws ClassNotFoundException, FileNotFoundException, IOException, SQLException
  {
    frame = f;
    
    MenuBar mb = new MenuBar();
    Menu m;
    MenuItem i;
    
    f.setMenuBar(mb);
    mb.add(m = new Menu("PostgreSQL"));
    m.add(i= new MenuItem("Initialise"));
    i.addActionListener(new ActionListener() {
      public void actionPerformed(ActionEvent e) {
	ImageViewer.this.init();
      }
    });
    
    m.add(i= new MenuItem("Exit"));
    ActionListener exitListener = new ActionListener() {
      public void actionPerformed(ActionEvent e) {
	ImageViewer.this.close();
      }
    };
    m.addActionListener(exitListener);
    
    mb.add(m = new Menu("Image"));
    m.add(i= new MenuItem("Import"));
    ActionListener importListener = new ActionListener() {
      public void actionPerformed(ActionEvent e) {
	ImageViewer.this.importImage();
      }
    };
    i.addActionListener(importListener);
    
    m.add(i= new MenuItem("Remove"));
    ActionListener removeListener = new ActionListener() {
      public void actionPerformed(ActionEvent e) {
	ImageViewer.this.removeImage();
      }
    };
    i.addActionListener(removeListener);
    
    // To the north is a label used to display the current images name
    f.add("North",label = new Label());
    
    // We have a panel to the south of the frame containing the controls
    Panel p = new Panel();
    p.setLayout(new FlowLayout());
    Button b;
    p.add(b=new Button("Refresh List"));
    b.addActionListener(new ActionListener() {
      public void actionPerformed(ActionEvent e) {
	ImageViewer.this.refreshList();
      }
    });
    p.add(b=new Button("Import new image"));
    b.addActionListener(importListener);
    p.add(b=new Button("Remove image"));
    b.addActionListener(removeListener);
    p.add(b=new Button("Quit"));
    b.addActionListener(exitListener);
    f.add("South",p);
    
    // And a panel to the west containing the list of available images
    f.add("West",list=new List());
    list.addItemListener(this);
    
    // Finally the centre contains our image
    f.add("Center",canvas = new imageCanvas());
    
    // Load the driver
    Class.forName("org.postgresql.Driver");
    
    // Connect to database
    System.out.println("Connecting to Database URL = " + url);
    db = DriverManager.getConnection(url, user, password);
    
    // Create a statement
    stat = db.createStatement();
    
    // Set the connection to use transactions
    db.setAutoCommit(false);
    
    // Also, get the LargeObjectManager for this connection
    lom = ((org.postgresql.Connection)db).getLargeObjectAPI();
    
    // Now refresh the image selection list
    refreshList();
  }
  
  
  /**
   * This method initialises the database by creating a table that contains
   * the image names, and Large Object OID's
   */
  public void init()
  {
    try {
      stat.executeUpdate("create table images (imgname name,imgoid oid)");
      label.setText("Initialised database");
      db.commit();
    } catch(SQLException ex) {
      label.setText(ex.toString());
    }
  }
  
  /**
   * This closes the connection, and ends the application
   */
  public void close()
  {
    try {
      db.close();
    } catch(SQLException ex) {
      System.err.println(ex.toString());
    }
    System.exit(0);
  }
  
  /**
   * This imports an image into the database, using a Thread to do this in the
   * background.
   */
  public void importImage()
  {
    FileDialog d = new FileDialog(frame,"Import Image",FileDialog.LOAD);
    d.setVisible(true);
    String name = d.getFile();
    String dir = d.getDirectory();
    d.dispose();
    
    // now start the true importer
    Thread t = new importer(db,name,dir);
    //t.setPriority(Thread.MAX_PRIORITY);
    t.start();
  }
  
  /**
   * This is an example of using a thread to import a file into a Large Object.
   * It uses the Large Object extension, to write blocks of the file to the
   * database.
   */
  class importer extends Thread
  {
    String name,dir;
    Connection db;
    
    public importer(Connection db,String name,String dir) {
      this.db = db;
      this.name = name;
      this.dir = dir;
    }
    
    public void run() {
      
      // Now the real import stuff
      if(name!=null && dir!=null) {
	Statement stat = null;
	
	try {
	  // fetch the large object manager
	  LargeObjectManager lom = ((org.postgresql.Connection)db).getLargeObjectAPI();
	  
	  System.out.println("Importing file");
	  // A temporary buffer - this can be as large as you like
	  byte buf[] = new byte[2048];
	  
	  // Open the file
	  System.out.println("Opening file "+dir+"/"+name);
	  FileInputStream fis = new FileInputStream(new File(dir,name));
	  
	  // Gain access to large objects
	  System.out.println("Gaining LOAPI");
	  
	  // Now create the large object
	  System.out.println("creating blob");
	  int oid = lom.create();
	  
	  System.out.println("Opening "+oid);
	  LargeObject blob = lom.open(oid);
	  
	  // Now copy the file into the object.
	  //
	  // Note: we dont use write(buf), as the last block is rarely the same
	  // size as our buffer, so we have to use the amount read.
	  System.out.println("Importing file");
	  int s,t=0;
	  while((s=fis.read(buf,0,buf.length))>0) {
	    System.out.println("Block s="+s+" t="+t);t+=s;
	    blob.write(buf,0,s);
	  }
	  
	  // Close the object
	  System.out.println("Closing blob");
	  blob.close();
	  
	  // Now store the entry into the table
	  
	  // As we are a different thread to the other window, we must use
	  // our own thread
	  stat = db.createStatement();
	  stat.executeUpdate("insert into images values ('"+name+"',"+oid+")");
	  db.commit();
	  
	  // Finally refresh the names list, and display the current image
	  ImageViewer.this.refreshList();
	  ImageViewer.this.displayImage(name);
	} catch(Exception ex) {
	  label.setText(ex.toString());
	} finally {
	  // ensure the statement is closed after us
	  try {
	    if(stat != null)
	      stat.close();
	  } catch(SQLException se) {
	    System.err.println("closing of Statement failed");
	  }
	}
      }
    }
  }
  
  /**
   * This refreshes the list of available images
   */
  public void refreshList()
  {
    try {
      // First, we'll run a query, retrieving all of the image names
      ResultSet rs = stat.executeQuery("select imgname from images order by imgname");
      if(rs!=null) {
	list.removeAll();
	while(rs.next())
	  list.addItem(rs.getString(1));
	rs.close();
      }
    } catch(SQLException ex) {
      label.setText(ex.toString()+" Have you initialised the database?");
    }
  }
  
  /**
   * This removes an image from the database
   *
   * Note: With postgresql, this is the only way of deleting a large object
   * using Java.
   */
  public void removeImage()
  {
    try {
      // Delete any large objects for the current name
      ResultSet rs = stat.executeQuery("select imgoid from images where imgname='"+currentImage+"'");
      if(rs!=null) {
	// Even though there should only be one image, we still have to
	// cycle through the ResultSet
	while(rs.next()) {
	  System.out.println("Got oid "+rs.getInt(1));
	  lom.delete(rs.getInt(1));
	  System.out.println("Import complete");
	}
      }
      rs.close();
      
      // Finally delete any entries for that name
      stat.executeUpdate("delete from images where imgname='"+currentImage+"'");
      db.commit();
      
      label.setText(currentImage+" deleted");
      currentImage=null;
      db.commit();
      refreshList();
    } catch(SQLException ex) {
      label.setText(ex.toString());
    }
  }
  
  /**
   * This displays an image from the database.
   *
   * For images, this is the easiest method.
   */
  public void displayImage(String name)
  {
    try {
      System.out.println("Selecting oid for "+name);
      ResultSet rs = stat.executeQuery("select imgoid from images where imgname='"+name+"'");
      if(rs!=null) {
	// Even though there should only be one image, we still have to
	// cycle through the ResultSet
	while(rs.next()) {
	  System.out.println("Got oid "+rs.getInt(1));
	  canvas.setImage(canvas.getToolkit().createImage(rs.getBytes(1)));
	  System.out.println("Import complete");
	  label.setText(currentImage = name);
	}
      }
      rs.close();
    } catch(SQLException ex) {
      label.setText(ex.toString());
    }
  }
  
  public void itemStateChanged(ItemEvent e) {
    displayImage(list.getItem(((Integer)e.getItem()).intValue()));
  }
  
  /**
   * This is the command line instructions
   */
  public static void instructions()
  {
    System.err.println("java example.ImageViewer jdbc-url user password");
    System.err.println("\nExamples:\n");
    System.err.println("java -Djdbc.driver=org.postgresql.Driver example.ImageViewer jdbc:postgresql:test postgres password\n");
    
    System.err.println("This example tests the binary large object api of the driver.\nBasically, it will allow you to store and view images held in the database.");
    System.err.println("Note: If you are running this for the first time on a particular database,\nyou have to select \"Initialise\" in the \"PostgreSQL\" menu.\nThis will create a table used to store image names.");
  }
  
  /**
   * This is the application entry point
   */
  public static void main(String args[])
  {
    if(args.length!=3) {
      instructions();
      System.exit(1);
    }
    
    try {
      Frame frame = new Frame("PostgreSQL ImageViewer v7.0 rev 1");
      frame.setLayout(new BorderLayout());
      ImageViewer viewer = new ImageViewer(frame,args[0],args[1],args[2]);
      frame.pack();
      frame.setVisible(true);
    } catch(Exception ex) {
      System.err.println("Exception caught.\n"+ex);
      ex.printStackTrace();
    }
  }
}
