// PhonePort GPL-3.0-or-later. Model Context Protocol server over standard input and
// output. It adds no input path of its own: every action is built as a command batch and
// runs through Plan and Program.Execute, with the same identity and release checks.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

// A JSON-RPC protocol error (unknown method, unknown tool). A tool that fails while it runs
// returns a normal result with isError instead, so the agent can read why.
sealed class McpError : Exception {
    internal readonly int Code;
    internal McpError(int code,string message):base(message) { Code=code; }
}

sealed class McpServer {
    // Answered when the client asks for a protocol version this server does not know.
    internal const string Fallback="2025-06-18";
    static readonly string[] Versions={"2025-11-25","2025-06-18","2025-03-26","2024-11-05"};
    internal static readonly JavaScriptSerializer Json=new JavaScriptSerializer{MaxJsonLength=64*1024*1024};
    static readonly string[] BatchCommands={"tap","move","down","up","swipe","flick","home","wake","page","type","key","hold","wait","settle","changed"};
    // What the host's model is told once, at the start. The sentence about how input is
    // delivered differs by mode, because it decides what the model should tell the user.
    internal static string Instructions(bool foreground) {
        return
        "PhonePort mirrors a real iPhone in a window on this Windows PC and sends mouse and keyboard input to it over Bluetooth. "+
        "The phone has no accessibility tree: find targets by looking at the picture. "+
        "Work in a loop: call phoneport_screenshot, choose one visible target, act once, then read the picture the action returns before the next step. "+
        "Coordinates are pixels of the newest full screenshot, with 0,0 at the top left. "+
        "Never repeat an action because its result is unclear: look first. A tool that reports an error may still have sent input, and its result says so. "+
        "Typing needs a text field with a visible cursor and accepts US ASCII only. Home needs AssistiveTouch Button 2 mapped to Home on the iPhone. "+
        (foreground
            ?"Each input brings the PhonePort window to the front for a moment. If the user is working in another window, Windows refuses and no input is sent: ask the user to click the PhonePort window. "
            :"Input goes to the PhonePort window in the background: the window does not need to be in front, and the PC's pointer and keyboard are not used, so the user can keep working. "+
             "If a result says someone is using the phone, the user has taken it: the PhonePort window is in front and they are using the PC. Stop and ask before continuing. ")+
        "Ask the user before a step that sends a message, pays, deletes something or changes a setting. "+
        "After a tool error, call phoneport_status for the next setup step. Phone pictures can hold private information: keep them in this session.";
    }

    readonly int fixedPid,maxSide;
    readonly bool keepFocus,foreground;
    // Tests replace this to point the server at a pipe they serve themselves.
    internal Func<int> Resolve;

    internal McpServer(int pid,bool keep,int side,bool front=false) {
        fixedPid=pid;keepFocus=keep;maxSide=side;foreground=front;
        Resolve=()=>Doctor.Select(Receivers(),fixedPid);
    }
    static int[] Receivers() {
        var found=new List<int>();
        foreach(string name in Receiver.ProcessNames)foreach(var p in Process.GetProcessesByName(name))using(p)found.Add(p.Id);
        return found.ToArray();
    }
    static string Version() { return FileVersionInfo.GetVersionInfo(typeof(McpServer).Assembly.Location).ProductVersion; }
    // Set by the self-test, which provokes refusals on purpose.
    internal static bool Quiet;
    static void Log(string message) { if(!Quiet)Console.Error.WriteLine("phoneport mcp: "+message); }

