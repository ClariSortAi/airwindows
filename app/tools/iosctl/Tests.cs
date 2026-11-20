using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

static class Tests {
    static int count;
    static void Check(bool ok,string name) {count++;if(!ok)throw new Exception("Check failed: "+name);}
    static void Reject(Action action,string name) {
        bool rejected=false;try{action();}catch{rejected=true;}Check(rejected,name);
    }
    static Dictionary<string,object> Meta(int pid,long now) {
        return new Dictionary<string,object>{{"ok",true},{"protocol",1},{"source","airplay"},{"format","ppm"},
            {"receiver_pid",pid},{"instance","12345"},{"generation",1},{"sequence",1},
            {"received_tick_ms",now-10},{"decoded_tick_ms",now-5},{"width",2},{"height",2},{"image_bytes",23},
            {"control_ready",true},{"control_busy",false},{"control_focused",true},{"control_tick_ms",now},{"control_epoch",1},
            {"control_peer","0000000000000001"},{"control_sender","0000000000000002"},
            {"status_tick_ms",now},{"mirror_active",true},{"mirror_paused",false}};
    }
    static byte[] Ppm() {
        byte[] header=Encoding.ASCII.GetBytes("P6\n2 2\n255\n");var data=new byte[header.Length+12];header.CopyTo(data,0);
        new byte[]{255,0,0,0,255,0,0,0,255,255,255,255}.CopyTo(data,header.Length);return data;
    }
    static Dictionary<string,object> Rpc(McpServer server,string json) {
        string reply=server.Handle(json);
        Check(reply!=null && reply.IndexOf('\n')<0 && reply.IndexOf('\r')<0,"an MCP reply is one line");
        return (Dictionary<string,object>)McpServer.Json.DeserializeObject(reply);
    }
    static Dictionary<string,object> Member(object value,string key) { return (Dictionary<string,object>)((Dictionary<string,object>)value)[key]; }
    static int ErrorCode(Dictionary<string,object> reply) { return Convert.ToInt32(Member(reply,"error")["code"]); }
    // The text part of a tool result that reports a failure, or null when the call succeeded.
    static string ToolError(McpServer server,string name,string arguments) {
        var result=Member(Rpc(server,"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\""+name+"\",\"arguments\":"+arguments+"}}"),"result");
        object failed;
        if(!result.TryGetValue("isError",out failed) || !(failed is bool) || !(bool)failed)return null;
        var content=(object[])result["content"];
        var facts=(Dictionary<string,object>)McpServer.Json.DeserializeObject((string)((Dictionary<string,object>)content[content.Length-1])["text"]);
        return (string)facts["error"];
    }
    // The MCP server without a phone: protocol answers, argument refusal and coordinate mapping.
    static void McpChecks() {
        McpServer.Quiet=true; // these checks provoke refusals on purpose; keep them off standard error
        var server=new McpServer(0,false,1280);
        server.Resolve=()=>{throw new InvalidOperationException("the self-test must not look for a receiver");};
        Check(server.Handle("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}")==null,"an MCP notification gets no reply");
        var hello=Member(Rpc(server,"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{},\"clientInfo\":{\"name\":\"self-test\",\"version\":\"1\"}}}"),"result");
        Check((string)hello["protocolVersion"]=="2025-03-26" && Member(hello,"capabilities").ContainsKey("tools")
            && (string)Member(hello,"serverInfo")["name"]=="phoneport" && ((string)hello["instructions"]).Contains("phoneport_screenshot"),
            "MCP initialize keeps a known protocol version, declares tools and gives instructions");
        Check(((string)hello["instructions"])==McpServer.Instructions(false) && McpServer.Instructions(false).Contains("in the background") && !McpServer.Instructions(false).Contains("to the front")
            && McpServer.Instructions(true).Contains("to the front") && !McpServer.Instructions(true).Contains("in the background"),
            "MCP instructions describe the input mode in use, and background is the default");
        hello=Member(Rpc(server,"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"1999-01-01\"}}"),"result");
        Check((string)hello["protocolVersion"]==McpServer.Fallback,"MCP initialize answers an unknown protocol version with its own");
        var pong=Rpc(server,"{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"method\":\"ping\"}");
        Check((string)pong["id"]=="a" && Member(pong,"result").Count==0,"MCP ping keeps a string id");
        Check(ErrorCode(Rpc(server,"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/list\"}"))==-32601,"unknown MCP method");
        Check(ErrorCode(Rpc(server,"{not json"))==-32700,"MCP parse error");
        Check(ErrorCode(Rpc(server,"[1,2]"))==-32600,"MCP request arrays are refused");
        Check(ErrorCode(Rpc(server,"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"phoneport_reboot\"}}"))==-32602,"unknown MCP tool");
        var tools=(object[])Member(Rpc(server,"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/list\"}"),"result")["tools"];
        var names=new List<string>();
        foreach(object tool in tools) {
            string name=(string)((Dictionary<string,object>)tool)["name"];names.Add(name);
            bool observes=name=="phoneport_status" || name=="phoneport_screenshot";
            Check(name.StartsWith("phoneport_",StringComparison.Ordinal) && (string)Member(tool,"inputSchema")["type"]=="object"
                && (bool)Member(tool,"annotations")["readOnlyHint"]==observes && (bool)Member(tool,"annotations")["destructiveHint"]==!observes,
                "MCP tool "+name+" has a schema and says whether it only observes");
        }
        Check(names.Count==8 && new HashSet<string>(names).Count==8,"eight distinct MCP tools");
        Check(ToolError(server,"phoneport_tap","{\"x\":5}").Contains("Missing argument: y"),"MCP tap without y sends nothing");
        Check(ToolError(server,"phoneport_tap","{\"x\":\"5\",\"y\":1}").Contains("must be a number"),"MCP tap refuses text coordinates");
        Check(ToolError(server,"phoneport_type","{\"text\":\"snowman ☃\"}").Contains("US ASCII"),"MCP type refuses Unicode before the receiver is contacted");
        Check(ToolError(server,"phoneport_key","{\"key\":\"escape\"}").Contains("Unsupported key"),"MCP key refuses an unsupported key");
        Check(ToolError(server,"phoneport_swipe","{\"x1\":1,\"y1\":1,\"x2\":2,\"y2\":2,\"duration_ms\":5000}").Contains("duration_ms"),"MCP swipe bounds its duration");
        Check(ToolError(server,"phoneport_batch","{\"commands\":\"tap 1 2; shot x.png\"}").Contains("does not run 'shot'"),"MCP batch refuses file commands");
        Check(ToolError(server,"phoneport_screenshot","{\"format\":\"gif\"}").Contains("jpeg or png"),"MCP screenshot refuses an unknown format");
        Check(ToolError(server,"phoneport_screenshot","{}").Contains("must not look for a receiver"),"an MCP tool failure is a result the agent can read, not a protocol error");
        Check(McpServer.Shown(1170,2532,1280)==new Size(591,1280) && McpServer.Shown(100,200,1280)==new Size(100,200),"MCP screenshots shrink to the size limit and never enlarge");
        Check(McpServer.Native(0,591,1170,"x")=="0.49" && McpServer.Native(590,591,1170,"x")=="1168.51","MCP coordinates map to the phone pixel under the screenshot pixel");
        Reject(()=>McpServer.Native(591,591,1170,"x"),"MCP coordinate past the screenshot edge");
        var mapped=McpServer.MapBatch("tap 10 20; type 'a; b'; flick 1 2 3 4 200",new Size(100,200),new Size(200,400));
        Check(mapped.Count==3 && mapped[0][1]=="20.5" && mapped[0][2]=="40.5" && mapped[1][1]=="a; b" && mapped[2][4]=="8.5" && mapped[2][5]=="200",
            "MCP batch moves coordinates to phone pixels and leaves text and timing alone");
        Reject(()=>McpServer.MapBatch("tap 100 20",new Size(100,200),new Size(200,400)),"MCP batch coordinate outside the screenshot");
        Reject(()=>McpServer.MapBatch("",new Size(100,200),new Size(200,400)),"empty MCP batch");
        string awkward="it's \"quoted\"; ends with a backslash\\";
        Check(new Plan(new List<string[]>{new[]{"type",awkward}}).Commands[0].Args[0]==awkward,"a batch built from words needs no quoting");
        Reject(()=>new Plan(new List<string[]>{new string[0]}),"empty word list");
        Reject(()=>new Plan(new List<string[]>{new[]{"wait","10"},new[]{"wake"}}),"batch rules apply to a batch built from words");
    }
    static void PipeRoundTrip(bool stall,bool retained=false,bool inspect=false,bool save=false,bool mcp=false) {
        int pid=Process.GetCurrentProcess().Id;Exception error=null;
        using(var server=new NamedPipeServerStream("AirWindows.Frames."+pid,PipeDirection.InOut,1,PipeTransmissionMode.Byte,PipeOptions.Asynchronous)) {
            var worker=new Thread(()=>{
                try {
                    server.WaitForConnection();
                    if(stall){Thread.Sleep(2100);return;}
                    var values=Meta(pid,(long)Frames.GetTickCount64());
                    if(retained)values["received_tick_ms"]=(long)Frames.GetTickCount64()-60000;
                    if(inspect){values["ok"]=false;values["image_bytes"]=0;values["error"]="no_active_frame";}
                    byte[] meta=Encoding.UTF8.GetBytes(Frames.Json.Serialize(values)),ppm=inspect?new byte[0]:Ppm();
                    byte[] size=BitConverter.GetBytes(meta.Length);server.Write(size,0,4);server.Write(meta,0,meta.Length);server.Write(ppm,0,ppm.Length);
                    if(server.ReadByte()!=75)throw new Exception("Missing frame acknowledgement");
                } catch(Exception e){error=e;}
            });worker.IsBackground=true;worker.Start();
            if(stall)Reject(()=>{using(var f=Frames.Read(pid)){}},"bounded stalled-pipe read");
            else if(save) {
                string path=Path.Combine(Path.GetTempPath(),"PhonePort-snapshot-test-"+Guid.NewGuid().ToString("N")+".png");
                try {
                    Check(Program.Execute(pid,new Plan("snapshot '"+path+"'"),new Targets(null),false)==0,"standalone snapshot command over real pipe");
                    using(var image=new Bitmap(path))Check(image.Width==2 && image.GetPixel(0,0).R==255,"snapshot saves actual decoded pixels");
                    var saved=Frames.Json.Deserialize<Dictionary<string,object>>(File.ReadAllText(Path.ChangeExtension(path,".json")));
                    Check(Frames.Number(saved,"frame_age_ms")>=60000 && !Frames.Flag(saved,"frame_fresh") && Frames.Text(saved,"purpose")=="inspection_only","snapshot sidecar retains age and inspection purpose");
                }finally{File.Delete(path);File.Delete(Path.ChangeExtension(path,".json"));}
            }
            else if(mcp) {
                var tools=new McpServer(0,false,1280);tools.Resolve=()=>pid;
                var result=Member(Rpc(tools,"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"phoneport_screenshot\",\"arguments\":{}}}"),"result");
                var content=(object[])result["content"];var picture=(Dictionary<string,object>)content[0];
                var facts=(Dictionary<string,object>)McpServer.Json.DeserializeObject((string)((Dictionary<string,object>)content[1])["text"]);
                Check(!result.ContainsKey("isError") && (string)picture["type"]=="image" && (string)picture["mimeType"]=="image/jpeg","MCP screenshot returns a JPEG over a real pipe");
                using(var image=new Bitmap(new MemoryStream(Convert.FromBase64String((string)picture["data"]))))Check(image.Width==2 && image.Height==2,"MCP screenshot keeps a small picture at its own size");
                Check(Convert.ToInt32(facts["width"])==2 && Convert.ToInt64(facts["frame_age_ms"])>=60000 && ((string)facts["note"]).Contains("still"),"MCP screenshot gives its size and says a kept picture is old");
            }
            else using(var frame=Frames.Read(pid,retained,inspect))Check(inspect?frame.Image==null && !Frames.Flag(frame.Meta,"ok"):frame.Width==2 && frame.Image.GetPixel(0,0).R==255,"real pipe image/status decoding");
            Check(worker.Join(3000),"pipe test worker finished");
            if(!stall && error!=null)throw error;
        }
    }
    internal static int Run() {
        Check(Marshal.SizeOf(typeof(Desktop.Input))==40,"x64 INPUT ABI");
        var plan=new Plan("tap 12 34; type 'text with spaces; and semicolon'; wait 20; shot 'C:\\test folder\\phone.png' .5");
        Check(plan.Commands.Count==4 && plan.Input && plan.Control,"batch parser");
        Check(plan.Commands[1].Args[0]=="text with spaces; and semicolon","quoted text");
        Check(plan.Commands[3].Args[0]=="C:\\test folder\\phone.png","Windows paths");
        Check(new Plan("status; preflight").Control && !new Plan("status; shot frame.png").Input,"observation does not require input");
        Check(!new Plan("snapshot 'idle image.png'").Input,"snapshot never takes input focus");
        Reject(()=>new Plan("snapshot idle.png; tap 1 2"),"retained snapshot cannot authorize input");
        Reject(()=>new Plan("tap 1 2; snapshot idle.png"),"retained snapshot cannot verify an action");
        Check(new Plan("wake; tap 1 2").Input,"wake may precede input");
        Reject(()=>new Plan("tap 1 2; wake"),"wake must come first");
        Reject(()=>new Plan("wake 1 2"),"wake takes no arguments");
        Reject(()=>new Plan("wake; snapshot idle.png"),"retained snapshot cannot follow wake");
        foreach(string script in new[]{"", "tap 1 2; typo", "tap NaN 3", "tap Infinity 0", "tap -1 0", "swipe 1 2 3 4 5000", "down 1 2", "up 1 2", "down 1 2; down 2 3; up 1 2", "down 1 2;wait 2000;up 1 2", "type 'snowman \u2603'", "type ''", "key escape", "tapv 1 2", "gov camera", "page right 4", "shot x.png 0", "shot x.jpg", "crop x.png 1 2 0 4", "type 'unterminated"})Reject(()=>new Plan(script),"reject "+script);
        Check(new Plan("down 1 2; move 2 3; wait 50; up 2 3").Commands.Count==4,"bounded drag primitives");
        Reject(()=>new Plan("down 1 2; settle; up 1 2"),"no blocking observation during held input");
        Reject(()=>new Plan("down 1 2; shot frame.png; up 1 2"),"release before frame I/O");
        Check(new Plan("type 'a\r\nb'").Commands[0].Args[0]=="a\nb","normalize CRLF");
        Check(Plan.Key("backspace")==8 && Plan.Key("right")==39,"key mapping");
        var portrait=Desktop.Map(new Rectangle(100,20,1000,1000),500,1000,0,0);
        Check(portrait.X==350 && portrait.Y==20,"portrait letterbox");
        var landscape=Desktop.Map(new Rectangle(-1800,-100,1600,1000),1600,800,1599,799);
        Check(landscape.X==-201 && landscape.Y==799,"landscape and negative monitor origin");
        Check(Desktop.Map(new Rectangle(0,0,1000,2000),500,1000,100,200)==new Point(201,401),"DPI-independent source mapping");
        Reject(()=>Desktop.Map(new Rectangle(0,0,100,100),50,50,50,10),"outside source width");
        Reject(()=>Desktop.Map(new Rectangle(0,0,0,100),50,50,1,1),"empty video rectangle");
        // Background input: the same mapping into the picture area's own coordinates, then the
        // window message a real mouse would have produced. Values from a run on 2026-11-20.
        Check(Desktop.Map(new Rectangle(0,0,560,1124),498,1080,249,884)==new Point(280,920),"background input maps a phone pixel into the picture area");
        Check(Desktop.Pack(new Point(280,920))==new IntPtr((920<<16)|280),"a posted pointer position carries x in the low word and y in the high word");
        Check(Desktop.MouseMessage(Desktop.MoveFlag)==0x200 && Desktop.MouseMessage(Desktop.LeftDown)==0x201 && Desktop.MouseMessage(Desktop.LeftUp)==0x202
            && Desktop.MouseMessage(Desktop.RightDown)==0x204 && Desktop.MouseMessage(Desktop.RightUp)==0x205 && Desktop.MouseMessage(64)==0,"pointer events map to their window messages");
        Check(Desktop.PersonHasPhone(true,0) && Desktop.PersonHasPhone(true,6000) && Desktop.PersonHasPhone(true,Desktop.PersonHoldMs-1),"PhonePort in front and a person at the PC: scripted input stands back");
        Check(!Desktop.PersonHasPhone(false,0) && !Desktop.PersonHasPhone(true,Desktop.PersonHoldMs) && !Desktop.PersonHasPhone(true,-5),"another window in front, or nobody at the PC for a minute: scripted input may run");
        int released=0;
        Reject(()=>Desktop.WithRelease(()=>{},()=>{throw new Exception("body");},()=>released++),"gesture body failure");
        Reject(()=>Desktop.WithRelease(()=>{throw new Exception("press");},()=>{},()=>released++),"partial press failure");
        Check(released==2,"release executes on both failures");
        bool competingRejected=false;
        using(var lease=new InputLease(Process.GetCurrentProcess().Id)) {
            var contender=new Thread(()=>{try{using(var competing=new InputLease(Process.GetCurrentProcess().Id)){} }catch(InvalidOperationException){competingRejected=true;}});
            contender.Start();Check(contender.Join(1000) && competingRejected,"concurrent input batch rejected");
        }
        using(var lease=new InputLease(Process.GetCurrentProcess().Id))Check(true,"input lease released after completion");
        long now=(long)Frames.GetTickCount64();var m=Meta(42,now);Frames.Validate(m,42,now);Frames.RequireControl(m,now);Check(true,"valid frame and control metadata");
        Check(!Frames.Drained(m,now+1),"old idle status does not acknowledge new input");
        Check(Frames.Drained(m,now),"fresh focused idle status");
        var canceled=new Dictionary<string,object>(m);canceled["control_focused"]=false;
        Reject(()=>Frames.Drained(canceled,now),"receiver cancellation is not successful input");
        foreach(string key in new[]{"generation","width","height","image_bytes"}) {
            var bad=new Dictionary<string,object>(m);bad[key]=0;Reject(()=>Frames.Validate(bad,42,now),"bad "+key);
        }
        var stale=new Dictionary<string,object>(m);stale["received_tick_ms"]=now-1001;Reject(()=>Frames.Validate(stale,42,now),"stale frame");
        Frames.Validate(stale,42,now,true);Check(true,"explicit retained snapshot permits old pixels");
        var paused=new Dictionary<string,object>(stale);paused["mirror_paused"]=true;
        Reject(()=>Frames.Validate(paused,42,now,true),"snapshot rejects pause");
        paused["mirror_paused"]=false;paused["mirror_active"]=false;
        Reject(()=>Frames.Validate(paused,42,now,true),"snapshot rejects disconnect");
        paused["mirror_active"]=true;paused["status_tick_ms"]=now-1001;
        Reject(()=>Frames.Validate(paused,42,now,true),"old status cannot authorize a retained snapshot");
        var legacy=new Dictionary<string,object>(m);legacy.Remove("status_tick_ms");
        Frames.Validate(legacy,42,now);Check(true,"legacy fresh capture remains compatible");
        Reject(()=>Frames.Validate(legacy,42,now,true),"old receiver cannot claim active retained image");
        Check(Doctor.Select(new[]{42},0)==42 && Doctor.Select(new[]{42,43},43)==43,"doctor selects only unambiguous or explicit receiver");
        Reject(()=>Doctor.Select(new int[0],0),"doctor reports no receiver");
        Reject(()=>Doctor.Select(new[]{42,43},0),"doctor refuses arbitrary receiver");
        Reject(()=>Doctor.Select(new[]{42},43),"doctor rejects absent requested PID");
        Check(Doctor.StartHint(true).Contains("phoneport --frames") && !Doctor.StartHint(true).Contains("Start-PhonePort.cmd")
            && Doctor.StartHint(false).Contains("Start-PhonePort.cmd") && Doctor.StartHint(false).IndexOf("airwindows",StringComparison.OrdinalIgnoreCase)<0,
            "doctor names the Store install's start, not the ZIP's");
        Check(StartMenuLaunch.ShouldPause(0,false,false,1)
            && !StartMenuLaunch.ShouldPause(0,false,false,2) && !StartMenuLaunch.ShouldPause(0,false,false,0)
            && !StartMenuLaunch.ShouldPause(1,false,false,1) && !StartMenuLaunch.ShouldPause(0,true,false,1)
            && !StartMenuLaunch.ShouldPause(0,false,true,1),
            "only a no-argument start in its own console window waits for Enter");
        Check(StartMenuLaunch.Message.Contains("iosctl --help") && StartMenuLaunch.Message.Contains("https://graniteai.co/PhonePort/Support"),
            "the Start menu message names iosctl --help and the support page");
        Check(StartMenuLaunch.ConsoleProcesses()!=1 || Console.IsOutputRedirected || Console.IsInputRedirected,
            "the self-test itself, run from a terminal or with redirected output, is not treated as a Start menu launch");
        Check(Receiver.IsReceiverName("PhonePort") && Receiver.IsReceiverName("AirWindowsNative") && Receiver.IsReceiverName("AirWindows")
            && !Receiver.IsReceiverName("iosctl"),"receiver discovery accepts PhonePort.exe and the exe names from before the rename");
        Check(DataFolder.ForFamily("Example.App_abc123").EndsWith(@"\Packages\Example.App_abc123\LocalState",StringComparison.OrdinalIgnoreCase)
            && DataFolder.Classic().EndsWith(@"\AirWindows",StringComparison.OrdinalIgnoreCase),"data folder paths for packaged and unpackaged receivers");
        {string basis;var own=DataFolder.Resolve(Process.GetCurrentProcess().Id,out basis);
         Check(DataFolder.OwnFamily()==null?basis=="receiver_unpackaged" && own==DataFolder.Classic():basis=="receiver_package","data folder follows the process's package identity");}
        {string basis,note;DataFolder.Resolve(0x7FFFFFF0,out basis,out note);
         Check(basis=="receiver_unknown" && note!=null && note.Contains("assumed"),"a receiver that cannot be inspected is reported as unknown, with the assumption");}
        Check(Doctor.Assess(m,42,now).ready,"doctor ready with unfocused fresh control");
        var diagnostic=new Dictionary<string,object>(m);diagnostic["control_focused"]=false;
        Check(Doctor.Assess(diagnostic,42,now).ready,"doctor does not require focus or take it");
        diagnostic["control_busy"]=true;Check(Doctor.Assess(diagnostic,42,now).code=="input_busy","doctor identifies active input");
        diagnostic["control_busy"]=false;diagnostic["control_ready"]=false;
        Check(Doctor.Assess(diagnostic,42,now).code=="control_not_ready","doctor identifies pairing or binding");
        diagnostic["control_tick_ms"]=now-1001;
        Check(Doctor.Assess(diagnostic,42,now).code=="control_status_stale","doctor catches stalled control updates");
        Check(Doctor.Assess(stale,42,now).code=="idle_image","doctor distinguishes idle from disconnect");
        diagnostic["mirror_paused"]=true;Check(Doctor.Assess(diagnostic,42,now).code=="mirror_paused","doctor identifies pause");
        diagnostic["mirror_paused"]=false;diagnostic["ok"]=false;
        Check(Doctor.Assess(diagnostic,42,now).code=="no_picture","doctor diagnoses before first frame");
        diagnostic["mirror_active"]=false;Check(Doctor.Assess(diagnostic,42,now).code=="not_mirroring","doctor identifies no mirror");
        Reject(()=>Frames.Validate(m,43,now),"wrong receiver PID");
        var wrong=new Dictionary<string,object>(m);wrong["source"]="selftest";Reject(()=>Frames.Validate(wrong,42,now),"synthetic frame rejected in normal client");
        wrong=new Dictionary<string,object>(m);wrong["control_ready"]=false;Reject(()=>Frames.RequireControl(wrong,now),"not ready");
        wrong=new Dictionary<string,object>(m);wrong["control_tick_ms"]=now-1001;Reject(()=>Frames.RequireControl(wrong,now),"stale readiness");
        wrong=new Dictionary<string,object>(m);wrong["control_peer"]="unbound";Reject(()=>Frames.RequireControl(wrong,now),"missing phone identity");
        using(var frame=new PhoneFrame{Meta=m,Image=Frames.Decode(Ppm(),2,2)}) {
            Check(frame.Image.GetPixel(1,0).G==255 && frame.Image.GetPixel(0,1).B==255,"PPM row and channel ordering");
            var session=new FrameSession();session.Check(frame,true);m["control_epoch"]=2;
            Reject(()=>session.Check(frame,true),"binding change aborts batch");m["control_epoch"]=1;
            var a=Vision.Sample(frame.Image);Check(Vision.Difference(a,a)==0,"unchanged pixels");
            byte[] b=new byte[a.Length];for(int i=0;i<b.Length;i++)b[i]=(byte)(a[i]>127?0:255);
            Check(Vision.Difference(a,b)==1000,"changed pixels");
            Reject(()=>Vision.Difference(null,a),"missing baseline is not success");
            string dir=Path.Combine(Path.GetTempPath(),"PhonePort-iosctl-test-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(dir);
            try {
                string path=Path.Combine(dir,"phone.png");Vision.Save(frame,path,1,new Rectangle(0,0,2,2));
                Check(File.Exists(path) && File.Exists(Path.ChangeExtension(path,".json")),"PNG and metadata saved");
                Reject(()=>Vision.Save(frame,path,1,new Rectangle(1,1,2,2)),"invalid crop");
                string map=Path.Combine(dir,"targets.json");File.WriteAllText(map,"{\"width\":2,\"height\":2,\"targets\":{\"sample\":[1,1]}}");
                Check(new Targets(map).Get("sample",frame)[0]==1,"portable target map");
                Reject(()=>new Targets(map).Get("missing",frame),"unknown target");
            } finally {
                foreach(string file in new[]{"phone.png","phone.json","targets.json"})File.Delete(Path.Combine(dir,file));
                Directory.Delete(dir); // only our now-empty unique test directory
            }
        }
        Reject(()=>Frames.Decode(new byte[2],2,2),"truncated PPM");
        PipeRoundTrip(false);PipeRoundTrip(true);PipeRoundTrip(false,true);PipeRoundTrip(false,false,true);PipeRoundTrip(false,true,false,true);
        McpChecks();PipeRoundTrip(false,true,false,false,true);
        Program.Output(new{status="passed",checks=count,desktop_input=false,bluetooth=false});return 0;
    }
}
