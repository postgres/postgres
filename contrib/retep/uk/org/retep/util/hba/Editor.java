package uk.org.retep.util.hba;

import uk.org.retep.tools.Tool;
import uk.org.retep.util.models.HBATableModel;

import java.awt.*;
import java.io.*;
import java.util.*;
import javax.swing.table.*;
import javax.swing.*;

/**
 * pg_hba.conf editor (& repairer)
 *
 * @author
 * @version 1.0
 */

public class Editor extends JPanel implements Tool
{
  BorderLayout borderLayout1 = new BorderLayout();
  HBATableModel model = new HBATableModel();
  JPanel jPanel1 = new JPanel();
  GridBagLayout gridBagLayout1 = new GridBagLayout();
  JLabel jLabel1 = new JLabel();
  JComboBox typeEntry = new JComboBox();
  JLabel jLabel2 = new JLabel();
  JLabel jLabel3 = new JLabel();
  JLabel jLabel4 = new JLabel();
  JTextField ipEntry = new JTextField();
  JTextField maskEntry = new JTextField();
  JComboBox authEntry = new JComboBox();
  JTextField argEntry = new JTextField();
  JLabel jLabel5 = new JLabel();
  JPanel jPanel2 = new JPanel();
  FlowLayout flowLayout1 = new FlowLayout();
  JButton jButton1 = new JButton();
  JButton jButton2 = new JButton();
  JScrollPane jScrollPane1 = new JScrollPane();
  JButton jButton3 = new JButton();
  JTable jTable1 = new JTable();

  public Editor()
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
  void jbInit() throws Exception
  {
    this.setLayout(borderLayout1);
    jTable1.setPreferredSize(new Dimension(600, 300));
    jTable1.setModel(model);
    this.setPreferredSize(new Dimension(600, 300));
    this.add(jScrollPane1, BorderLayout.CENTER);
    jScrollPane1.getViewport().add(jTable1, null);
    jPanel1.setLayout(gridBagLayout1);
    jLabel1.setText("Type");
    jLabel2.setText("IP Address");
    jLabel3.setText("Mask");
    jLabel4.setText("Authentication");
    ipEntry.setText("jTextField1");
    maskEntry.setText("jTextField2");
    argEntry.setText("jTextField3");
    jLabel5.setText("Argument");
    jPanel2.setLayout(flowLayout1);
    jButton1.setText("New entry");
    jButton2.setText("Validate");
    jButton3.setText("Devele");
    this.add(jPanel1, BorderLayout.SOUTH);
    jPanel1.add(jLabel1, new GridBagConstraints(1, 0, 1, 1, 0.0, 0.0
            ,GridBagConstraints.EAST, GridBagConstraints.NONE, new Insets(0, 0, 0, 0), 0, 0));
    jPanel1.add(typeEntry, new GridBagConstraints(2, 0, 1, 1, 0.0, 0.0
            ,GridBagConstraints.CENTER, GridBagConstraints.NONE, new Insets(0, 0, 0, 0), 0, 0));
    jPanel1.add(jLabel2, new GridBagConstraints(1, 1, 1, 1, 0.0, 0.0
            ,GridBagConstraints.EAST, GridBagConstraints.NONE, new Insets(0, 0, 0, 0), 0, 0));
    jPanel1.add(jLabel3, new GridBagConstraints(3, 1, 1, 1, 0.0, 0.0
            ,GridBagConstraints.CENTER, GridBagConstraints.NONE, new Insets(0, 0, 0, 0), 0, 0));
    jPanel1.add(jLabel4, new GridBagConstraints(1, 2, 1, 1, 0.0, 0.0
            ,GridBagConstraints.EAST, GridBagConstraints.NONE, new Insets(0, 0, 0, 0), 0, 0));
    jPanel1.add(ipEntry, new GridBagConstraints(2, 1, 1, 1, 0.0, 0.0
            ,GridBagConstraints.WEST, GridBagConstraints.HORIZONTAL, new Insets(0, 0, 0, 0), 0, 0));
    jPanel1.add(maskEntry, new GridBagConstraints(4, 1, 1, 1, 0.0, 0.0
            ,GridBagConstraints.WEST, GridBagConstraints.HORIZONTAL, new Insets(0, 0, 0, 0), 0, 0));
    jPanel1.add(authEntry, new GridBagConstraints(2, 2, 1, 1, 0.0, 0.0
            ,GridBagConstraints.CENTER, GridBagConstraints.NONE, new Insets(0, 0, 0, 0), 0, 0));
    jPanel1.add(argEntry, new GridBagConstraints(4, 2, 1, 1, 0.0, 0.0
            ,GridBagConstraints.CENTER, GridBagConstraints.NONE, new Insets(0, 0, 0, 0), 0, 0));
    jPanel1.add(jLabel5, new GridBagConstraints(3, 2, 1, 1, 0.0, 0.0
            ,GridBagConstraints.CENTER, GridBagConstraints.NONE, new Insets(0, 0, 0, 0), 0, 0));
    jPanel1.add(jPanel2, new GridBagConstraints(0, 3, 5, 1, 0.0, 0.0
            ,GridBagConstraints.CENTER, GridBagConstraints.HORIZONTAL, new Insets(0, 0, 0, 0), 0, 0));
    jPanel2.add(jButton3, null);
    jPanel2.add(jButton1, null);
    jPanel2.add(jButton2, null);
  }

  public void openFile(String aFilename)
  throws IOException
  {
    FileInputStream fis = new FileInputStream(aFilename);
    BufferedReader br = new BufferedReader(new InputStreamReader(fis));
    ArrayList list = model.getArray();

    String s = br.readLine();
    while(s!=null) {
      if(s.startsWith("#")) {
        // ignore comments
      } else {
        Record rec = Record.parseLine(s);
        if(rec!=null) {
          rec.validate();
          list.add(rec);
        }
      }
      s=br.readLine();
    }

    model.fireTableDataChanged();
  }

  public JMenuBar getMenuBar()
  {
    return null;
  }

  public String getTitle()
  {
    return "pg_hba.conf Editor/Repair tool";
  }

  public void setStandaloneMode(boolean aMode)
  {
  }

}