    internal static int Run(string[] args) {
        int pid=0,side=1280;bool keep=false,front=false;
        for(int i=1;i<args.Length;i++) {
            // Keeping PhonePort in front only means something for foreground input, so it selects it.
            if(args[i]=="--keep-focus")keep=front=true;
            else if(args[i]=="--foreground")front=true;
            else if(args[i]=="--pid") { if(++i>=args.Length || !Int32.TryParse(args[i],out pid) || pid<1)throw new ArgumentException("Use --mcp --pid PID with one positive receiver PID"); }
            else if(args[i]=="--max-side") { if(++i>=args.Length || !Int32.TryParse(args[i],out side) || side<320 || side>4096)throw new ArgumentException("Use --mcp --max-side N with N from 320 to 4096"); }
            else throw new ArgumentException("Unknown --mcp option: "+args[i]);
        }
        var server=new McpServer(pid,keep,side,front);
        // Standard output carries protocol messages only, one JSON object per line, UTF-8.
        // Anything else this process prints goes to standard error.
        var input=new StreamReader(Console.OpenStandardInput(),new UTF8Encoding(false));
        var output=new StreamWriter(Console.OpenStandardOutput(),new UTF8Encoding(false));
        output.NewLine="\n";output.AutoFlush=true;
        Console.SetOut(Console.Error);
        Log("server "+Version()+" ready on standard input and output; "+(front?"foreground":"background")+" input");
        string line;
        while((line=input.ReadLine())!=null) {
            if(line.Trim().Length==0)continue;
            string reply=server.Handle(line);
            if(reply==null)continue;
            try { output.WriteLine(reply); }
            catch(IOException e) { Log("the client closed the connection: "+e.Message);return 0; }
        }
        return 0;
    }

    static string Error(object id,int code,string message) {
        return Json.Serialize(new Dictionary<string,object>{{"jsonrpc","2.0"},{"id",id},
            {"error",new Dictionary<string,object>{{"code",code},{"message",message}}}});
    }
    // One request line in, one reply line out; null for a notification.
    internal string Handle(string line) {
        object id=null;
        try {
            object parsed;
            try { parsed=Json.DeserializeObject(line); }
            catch(Exception e) { Log("unreadable message: "+e.Message);return Error(null,-32700,"Parse error: "+e.Message); }
            var message=parsed as Dictionary<string,object>;
            if(message==null)return Error(null,-32600,"Send one JSON-RPC request object per line; batches are not supported");
            bool hasId=message.TryGetValue("id",out id);
            object named;message.TryGetValue("method",out named);
            string method=named as string;
            if(method==null)return hasId && !message.ContainsKey("result") && !message.ContainsKey("error")?Error(id,-32600,"Missing method"):null;
            if(!hasId)return null;
            object raw;message.TryGetValue("params",out raw);
            var parameters=raw as Dictionary<string,object>??new Dictionary<string,object>();
            return Json.Serialize(new Dictionary<string,object>{{"jsonrpc","2.0"},{"id",id},{"result",Dispatch(method,parameters)}});
        }
        catch(McpError e) { Log(e.Message);return Error(id,e.Code,e.Message); }
        catch(Exception e) { Log("internal error: "+e);return Error(id,-32603,"Internal error: "+e.Message); }
    }
    object Dispatch(string method,Dictionary<string,object> p) {
        switch(method) {
            case "initialize":
                object asked;p.TryGetValue("protocolVersion",out asked);
                string version=asked as string;
                return new Dictionary<string,object>{
                    {"protocolVersion",version!=null && Versions.Contains(version)?version:Fallback},
                    {"capabilities",new Dictionary<string,object>{{"tools",new Dictionary<string,object>{{"listChanged",false}}}}},
                    {"serverInfo",new Dictionary<string,object>{{"name","phoneport"},{"title","PhonePort"},{"version",Version()}}},
                    {"instructions",Instructions(foreground)}};
            case "ping":return new Dictionary<string,object>();
            case "tools/list":return new Dictionary<string,object>{{"tools",Tools()}};
            case "tools/call":return Call(p);
            default:throw new McpError(-32601,"Method not found: "+method);
        }
    }

