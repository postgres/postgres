package uk.org.retep.util.proped;

import uk.org.retep.util.ExceptionDialog;
import uk.org.retep.util.misc.PropertiesIO;
import uk.org.retep.util.models.PropertiesTableModel;

import java.awt.*;
import java.io.*;
import java.util.*;
import javax.swing.*;
import java.awt.event.*;

/**
 * A property file editor
 *
 * $Id: PropertyEditor.java,v 1.1 2001/03/05 09:15:38 peter Exp $
 *
 * @author
 * @version 1.0
 */

public class PropertyEditor
extends JPanel
implements uk.org.retep.tools.Tool
{
  BorderLayout borderLayout1 = new BorderLayout();

  // The filename, null if not set
  String filename;
  File file;

  JScrollPane jScrollPane1 = new JScrollPane();
  JTable contentTable = new JTable();

  PropertiesTableModel model = new PropertiesTableModel();

  boolean standaloneMode;

  private static final String TITLE_PREFIX = "Retep PropertyEditor";
  JPopupMenu popupMenu = new JPopupMenu();
  JMenuItem newPopupItem = new JMenuItem();
  JMenuItem dupPopupItem = new JMenuItem();
  JMenuItem delPopupItem = new JMenuItem();
  JMenuBar menuBar = new JMenuBar();
  JMenu jMenu1 = new JMenu();
  JMenuItem jMenuItem4 = new JMenuItem();
  JMenuItem jMenuItem5 = new JMenuItem();
  JMenuItem jMenuItem6 = new JMenuItem();
  JMenuItem jMenuItem7 = new JMenuItem();
  JMenuItem jMenuItem8 = new JMenuItem();
  JMenuItem closeMenuItem = new JMenuItem();

  public PropertyEditor()
  {
    try
    {
      jbInit();
    }
    catch(Exception ex)
    {
      ex.printStackTrace();
    }
  }

  /**
   * @return the default menubar
   */
  public JMenuBar getMenuBar()
  {
    return menuBar;
  }

  /**
   * @return the File menu
   */
  public JMenu getMenu()
  {
    return jMenu1;
  }

  /**
   * @return the recomended title string for the parent JFrame/JInternalFrame
   */
  public String getTitle()
  {
    if(filename==null) {
      return TITLE_PREFIX;
    }
    return TITLE_PREFIX+": "+filename;
  }

  /**
   * Sets menus up to Standalone mode
   */
  public void setStandaloneMode(boolean aMode)
  {
    standaloneMode=aMode;
    if(aMode) {
      closeMenuItem.setText("Exit");
    } else {
      closeMenuItem.setText("Close");
    }
  }

  public boolean isStandalone()
  {
    return standaloneMode;
  }

  public void openFile(String aFile)
  throws IOException
  {
    openFile(new File(aFile));
  }

  public void openFile(File aFile)
  throws IOException
  {
    FileInputStream fis = new FileInputStream(aFile);
    Properties p = new Properties();
    p.load(fis);
    fis.close();
    model.setProperties(p);

    file=aFile;
    filename = aFile.getAbsolutePath();
  }

  public void saveFile(File aFile)
  throws IOException
  {
    FileOutputStream fis = new FileOutputStream(aFile);
    PropertiesIO.save(model.getProperties(),fis,'=',"Written by "+TITLE_PREFIX);
    fis.close();

    filename = aFile.getAbsolutePath();
    file = aFile;
  }

  void jbInit() throws Exception
  {
    this.setLayout(borderLayout1);
    contentTable.setToolTipText("");
    contentTable.setAutoResizeMode(JTable.AUTO_RESIZE_LAST_COLUMN);
    contentTable.setModel(model);
    contentTable.addMouseListener(new java.awt.event.MouseAdapter()
    {
      public void mouseClicked(MouseEvent e)
      {
        contentTable_mouseClicked(e);
      }
      public void mouseReleased(MouseEvent e)
      {
        contentTable_mouseReleased(e);
      }
    });
    newPopupItem.setText("New");
    newPopupItem.addActionListener(new java.awt.event.ActionListener()
    {
      public void actionPerformed(ActionEvent e)
      {
        newPopupItem_actionPerformed(e);
      }
    });
    dupPopupItem.setText("Duplicate");
    dupPopupItem.setAccelerator(javax.swing.KeyStroke.getKeyStroke(67, java.awt.event.KeyEvent.CTRL_MASK, false));
    dupPopupItem.addActionListener(new java.awt.event.ActionListener()
    {
      public void actionPerformed(ActionEvent e)
      {
        dupPopupItem_actionPerformed(e);
      }
    });
    delPopupItem.setText("Delete");
    delPopupItem.setAccelerator(javax.swing.KeyStroke.getKeyStroke(68, java.awt.event.KeyEvent.CTRL_MASK, false));
    delPopupItem.addActionListener(new java.awt.event.ActionListener()
    {
      public void actionPerformed(ActionEvent e)
      {
        delPopupItem_actionPerformed(e);
      }
    });
    jMenu1.setText("File");
    jMenuItem4.setText("Open");
    jMenuItem4.addActionListener(new java.awt.event.ActionListener()
    {
      public void actionPerformed(ActionEvent e)
      {
        jMenuItem4_actionPerformed(e);
      }
    });
    jMenuItem5.setText("Save");
    jMenuItem5.addActionListener(new java.awt.event.ActionListener()
    {
      public void actionPerformed(ActionEvent e)
      {
        jMenuItem5_actionPerformed(e);
      }
    });
    jMenuItem6.setText("Save As");
    jMenuItem6.addActionListener(new java.awt.event.ActionListener()
    {
      public void actionPerformed(ActionEvent e)
      {
        jMenuItem6_actionPerformed(e);
      }
    });
    jMenuItem7.setText("Revert");
    jMenuItem7.addActionListener(new java.awt.event.ActionListener()
    {
      public void actionPerformed(ActionEvent e)
      {
        jMenuItem7_actionPerformed(e);
      }
    });
    jMenuItem8.setText("Print");
    closeMenuItem.setText("Close");
    closeMenuItem.addActionListener(new java.awt.event.ActionListener()
    {
      public void actionPerformed(ActionEvent e)
      {
        closeMenuItem_actionPerformed(e);
      }
    });
    jMenu2.setText("Edit");
    jMenuItem1.setText("New");
    jMenuItem1.setAccelerator(javax.swing.KeyStroke.getKeyStroke(78, java.awt.event.KeyEvent.CTRL_MASK, false));
    jMenuItem1.addActionListener(new java.awt.event.ActionListener()
    {
      public void actionPerformed(ActionEvent e)
      {
        newPopupItem_actionPerformed(e);
      }
    });
    jMenuItem2.setText("Duplicate");
    jMenuItem3.setText("Delete");
    this.add(jScrollPane1, BorderLayout.CENTER);
    jScrollPane1.getViewport().add(contentTable, null);
    popupMenu.add(newPopupItem);
    popupMenu.add(dupPopupItem);
    popupMenu.add(delPopupItem);
    menuBar.add(jMenu1);
    menuBar.add(jMenu2);
    jMenu1.add(jMenuItem4);
    jMenu1.add(jMenuItem5);
    jMenu1.add(jMenuItem6);
    jMenu1.add(jMenuItem7);
    jMenu1.addSeparator();
    jMenu1.add(jMenuItem8);
    jMenu1.addSeparator();
    jMenu1.add(closeMenuItem);
    jMenu2.add(jMenuItem1);
    jMenu2.add(jMenuItem2);
    jMenu2.add(jMenuItem3);
  }

  Point popupPoint = new Point();
  JMenu jMenu2 = new JMenu();
  JMenuItem jMenuItem1 = new JMenuItem();
  JMenuItem jMenuItem2 = new JMenuItem();
  JMenuItem jMenuItem3 = new JMenuItem();
  void contentTable_mouseClicked(MouseEvent e)
  {
    if(e.isPopupTrigger()) {
      popupPoint.setLocation(e.getX(),e.getY());
      popupMenu.show(contentTable,e.getX(),e.getY());
    }
  }

  void contentTable_mouseReleased(MouseEvent e)
  {
    contentTable_mouseClicked(e);
  }

  void jMenuItem4_actionPerformed(ActionEvent e)
  {
    JFileChooser fc = new JFileChooser();
    if(fc.showOpenDialog(this) == JFileChooser.APPROVE_OPTION) {
      try {
        openFile(fc.getSelectedFile());
      } catch(IOException ioe) {
        ExceptionDialog.displayException(ioe);
      }
    }
  }

  void closeMenuItem_actionPerformed(ActionEvent e)
  {
    if(standaloneMode) {
      System.exit(0);
    } else {
      filename="";
      file=null;
      model.setProperties(new Properties());
    }
  }

  void newPopupItem_actionPerformed(ActionEvent e)
  {
    int y = contentTable.rowAtPoint(popupPoint);

    // create a new unique key based on the current one
    String key=(String) model.getValueAt(y,0);

    if(key==null) {
      key="new-key";
    }

    int uid=1;
    while(model.containsKey(key+uid)) {
      uid++;
    }

    key=key+uid;
    model.put(key,"");
    contentTable.clearSelection();
  }

  void dupPopupItem_actionPerformed(ActionEvent e)
  {
    int y = contentTable.rowAtPoint(popupPoint);

    // create a new unique key based on the current one
    String key=(String) model.getValueAt(y,0);
    Object val=model.get(key);

    int uid=1;
    while(model.containsKey(key+uid)) {
      uid++;
    }

    key=key+uid;
    model.put(key,val);
    contentTable.clearSelection();
  }

  void delPopupItem_actionPerformed(ActionEvent e)
  {
    int y = contentTable.rowAtPoint(popupPoint);
    model.remove(model.getValueAt(y,0));
  }

  void jMenuItem6_actionPerformed(ActionEvent e)
  {
    JFileChooser fc = new JFileChooser();
    if(fc.showSaveDialog(this) == JFileChooser.APPROVE_OPTION) {
      try {
        saveFile(fc.getSelectedFile());
      } catch(IOException ioe) {
        ExceptionDialog.displayException(ioe);
      }
    }
  }

  void jMenuItem5_actionPerformed(ActionEvent e)
  {
    if(filename==null) {
      jMenuItem6_actionPerformed(e);
    } else {
      try {
        saveFile(file);
      } catch(IOException ioe) {
        ExceptionDialog.displayException(ioe);
      }
    }
  }

  void jMenuItem7_actionPerformed(ActionEvent e)
  {
    // add check here
    if(file!=null) {
      try {
        openFile(file);
      } catch(IOException ioe) {
        ExceptionDialog.displayException(ioe);
      }
    } else {
      jMenuItem4_actionPerformed(e);
    }
  }
}