
using System;

public class TestObjAddr {
	public static void Main ()
	{
		object o = new object ();
		IntPtr addr = ObjAddr (o);
		Console.WriteLine ("addr=0x" + addr.ToString ("X"));
	}
}