    static Dictionary<string,object> Field(string type,string description) {
        return new Dictionary<string,object>{{"type",type},{"description",description}};
    }
    static Dictionary<string,object> Tool(string name,string title,string description,bool readOnly,Dictionary<string,object> properties,params string[] required) {
        var schema=new Dictionary<string,object>{{"type","object"},{"properties",properties},{"additionalProperties",false}};
        if(required.Length>0)schema["required"]=required;
        // An input tool acts on a real phone, and through it on the outside world.
        var hints=new Dictionary<string,object>{{"title",title},{"readOnlyHint",readOnly},{"destructiveHint",!readOnly},{"idempotentHint",readOnly},{"openWorldHint",!readOnly}};
        return new Dictionary<string,object>{{"name",name},{"title",title},{"description",description},{"inputSchema",schema},{"annotations",hints}};
    }
    internal static List<object> Tools() {
        Func<Dictionary<string,object>> wait=()=>{
            var field=Field("integer","Milliseconds to wait after the input before the returned picture is taken. Default 700. Raise it for a slow screen.");
            field["minimum"]=0;field["maximum"]=5000;return field;
        };
        const string X="pixels from the left edge of the newest full screenshot",Y="pixels from the top edge of the newest full screenshot";
        var format=Field("string","jpeg (default, smaller) or png (exact pixels).");format["enum"]=new[]{"jpeg","png"};
        var key=Field("string","The key to press.");key["enum"]=new[]{"enter","tab","backspace","left","right","up","down","space"};
        var duration=Field("integer","Length of the drag in milliseconds. Default 400, or 180 with flick.");duration["minimum"]=50;duration["maximum"]=900;
        return new List<object>{
            Tool("phoneport_status","PhonePort status",
                "Report whether PhonePort is running, the iPhone is mirroring and Bluetooth control is ready, with the next setup step when it is not. Sends no input and takes no focus. Call it first, and after any tool error.",
                true,new Dictionary<string,object>()),
            Tool("phoneport_screenshot","iPhone screenshot",
                "Return the mirrored iPhone's current picture. Sends no input and takes no focus. The result gives the picture's width and height; x and y in every other tool are pixels in this full picture, with 0,0 at the top left. "+
                "To read small text, pass x, y, width and height together: that region comes back enlarged, and input coordinates still refer to the full picture. "+
                "A frame_age_ms above 1000 means the screen is still or the stream has stalled.",
                true,new Dictionary<string,object>{{"format",format},
                    {"x",Field("number","Left edge of a region to enlarge, in "+X+".")},{"y",Field("number","Top edge of the region, in "+Y+".")},
                    {"width",Field("number","Width of the region in full-screenshot pixels.")},{"height",Field("number","Height of the region in full-screenshot pixels.")}}),
            Tool("phoneport_tap","Tap the iPhone",
                "Tap one point on the iPhone, then return the picture taken after the wait. Take coordinates from the newest full screenshot. Read the returned picture before the next action, and never repeat a tap because its result is unclear.",
                false,new Dictionary<string,object>{{"x",Field("number","Where to tap: "+X+".")},{"y",Field("number","Where to tap: "+Y+".")},{"wait_ms",wait()}},"x","y"),
            Tool("phoneport_swipe","Drag on the iPhone",
                "Press at one point, drag to another and release, then return the picture. To scroll a list down, drag upward: start low and end higher. Start at the left edge to go back. "+
                "Set flick for a fast straight drag, for example to move between Home Screen pages.",
                false,new Dictionary<string,object>{{"x1",Field("number","Start: "+X+".")},{"y1",Field("number","Start: "+Y+".")},
                    {"x2",Field("number","End: "+X+".")},{"y2",Field("number","End: "+Y+".")},
                    {"duration_ms",duration},{"flick",Field("boolean","true for a fast straight drag released at once; false (default) for a paced list drag.")},{"wait_ms",wait()}},"x1","y1","x2","y2"),
            Tool("phoneport_type","Type on the iPhone",
                "Type text into the text field that has the cursor, then return the picture. Tap the field first and check that a cursor is visible. US ASCII only: letters, digits, punctuation, tab and newline; 1 to 1,024 characters. A newline presses Return.",
                false,new Dictionary<string,object>{{"text",Field("string","The text to type.")},{"wait_ms",wait()}},"text"),
            Tool("phoneport_key","Press a key on the iPhone",
                "Press one key, then return the picture. With no text field focused, space and enter activate the focused control.",
                false,new Dictionary<string,object>{{"key",key},{"wait_ms",wait()}},"key"),
            Tool("phoneport_home","Go to the Home Screen",
                "Go to the iPhone's Home Screen, then return the picture. Needs AssistiveTouch Button 2 mapped to Home on the iPhone, a one-time setup; if the picture does not change, that mapping is missing.",
                false,new Dictionary<string,object>{{"wait_ms",wait()}}),
            Tool("phoneport_batch","Run several inputs",
                "Run several commands in one call when the sequence is already known, then return the picture. Separate commands with semicolons: tap X Y; swipe X1 Y1 X2 Y2 [ms]; flick X1 Y1 X2 Y2 [ms]; page left|right; type \"text\"; key NAME; home; wait MS; settle [ms]; changed; hold MS; down X Y; move X Y; up X Y. "+
                "Coordinates are full-screenshot pixels. The batch stops at the first failure and nothing is retried. End a batch before any step that cannot be undone, and look. "+
                "An input needs a picture less than a second old, so after a wait of about a second on a screen that has stopped moving the next command is refused: put that command in a new call.",
                false,new Dictionary<string,object>{{"commands",Field("string","The commands, separated by semicolons. Example: tap 120 640; wait 500; type \"hello\"; key enter")},{"wait_ms",wait()}},"commands")};
    }

