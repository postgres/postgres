package uk.org.retep.dtu;

import uk.org.retep.xml.core.XMLFactory;
import uk.org.retep.xml.core.XMLFactoryException;

import java.io.IOException;

/**
 * This manages the links between two nodes.
 */
public class DTransform
{
  protected DNode from,to;
  protected int type;

  public DTransform()
  {
    this(null,null);
  }

  public DTransform(DNode aFrom,DNode aTo)
  {
    from=aFrom;
    to=aTo;
  }

  public int getType()
  {
    return type;
  }

  public void setType(int aType)
  {
    type=aType;
  }

  public void setFrom(DNode aNode)
  {
    if(from!=null) {
      from.removeTo(this);
    }

    from=aNode;
    from.setTo(this);
  }

  public DNode getFrom()
  {
    return from;
  }

  public void setTo(DNode aNode)
  {
    if(to!=null) {
      to.removeFrom(this);
    }

    to=aNode;
    aNode.setFrom(this);
  }

  public DNode getTo()
  {
    return to;
  }

  /**
   * This ensures the minimum tag/attributes are generated.
   * To extend, extend saveCustomXML() which is called by this method
   * appropriately.
   */
  public final void saveXML(XMLFactory aFactory)
  throws IOException, XMLFactoryException
  {
    // Bare minimum is the tag type, and the destination node's id
    aFactory.startTag(DConstants.XML_TRANSFORM);
    aFactory.addAttribute(DConstants.XML_TYPE,Integer.toString(getType()));
    aFactory.addAttribute(DConstants.XML_TO,Integer.toString(to.getID()));
    saveCustomXML(aFactory);
    aFactory.endTag();
  }

  /**
   * Custom transformations must overide this method and write direct to the
   * supplied XMLFactory. A tag is currently open when the method is called, but
   * is closed immediately this method exits.
   */
  public void saveCustomXML(XMLFactory aFactory)
  throws IOException, XMLFactoryException
  {
    // Default method does nothing...
  }

  /**
   * Checks to see if we should run based on the calling nodes status. Overide
   * this to add this sort of checking.
   *
   * @param status The return status of the calling node
   * @param env DEnvironment we are using
   * @return true if we will run.
   */
  public boolean willRun(int status,DEnvironment env)
  {
    switch(getType())
    {
      // NOP is the generic link - always run
      case DConstants.NOP:
        return true;

      // SUCCESS only runs if the previous node was OK
      case DConstants.SUCCESS:
        return status==DNode.OK;

      case DConstants.ERROR:
        return status==DNode.ERROR;

      // Default - always run. Overide the method if you need to change this
      default:
        return true;
    }
  }

  /**
   * Overide this for a transform to do something.
   * @param env DEnvironment we are using
   * @return true if we actually ran. DProcessor will jump to the getTo() node if so.
   */
  public boolean run(DEnvironment env)
  {
    return true;
  }

}