package uk.org.retep.util;

import uk.org.retep.util.StandaloneApp;
import javax.swing.JComponent;
import javax.swing.JLabel;

/**
 * This is a template for your own Tools. Copy not extend this class. Please
 * refer to Implementation for details.
 *
 * All you need to to is implement the init() method.
 *
 * $Id: Main.java,v 1.1 2001/03/05 09:15:36 peter Exp $
 */

public class Main extends StandaloneApp
{
  public Main(String[] args)
  throws Exception
  {
    super(args);
  }

  public JComponent init()
  throws Exception
  {
    // Create your tool here, then do things like load files based on the
    // command line arguments. Then return that tool.

    // NB: This just allows us to compile. You're implementation must return
    // the Tool itself.
    return new JLabel("Replace with your own tool!");
  }

  public static void main(String[] args)
  throws Exception
  {
    Main main = new Main(args);
    main.pack();
    main.setVisible(true);
  }
}