    static double Number(Dictionary<string,object> a,string name,double low,double high) {
        object value;
        if(!a.TryGetValue(name,out value) || value==null)throw new ArgumentException("Missing argument: "+name+". Nothing was sent.");
        if(!(value is int || value is long || value is decimal || value is double))throw new ArgumentException(name+" must be a number. Nothing was sent.");
        double n=Convert.ToDouble(value,CultureInfo.InvariantCulture);
        if(Double.IsNaN(n) || n<low || n>high)throw new ArgumentException(name+" must be from "+low.ToString(CultureInfo.InvariantCulture)+" to "+high.ToString(CultureInfo.InvariantCulture)+". Nothing was sent.");
        return n;
    }
    static bool Given(Dictionary<string,object> a,string name) { object value;return a.TryGetValue(name,out value) && value!=null; }
    static double Optional(Dictionary<string,object> a,string name,double fallback,double low,double high) { return Given(a,name)?Number(a,name,low,high):fallback; }
    static string Text(Dictionary<string,object> a,string name) {
        object value;
        if(!a.TryGetValue(name,out value) || !(value is string))throw new ArgumentException("Missing text argument: "+name+". Nothing was sent.");
        return (string)value;
    }

    // The picture an agent sees: the phone image, made smaller when its longer side exceeds the limit.
    internal static Size Shown(int width,int height,int side) {
        double scale=Math.Min(1.0,side/(double)Math.Max(width,height));
        return new Size(Math.Max(1,(int)Math.Round(width*scale)),Math.Max(1,(int)Math.Round(height*scale)));
    }
    // A screenshot pixel to the phone-image pixel under its centre, as a batch word.
    internal static string Native(double value,int shown,int native,string name) {
        if(Double.IsNaN(value) || value<0 || value>=shown)
            throw new ArgumentException(name+" is outside the screenshot (0 to "+(shown-1).ToString(CultureInfo.InvariantCulture)+"). Take a new screenshot and use its pixels. Nothing was sent.");
        double mapped=Math.Max(0,Math.Min(native-1,(value+.5)*native/shown-.5));
        return mapped.ToString("0.###",CultureInfo.InvariantCulture);
    }
    // phoneport_batch: input and timing commands only, with coordinates moved to phone pixels.
    internal static List<string[]> MapBatch(string script,Size shown,Size native) {
        if(script.Length>16384)throw new ArgumentException("Command batch is too long");
        var result=new List<string[]>();
        foreach(var tokens in Plan.Tokenize(script)) {
            string name=tokens[0].ToLowerInvariant();
            if(!BatchCommands.Contains(name))
                throw new ArgumentException("phoneport_batch does not run '"+tokens[0]+"'. It accepts: "+String.Join(", ",BatchCommands)+". Use phoneport_screenshot to look. Nothing was sent.");
            var mapped=(string[])tokens.Clone();
            int coordinates=name=="tap" || name=="move" || name=="down" || name=="up"?2:name=="swipe" || name=="flick"?4:0;
            if(mapped.Length<1+coordinates)throw new ArgumentException("Wrong argument count for "+name+". Nothing was sent.");
            for(int i=0;i<coordinates;i++) {
                double value;
                if(!Double.TryParse(mapped[1+i],NumberStyles.Float,CultureInfo.InvariantCulture,out value))throw new ArgumentException("Invalid "+name+" argument "+(i+1)+". Nothing was sent.");
                mapped[1+i]=i%2==0?Native(value,shown.Width,native.Width,name+" x"):Native(value,shown.Height,native.Height,name+" y");
            }
            result.Add(mapped);
        }
        if(result.Count==0)throw new ArgumentException("commands is empty. Nothing was sent.");
        return result;
    }
    static string Encode(Bitmap source,Rectangle region,Size size,bool png,out string mime) {
        using(var output=new Bitmap(size.Width,size.Height,PixelFormat.Format24bppRgb))
        using(var memory=new MemoryStream()) {
            using(var g=Graphics.FromImage(output)) {
                g.InterpolationMode=System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode=System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                g.DrawImage(source,new Rectangle(0,0,size.Width,size.Height),region,GraphicsUnit.Pixel);
            }
            if(png) { output.Save(memory,ImageFormat.Png);mime="image/png"; }
            else {
                var codec=ImageCodecInfo.GetImageEncoders().First(c=>c.FormatID==ImageFormat.Jpeg.Guid);
                using(var options=new EncoderParameters(1)) {
                    options.Param[0]=new EncoderParameter(System.Drawing.Imaging.Encoder.Quality,82L);
                    output.Save(memory,codec,options);
                }
                mime="image/jpeg";
            }
            return Convert.ToBase64String(memory.ToArray());
        }
    }
    static List<object> Lines(string text) {
        var result=new List<object>();
        foreach(string line in text.Split(new[]{'\r','\n'},StringSplitOptions.RemoveEmptyEntries)) {
            try { result.Add(Json.DeserializeObject(line)); }
            catch(Exception e) { Log("kept a line that is not JSON as text: "+e.Message);result.Add(line); }
        }
        return result;
    }
    static Dictionary<string,object> Result(bool error,Dictionary<string,object> info,string data,string mime) {
        var content=new List<object>();
        if(data!=null)content.Add(new Dictionary<string,object>{{"type","image"},{"data",data},{"mimeType",mime}});
        content.Add(new Dictionary<string,object>{{"type","text"},{"text",Json.Serialize(info)}});
        var result=new Dictionary<string,object>{{"content",content}};
        if(error)result["isError"]=true;
        return result;
    }
    static Dictionary<string,object> Fail(string error,string next) {
        return Result(true,new Dictionary<string,object>{{"status","error"},{"error",error},{"input_may_have_been_submitted",false},{"next",next}},null,null);
    }
    static long Describe(Dictionary<string,object> info,PhoneFrame frame,Size shown) {
        long age=(long)Frames.GetTickCount64()-Frames.Number(frame.Meta,"received_tick_ms");
        info["width"]=shown.Width;info["height"]=shown.Height;info["frame_age_ms"]=age;
        return age;
    }
    // The setup check words its advice for the command line; say the same for a tool caller.
    static string Next(string code,string next) {
        if(code=="ready")return "Ready. Take a screenshot, act once, then read the picture the action returns.";
        if(code=="idle_image")return "Ready. The screen is still, so the last picture is kept; an input tool wakes the stream before it acts.";
        return next;
    }

