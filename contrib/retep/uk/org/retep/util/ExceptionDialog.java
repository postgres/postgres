package uk.org.retep.util;

import java.awt.*;
import javax.swing.*;
import java.awt.event.*;

/**
 * Display an Exception to the user
 * @author
 * @version 1.0
 */

public class ExceptionDialog extends JDialog
{
  // This is used to store the parent frame.
  // Classes like StandaloneApp set's this so that the
  // displayException() method can work without knowing/finding out
  // the parent Frame/JFrame.
  private static Frame globalFrame;

  private static ExceptionDialog globalDialog;

  JPanel panel1 = new JPanel();
  BorderLayout borderLayout1 = new BorderLayout();
  JTextArea message = new JTextArea();
  JPanel jPanel1 = new JPanel();
  JButton jButton1 = new JButton();
  GridLayout gridLayout1 = new GridLayout();
  JButton jButton2 = new JButton();
  JLabel jLabel1 = new JLabel();

  public ExceptionDialog(Frame frame, String title, boolean modal)
  {
    super(frame, title, modal);
    try
    {
      jbInit();
      pack();
    }
    catch(Exception ex)
    {
      ex.printStackTrace();
    }
  }

  public ExceptionDialog()
  {
    this(null, "", false);
  }
  void jbInit() throws Exception
  {
    panel1.setLayout(borderLayout1);
    message.setBorder(BorderFactory.createLoweredBevelBorder());
    message.setText("jTextArea1");
    message.setBackground(Color.lightGray);
    message.setEditable(false);
    jButton1.setText("Close");
    jButton1.addActionListener(new java.awt.event.ActionListener()
    {
      public void actionPerformed(ActionEvent e)
      {
        jButton1_actionPerformed(e);
      }
    });
    jPanel1.setLayout(gridLayout1);
    jButton2.setEnabled(false);
    jButton2.setText("Stack Trace");
    jLabel1.setEnabled(false);
    getContentPane().add(panel1);
    panel1.add(message, BorderLayout.CENTER);
    this.getContentPane().add(jPanel1, BorderLayout.SOUTH);
    jPanel1.add(jButton2, null);
    jPanel1.add(jLabel1, null);
    jPanel1.add(jButton1, null);
  }

  /**
   * Sets the Frame used to display all dialog boxes.
   */
  public static void setFrame(Frame aFrame)
  {
    globalFrame = aFrame;
  }

  /**
   * Displays a dialog based on the exception
   * @param ex Exception that was thrown
   */
  public static void displayException(Exception ex)
  {
    displayException(ex,null);
  }

  /**
   * Displays a dialog based on the exception
   * @param ex Exception that was thrown
   */
  public static void displayException(Exception ex,String msg)
  {
    String cname = ex.getClass().getName();
    int i=cname.lastIndexOf(".");
    displayException(cname.substring(i+1),ex,msg);
  }

  public static void displayException(String title,Exception ex)
  {
    displayException(title,ex,null);
  }

  public static void displayException(String title,Exception ex,String msg)
  {
    Logger.log(Logger.ERROR,title,ex.getMessage());

    // Default to a stack trace if no frame set
    if(globalFrame==null) {
      ex.printStackTrace();
      return;
    }

    if(globalDialog==null) {
      globalDialog=new ExceptionDialog(globalFrame,title,true);
      globalDialog.pack();
    }

    globalDialog.setTitle(title);

    if(msg!=null) {
      globalDialog.message.setText(msg);
      globalDialog.message.append(":\n");
    }
    globalDialog.message.append(ex.getMessage());

    globalDialog.pack();
    globalDialog.setVisible(true);
  }

  void jButton1_actionPerformed(ActionEvent e)
  {
    setVisible(false);
  }
}