<%@ Page Language="c#" AutoEventWireup="false" Codebehind="TableCell_ctor_.aspx.cs" Inherits="GHTTests.System_Web_dll.System_Web_UI_WebControls.TableCell_ctor_" %>
<%@ Register TagPrefix="cc1" Namespace="GHTWebControls" Assembly="MainsoftWebApp" %>
<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.0 Transitional//EN">
<HTML>
	<HEAD>
		<title>TableCell_ctor_</title>
		<meta content="Microsoft Visual Studio .NET 7.1" name="GENERATOR">
		<meta content="Visual Basic .NET 7.1" name="CODE_LANGUAGE">
		<meta content="JavaScript" name="vs_defaultClientScript">
		<meta content="http://schemas.microsoft.com/intellisense/ie5" name="vs_targetSchema">
		<script LANGUAGE="JavaScript">
        function ScriptTest()
        {
            var theform;
		    if (window.navigator.appName.toLowerCase().indexOf("netscape") > -1) {
			    theform = document.forms["Form1"];
		    }
		    else {
			    theform = document.Form1;
		    }
        }
		</script>
	</HEAD>
	<body>
		<form id="Form1" method="post" runat="server">
			<cc1:GHTSubTest id="GHTSubTest1" style="Z-INDEX: 100; LEFT: 16px; POSITION: absolute; TOP: 15px"
				runat="server" Width="432px" Height="152px">
				<asp:Table id="Table1" runat="server" Height="88px" Width="160px">
					<asp:TableRow>
						<asp:TableCell>AAAA</asp:TableCell>
						<asp:TableCell>BBBB</asp:TableCell>
					</asp:TableRow>
					<asp:TableRow>
						<asp:TableCell>CCCC</asp:TableCell>
						<asp:TableCell>DDDD</asp:TableCell>
						<asp:TableCell>1234</asp:TableCell>
						<asp:TableCell>5678</asp:TableCell>
					</asp:TableRow>
				</asp:Table>
			</cc1:GHTSubTest>
		</form>
		<br>
		<br>
	</body>
</HTML>