    object Call(Dictionary<string,object> p) {
        object named;p.TryGetValue("name",out named);
        string name=named as string;
        if(name==null || !Tools().Any(t=>(string)((Dictionary<string,object>)t)["name"]==name))throw new McpError(-32602,"Unknown tool: "+name);
        object raw;p.TryGetValue("arguments",out raw);
        var a=raw as Dictionary<string,object>??new Dictionary<string,object>();
        try {
            if(name=="phoneport_status")return Status();
            if(name=="phoneport_screenshot")return Screenshot(a);
            return Act(name,a);
        }
        catch(ArgumentException e) { Log(name+" refused: "+e.Message);return Fail(e.Message,"Fix the arguments and call again. Nothing was sent to the phone."); }
        catch(Exception e) { Log(name+" failed before any input: "+e.Message);return Fail(e.Message,"Nothing was sent to the phone. Call phoneport_status for the next setup step."); }
    }
    object Status() {
        var printed=new StringWriter();TextWriter saved=Console.Out;
        Console.SetOut(printed);
        try { Doctor.Run(fixedPid); } finally { Console.SetOut(saved); }
        var report=Lines(printed.ToString()).OfType<Dictionary<string,object>>().FirstOrDefault();
        if(report==null)throw new InvalidOperationException("The setup check printed no result");
        string code=Convert.ToString(report["code"]);
        bool usable=code=="ready" || code=="idle_image";
        report.Remove("command");
        report["next"]=Next(code,Convert.ToString(report["next"]));
        report["ready_for_input"]=usable;
        report["server"]="phoneport "+Version();
        if(usable) {
            try {
                using(var frame=Frames.Read(Convert.ToInt32(report["pid"],CultureInfo.InvariantCulture),true)) {
                    Size shown=Shown(frame.Width,frame.Height,maxSide);
                    report["screenshot_width"]=shown.Width;report["screenshot_height"]=shown.Height;
                }
            } catch(Exception e) { Log("status could not read the picture size: "+e.Message); }
        }
        return Result(false,report,null,null);
    }
    object Screenshot(Dictionary<string,object> a) {
        string format=Given(a,"format")?Text(a,"format").ToLowerInvariant():"jpeg";
        if(format!="jpeg" && format!="png")throw new ArgumentException("format is jpeg or png");
        bool crop=new[]{"x","y","width","height"}.Any(name=>Given(a,name));
        double cx=0,cy=0,cw=0,ch=0;
        if(crop) { cx=Number(a,"x",0,4095);cy=Number(a,"y",0,4095);cw=Number(a,"width",1,4096);ch=Number(a,"height",1,4096); }
        int pid=Resolve();
        using(var frame=Frames.Read(pid,true)) {
            Size shown=Shown(frame.Width,frame.Height,maxSide),size=shown;
            var region=new Rectangle(0,0,frame.Width,frame.Height);
            var info=new Dictionary<string,object>{{"status","ok"}};
            long age=Describe(info,frame,shown);
            if(crop) {
                if(cx+cw>shown.Width || cy+ch>shown.Height)throw new ArgumentException("The region is outside the "+shown.Width+" x "+shown.Height+" screenshot");
                double fx=frame.Width/(double)shown.Width,fy=frame.Height/(double)shown.Height;
                int left=(int)Math.Floor(cx*fx),top=(int)Math.Floor(cy*fy);
                int right=Math.Min(frame.Width,(int)Math.Ceiling((cx+cw)*fx)),bottom=Math.Min(frame.Height,(int)Math.Ceiling((cy+ch)*fy));
                region=Rectangle.FromLTRB(left,top,Math.Max(left+1,right),Math.Max(top+1,bottom));
                // Enlarge up to twice the phone's own pixels, within the size limit.
                double zoom=Math.Min(2.0,maxSide/(double)Math.Max(region.Width,region.Height));
                size=new Size(Math.Max(1,(int)Math.Round(region.Width*zoom)),Math.Max(1,(int)Math.Round(region.Height*zoom)));
                info["region"]=new Dictionary<string,object>{{"x",cx},{"y",cy},{"width",cw},{"height",ch}};
                info["note"]="This picture is one enlarged region, for reading. Input coordinates still refer to the full "+shown.Width+" x "+shown.Height+" screenshot.";
            }
            else if(age>1000)info["note"]="The picture is "+age+" ms old: the screen is still, or the stream has stalled. An input tool wakes the stream before it acts.";
            string mime;string data=Encode(frame.Image,region,size,format=="png",out mime);
            return Result(false,info,data,mime);
        }
    }
    string Advice(int pid,string error,bool submitted) {
        if(error.IndexOf("denied focus",StringComparison.OrdinalIgnoreCase)>=0 || error.IndexOf("Focus changed",StringComparison.OrdinalIgnoreCase)>=0)
            return (submitted?"Part of the input may have reached the phone before focus was lost: read the picture first. ":"")+
                "Windows kept another window in front, so input stopped. Ask the user to click the PhonePort window, then act again.";
        if(error.IndexOf("someone is using the phone",StringComparison.OrdinalIgnoreCase)>=0)
            return (submitted?"Part of the input may have reached the phone before it stopped: read the picture first. ":"Nothing was sent. ")+
                "The user has the PhonePort window in front and is using the PC, so input stood back. Stop and ask the user before continuing. Input is accepted again once another window is in front, or a minute after the user stops.";
        if(error.IndexOf("is minimized",StringComparison.OrdinalIgnoreCase)>=0 || error.IndexOf("is hidden",StringComparison.OrdinalIgnoreCase)>=0)
            return (submitted?"Part of the input may have reached the phone before it stopped: read the picture first. ":"Nothing was sent. ")+
                "Ask the user to restore the PhonePort window. It can stay behind other windows; it only must not be minimized or hidden.";
        string lead=submitted?"Part of the input may have reached the phone. Read the picture before anything else, and do not repeat the action blindly. ":"Nothing was sent. ";
        try {
            using(var status=Frames.Read(pid,false,true)) {
                var finding=Doctor.Assess(status.Meta,pid,(long)Frames.GetTickCount64());
                return lead+Next(finding.code,finding.next);
            }
        } catch(Exception e) { Log("no receiver status for advice: "+e.Message);return lead+"Call phoneport_status for the next setup step."; }
    }
    object Act(string name,Dictionary<string,object> a) {
        int wait=(int)Optional(a,"wait_ms",700,0,5000);
        // Read every argument before the receiver is contacted, so a wrong call sends nothing.
        double[] at=null;string text=null;int duration=0;bool flick=false;
        switch(name) {
            case "phoneport_tap":at=new[]{Number(a,"x",0,4095),Number(a,"y",0,4095)};break;
            case "phoneport_swipe":
                at=new[]{Number(a,"x1",0,4095),Number(a,"y1",0,4095),Number(a,"x2",0,4095),Number(a,"y2",0,4095)};
                object fast;flick=a.TryGetValue("flick",out fast) && fast is bool && (bool)fast;
                duration=(int)Optional(a,"duration_ms",flick?180:400,50,900);break;
            case "phoneport_type":
                text=Text(a,"text").Replace("\r\n","\n").Replace('\r','\n');
                if(text.Length==0 || text.Length>1024 || text.Any(ch=>ch!='\t' && ch!='\n' && (ch<32 || ch>126)))
                    throw new ArgumentException("text accepts 1 to 1,024 US ASCII characters, tab and newline. Nothing was typed.");
                break;
            case "phoneport_key":text=Text(a,"key");Plan.Key(text);break;
            case "phoneport_batch":
                // Checks the command names and argument shapes; real sizes are applied below.
                text=Text(a,"commands");MapBatch(text,new Size(4096,4096),new Size(4096,4096));break;
        }
        int pid=Resolve();
        int width,height;long age;byte[] before;
        using(var frame=Frames.Read(pid,true)) {
            width=frame.Width;height=frame.Height;before=Vision.Sample(frame.Image);
            age=(long)Frames.GetTickCount64()-Frames.Number(frame.Meta,"received_tick_ms");
        }
        Size shown=Shown(width,height,maxSide);
        var batch=new List<string[]>();
        switch(name) {
            case "phoneport_tap":batch.Add(new[]{"tap",Native(at[0],shown.Width,width,"x"),Native(at[1],shown.Height,height,"y")});break;
            case "phoneport_swipe":
                batch.Add(new[]{flick?"flick":"swipe",Native(at[0],shown.Width,width,"x1"),Native(at[1],shown.Height,height,"y1"),
                    Native(at[2],shown.Width,width,"x2"),Native(at[3],shown.Height,height,"y2"),duration.ToString(CultureInfo.InvariantCulture)});break;
            case "phoneport_type":batch.Add(new[]{"type",text});break;
            case "phoneport_key":batch.Add(new[]{"key",text});break;
            case "phoneport_home":batch.Add(new[]{"home"});break;
            case "phoneport_batch":batch.AddRange(MapBatch(text,shown,new Size(width,height)));break;
        }
        // A still screen sends no frames and input needs a fresh one: move the pointer first.
        if(age>300 && batch[0][0].ToLowerInvariant()!="wake")batch.Insert(0,new[]{"wake"});
        var plan=new Plan(batch);
        var printed=new StringWriter();var complaints=new StringWriter();int code;
        TextWriter savedOut=Console.Out,savedError=Console.Error;
        long started=(long)Frames.GetTickCount64();
        Console.SetOut(printed);Console.SetError(complaints);
        try { using(var lease=plan.Input?new InputLease(pid):null)code=Program.Execute(pid,plan,new Targets(null),keepFocus,foreground); }
        finally { Console.SetOut(savedOut);Console.SetError(savedError); }
        var problems=Lines(complaints.ToString()).OfType<Dictionary<string,object>>().ToList();
        var info=new Dictionary<string,object>{{"action",name.Substring("phoneport_".Length)},{"steps",Lines(printed.ToString())}};
        if(code==1) {
            var problem=problems.FirstOrDefault(d=>d.ContainsKey("error"));
            string error=problem!=null?Convert.ToString(problem["error"]):"The batch stopped";
            object flag;bool submitted=problem!=null && problem.TryGetValue("input_may_have_been_submitted",out flag) && flag is bool && (bool)flag;
            Log(name+" stopped: "+error);
            info["status"]="error";info["error"]=error;info["input_may_have_been_submitted"]=submitted;info["next"]=Advice(pid,error,submitted);
            if(!submitted)return Result(true,info,null,null);
        } else {
            info["status"]=code==2?"submitted_not_settled":"submitted";
            var warning=problems.FirstOrDefault(d=>d.ContainsKey("message"));
            if(warning!=null)info["warning"]=Convert.ToString(warning["message"]);
            info["verify"]="Read the picture. Submitted input is not proof that the phone did it.";
        }
        Thread.Sleep(wait);
        try {
            using(var frame=Frames.Read(pid,true)) {
                Size size=Shown(frame.Width,frame.Height,maxSide);string mime;
                string data=Encode(frame.Image,new Rectangle(0,0,frame.Width,frame.Height),size,false,out mime);
                Describe(info,frame,size);
                int delta=Vision.Difference(before,Vision.Sample(frame.Image));
                info["change_permille"]=delta;info["screen_changed"]=delta>=30;
                if(Frames.Number(frame.Meta,"received_tick_ms")<started)info["warning"]="No new picture arrived after the input. The screen did not change, or the stream has stalled.";
                return Result(code==1,info,data,mime);
            }
        } catch(Exception e) {
            Log("no picture after "+name+": "+e.Message);
            info["picture_error"]=e.Message;
            info["next"]="Input was sent, but no picture could be read afterwards. Do not repeat the action. Call phoneport_status.";
            return Result(true,info,null,null);
        }
    }
}
