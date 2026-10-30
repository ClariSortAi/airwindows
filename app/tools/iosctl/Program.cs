using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;

sealed class Command {
    internal string Name;
    internal string[] Args;
    internal double N(int i) { return Double.Parse(Args[i],CultureInfo.InvariantCulture); }
}
sealed class Plan {
    internal readonly List<Command> Commands=new List<Command>();
    internal bool Input;
    internal bool Control;
    internal static List<string[]> Tokenize(string text) {
        var result=new List<string[]>();var words=new List<string>();var word=new StringBuilder();char quote='\0';bool started=false;
        for(int i=0;i<=text.Length;i++) {
            char ch=i==text.Length?'\0':text[i];
            if(quote!='\0') {
                if(ch=='\0')throw new ArgumentException("Unclosed command quote");
                if(ch==quote)quote='\0';
                else if(ch=='\\' && i+1<text.Length && text[i+1]==quote){word.Append(quote);i++;}
                else word.Append(ch);
                continue;
            }
            if(ch=='\'' || ch=='\"') {quote=ch;started=true;continue;}
            if(ch=='\0' || ch==';' || Char.IsWhiteSpace(ch)) {
                if(started){words.Add(word.ToString());word.Clear();started=false;}
                if(ch=='\0' || ch==';'){if(words.Count>0)result.Add(words.ToArray());words.Clear();}
            } else {word.Append(ch);started=true;}
        }
        return result;
    }
    static void Count(Command c,int low,int high) { if(c.Args.Length<low || c.Args.Length>high)throw new ArgumentException("Wrong argument count for "+c.Name); }
    static double Range(Command c,int i,double low,double high,bool integer=false) {
        double n;
        if(!Double.TryParse(c.Args[i],NumberStyles.Float,CultureInfo.InvariantCulture,out n) || Double.IsNaN(n) || Double.IsInfinity(n) || n<low || n>high || (integer && n!=Math.Truncate(n)))throw new ArgumentException("Invalid "+c.Name+" argument "+(i+1));
        return n;
    }
    internal static ushort Key(string name) {
        switch(name.ToLowerInvariant()) {
            case "enter":case "return":return 13;case "tab":return 9;case "back":case "backspace":return 8;
            case "left":return 37;case "up":return 38;case "right":return 39;case "down":return 40;case "space":return 32;
            default:throw new ArgumentException("Unsupported key: "+name);
        }
    }
    internal Plan(string script) {
        if(script.Length>16384)throw new ArgumentException("Command batch is too long");
        bool held=false;int heldWait=0,totalWait=0;
        foreach(var tokens in Tokenize(script)) {
            var c=new Command{Name=tokens[0].ToLowerInvariant(),Args=tokens.Skip(1).ToArray()};
            if(held && c.Name!="move" && c.Name!="up" && c.Name!="wait")throw new ArgumentException("While holding, use only move, a short wait, and up");
            bool action=false;
            switch(c.Name) {
                case "tap":case "move":case "down":case "up":
                    Count(c,2,2);Range(c,0,0,4095);Range(c,1,0,4095);action=true;break;
                case "swipe":case "flick":
                    Count(c,4,5);for(int i=0;i<4;i++)Range(c,i,0,4095);if(c.Args.Length==5)Range(c,4,50,900,true);action=true;break;
                case "home":Count(c,0,0);action=true;break;
                case "page":Count(c,1,1);if(c.Args[0]!="left" && c.Args[0]!="right")throw new ArgumentException("page expects left or right");action=true;break;
                case "go":Count(c,1,1);action=true;break;
                case "type":
                    Count(c,1,1);c.Args[0]=c.Args[0].Replace("\r\n","\n").Replace('\r','\n');
                    if(c.Args[0].Length==0 || c.Args[0].Length>1024 || c.Args[0].Any(ch=>ch!='\t' && ch!='\n' && (ch<32 || ch>126)))throw new ArgumentException("type accepts 1-1024 US ASCII characters, tab and newline");
                    action=true;break;
                case "key":Count(c,1,1);Key(c.Args[0]);action=true;break;
                case "hold":Count(c,1,1);Range(c,0,0,350,true);break;
                case "wait":
                    Count(c,1,1);int ms=(int)Range(c,0,0,5000,true);totalWait+=ms;if(held)heldWait+=ms;
                    if(heldWait>1000 || totalWait>15000)throw new ArgumentException("Wait budget exceeded");break;
                case "settle":Count(c,0,1);if(c.Args.Length>0)Range(c,0,100,5000,true);break;
                case "snapshot":case "shot":
                    Count(c,1,2);if(c.Args.Length==2)Range(c,1,.1,2);ValidatePath(c.Args[0]);break;
                case "crop":
                    Count(c,5,6);ValidatePath(c.Args[0]);Range(c,1,0,4095,true);Range(c,2,0,4095,true);Range(c,3,1,4096,true);Range(c,4,1,4096,true);if(c.Args.Length==6)Range(c,5,.1,4);break;
                case "grid":Count(c,0,1);if(c.Args.Length>0)Range(c,0,0,255,true);break;
                case "status":case "preflight":case "targets":case "changed":Count(c,0,0);break;
                case "tapv":case "gov":throw new ArgumentException("Automatic retries were removed. Use tap/go, then observe the result.");
                default:throw new ArgumentException("Unknown command: "+c.Name);
            }
            if(c.Name=="down") {if(held)throw new ArgumentException("Nested down");held=true;heldWait=0;}
            else if(c.Name=="up") {if(!held)throw new ArgumentException("up requires down in the same batch");held=false;}
            else if(held && action && c.Name!="move")throw new ArgumentException("Release the held button before another action");
            Input|=action;Control|=action || c.Name=="preflight";Commands.Add(c);
        }
        if(held)throw new ArgumentException("down must have a matching up in the same batch");
        if(Commands.Count==0 || Commands.Count>64)throw new ArgumentException("Use 1-64 commands per batch");
        if(Commands.Any(c=>c.Name=="snapshot") && Commands.Count!=1)throw new ArgumentException("snapshot is a standalone observation; never mix retained images with input or verification");
    }
    static void ValidatePath(string path) {
        if(!String.Equals(Path.GetExtension(path),".png",StringComparison.OrdinalIgnoreCase))throw new ArgumentException("Capture paths must end in .png");
        Path.GetFullPath(path);
    }
}

sealed class Targets {
    internal int Width,Height;
    internal readonly Dictionary<string,double[]> Values=new Dictionary<string,double[]>(StringComparer.OrdinalIgnoreCase);
    internal Targets(string path) {
        if(path==null)return;
        if(new FileInfo(path).Length>65536)throw new ArgumentException("Target map is too large");
        var map=Frames.Json.Deserialize<Dictionary<string,object>>(File.ReadAllText(path));
        Width=(int)Frames.Number(map,"width");Height=(int)Frames.Number(map,"height");
        if(Width<1 || Height<1 || Width>4096 || Height>4096)throw new ArgumentException("Invalid target-map dimensions");
        var values=(Dictionary<string,object>)map["targets"];
        if(values.Count>256)throw new ArgumentException("Too many targets");
        foreach(var entry in values) {
            if(!System.Text.RegularExpressions.Regex.IsMatch(entry.Key,"^[a-zA-Z0-9_-]{1,64}$"))throw new ArgumentException("Invalid target name");
            var xy=(System.Collections.IList)entry.Value;
            if(xy.Count!=2)throw new ArgumentException("Targets require x,y");
            foreach(object value in xy)if(!(value is int || value is long || value is double || value is decimal))throw new ArgumentException("Target coordinates must be numbers");
            double x=Convert.ToDouble(xy[0],CultureInfo.InvariantCulture),y=Convert.ToDouble(xy[1],CultureInfo.InvariantCulture);
            Desktop.Map(new Rectangle(0,0,Width,Height),Width,Height,x,y);Values.Add(entry.Key,new[]{x,y});
        }
    }
    internal double[] Get(string name,PhoneFrame frame) {
        if(frame.Width!=Width || frame.Height!=Height)throw new ArgumentException("Target map belongs to different phone dimensions");
        double[] xy;if(!Values.TryGetValue(name,out xy))throw new ArgumentException("Unknown target: "+name);
        return xy;
    }
}

static class Program {
    internal static void Output(object value) { Console.WriteLine(Frames.Json.Serialize(value)); }
    static PhoneFrame Read(int pid,FrameSession session,bool control) {
        var frame=Frames.Read(pid);try{session.Check(frame,control);return frame;}catch{frame.Dispose();throw;}
    }
    static void Idle(int pid,FrameSession session) {
        long after=(long)Frames.GetTickCount64(),previous=0;
        Thread.Sleep(180);var timer=Stopwatch.StartNew();int quiet=0;
        while(timer.ElapsedMilliseconds<3500) {
            using(var frame=Read(pid,session,true)) {
                long tick=Frames.Number(frame.Meta,"control_tick_ms");
                if(Frames.Drained(frame.Meta,after)) {if(tick!=previous)quiet++;}else quiet=0;
                previous=tick;
                if(quiet==2)return;
            }
            Thread.Sleep(100);
        }
        throw new TimeoutException("Receiver input is still busy; outcome unknown, do not replay");
    }
    static void Geometry(Plan plan,Targets targets,PhoneFrame frame) {
        var r=new Rectangle(0,0,frame.Width,frame.Height);
        foreach(var c in plan.Commands) {
            switch(c.Name) {
                case "tap":case "move":case "down":case "up":Desktop.Map(r,frame.Width,frame.Height,c.N(0),c.N(1));break;
                case "swipe":case "flick":Desktop.Map(r,frame.Width,frame.Height,c.N(0),c.N(1));Desktop.Map(r,frame.Width,frame.Height,c.N(2),c.N(3));break;
                case "go":targets.Get(c.Args[0],frame);break;
                case "crop":if(c.N(1)+c.N(3)>frame.Width || c.N(2)+c.N(4)>frame.Height)throw new ArgumentException("Crop exceeds phone dimensions");break;
            }
        }
    }
    internal static int Execute(int pid,Plan plan,Targets targets,bool keep) {
        var session=new FrameSession();Desktop desktop=null;byte[] baseline=null;int hold=300;bool submitted=false;
        var total=Stopwatch.StartNew();
        try {
            if(plan.Commands[0].Name=="snapshot") {
                var c=plan.Commands[0];
                using(var frame=Frames.Read(pid,true)) {
                    frame.Meta["frame_age_ms"]=(long)Frames.GetTickCount64()-Frames.Number(frame.Meta,"received_tick_ms");
                    frame.Meta["frame_fresh"]=Frames.Number(frame.Meta,"frame_age_ms")<=1000;
                    frame.Meta["purpose"]="inspection_only";
                    Vision.Save(frame,c.Args[0],c.Args.Length==2?c.N(1):1,new Rectangle(0,0,frame.Width,frame.Height));
                    Output(new{command=c.Name,status="saved",path=Path.GetFullPath(c.Args[0]),frame_age_ms=Frames.Number(frame.Meta,"frame_age_ms"),frame_fresh=Frames.Flag(frame.Meta,"frame_fresh"),purpose="inspection_only"});
                }
                return 0;
            }
            using(var first=Read(pid,session,plan.Control)) {
                Geometry(plan,targets,first);
                if(plan.Input && Frames.Flag(first.Meta,"control_busy"))throw new InvalidOperationException("Receiver is processing input; observe before starting another batch");
            }
            if(plan.Input)desktop=new Desktop(pid,keep);
            foreach(var c in plan.Commands) {
                if(total.ElapsedMilliseconds>120000)throw new TimeoutException("Batch exceeded two minutes");
                if(c.Name=="wait"){Thread.Sleep((int)c.N(0));Output(new{command=c.Name,status="elapsed"});continue;}
                if(desktop!=null && desktop.Holding && (c.Name=="move" || c.Name=="up")) {
                    // A gesture uses one observed geometry. Do not block on a
                    // new frame while holding a button; release before capture.
                    if(c.Name=="move")desktop.Move(c.N(0),c.N(1));else desktop.Up(c.N(0),c.N(1));
                    Output(new{command=c.Name,status="submitted",physical_result="verify_by_observation"});
                    if(c.Name=="up")Idle(pid,session);
                    continue;
                }
                if(c.Name=="hold"){hold=(int)c.N(0);continue;}
                if(c.Name=="targets"){Output(new{command=c.Name,width=targets.Width,height=targets.Height,targets=targets.Values});continue;}
                if(c.Name=="settle") {
                    var timer=Stopwatch.StartNew();byte[] prior=null;bool changed=false;int stable=0,cap=c.Args.Length==0?2500:(int)c.N(0);
                    while(timer.ElapsedMilliseconds<cap) {
                        using(var f=Read(pid,session,plan.Control)) {
                            var sample=Vision.Sample(f.Image);
                            changed|=Vision.Difference(baseline,sample)>=30;
                            if(changed && prior!=null && Vision.Difference(prior,sample)<=2)stable++;else stable=0;
                            prior=sample;
                        }
                        if(stable>=2)break;Thread.Sleep(100);
                    }
                    Output(new{command=c.Name,status=stable>=2?"settled":changed?"unstable":"unchanged",elapsed_ms=timer.ElapsedMilliseconds,semantic_success="not_checked"});
                    if(stable<2)return 2;baseline=prior;continue;
                }
                using(var frame=Read(pid,session,plan.Control)) {
                    switch(c.Name) {
                        case "status":Output(frame.Meta);continue;
                        case "preflight":
                            for(int i=0;i<2;i++){Thread.Sleep(150);using(var probe=Read(pid,session,true)){}}
                            Output(new{command=c.Name,status="ready",pid=pid,width=frame.Width,height=frame.Height,physical_input="not_tested"});continue;
                        case "shot":Vision.Save(frame,c.Args[0],c.Args.Length==2?c.N(1):1,new Rectangle(0,0,frame.Width,frame.Height));Output(new{command=c.Name,status="saved",path=Path.GetFullPath(c.Args[0])});continue;
                        case "crop":Vision.Save(frame,c.Args[0],c.Args.Length==6?c.N(5):2,new Rectangle((int)c.N(1),(int)c.N(2),(int)c.N(3),(int)c.N(4)));Output(new{command=c.Name,status="saved",path=Path.GetFullPath(c.Args[0])});continue;
                        case "grid":Output(Vision.Grid(frame.Image,c.Args.Length==1?(int)c.N(0):45));continue;
                        case "changed":
                            var sample=Vision.Sample(frame.Image);int delta=Vision.Difference(baseline,sample);baseline=sample;
                            Output(new{command=c.Name,permille=delta,changed=delta>=30,semantic_success="not_checked"});continue;
                    }
                    baseline=Vision.Sample(frame.Image);desktop.Prepare(frame);submitted=true;
                    switch(c.Name) {
                        case "tap":desktop.Tap(c.N(0),c.N(1));break;
                        case "go":var point=targets.Get(c.Args[0],frame);desktop.Tap(point[0],point[1]);break;
                        case "home":desktop.Tap(frame.Width/2.0,frame.Height/2.0,true);break;
                        case "move":desktop.Move(c.N(0),c.N(1));break;
                        case "down":desktop.Down(c.N(0),c.N(1));break;
                        case "up":desktop.Up(c.N(0),c.N(1));break;
                        case "swipe":case "flick":desktop.Swipe(c.N(0),c.N(1),c.N(2),c.N(3),c.Args.Length==5?(int)c.N(4):c.Name=="flick"?180:400,c.Name=="flick"?hold:40,c.Name=="flick");break;
                        case "page":
                            bool right=c.Args[0]=="right";desktop.Swipe(frame.Width*(right?.88:.12),frame.Height*.78,frame.Width*(right?.12:.88),frame.Height*.78,180,hold,true);break;
                        case "type":desktop.Type(c.Args[0]);break;
                        case "key":desktop.Press(Plan.Key(c.Args[0]));break;
                    }
                    Output(new{command=c.Name,status="submitted",physical_result="verify_by_observation"});
                }
                if(c.Name!="down" && c.Name!="move")Idle(pid,session);
            }
            if(desktop!=null) {
                var completed=desktop;desktop=null;completed.Dispose();
                if(completed.RestoreWarning!=null) { Console.Error.WriteLine(Frames.Json.Serialize(new{status="warning",message=completed.RestoreWarning}));return 3; }
            }
            return 0;
        } catch(Exception e) {
            Console.Error.WriteLine(Frames.Json.Serialize(new{status="error",error=e.Message,input_may_have_been_submitted=submitted,replayed=false}));return 1;
        } finally {
            if(desktop!=null){desktop.Dispose();if(desktop.RestoreWarning!=null)Console.Error.WriteLine(Frames.Json.Serialize(new{status="warning",message=desktop.RestoreWarning}));}
        }
    }
    static int Main(string[] args) {
        try {
            if(args.Length==1 && args[0]=="--self-test")return Tests.Run();
            if(args.Length>=1 && args[0]=="--doctor") {
                int selected=0;
                if(args.Length!=1 && (args.Length!=3 || args[1]!="--pid" || !Int32.TryParse(args[2],out selected) || selected<1))throw new ArgumentException("Use --doctor [--pid PID]");
                return Doctor.Run(selected);
            }
            if(args.Length==1 && args[0]=="--version"){Console.WriteLine("iosctl "+FileVersionInfo.GetVersionInfo(typeof(Program).Assembly.Location).ProductVersion);return 0;}
            if(args.Length==0 || (args.Length==1 && args[0]=="--help")) {
                Console.WriteLine("iosctl --pid PID [--targets FILE] [--keep-focus] \"commands\"\nRead README.md for input permissions and examples.\nCommands: status, preflight, shot PATH [scale], crop PATH X Y W H [zoom],\ngrid [threshold], tap X Y, go NAME, home, type 'text', key NAME,\nswipe/flick X1 Y1 X2 Y2 [ms], page left|right, hold MS,\ndown X Y; move X Y; up X Y, wait MS, changed, settle [ms], targets.\nsnapshot PATH [scale] saves a retained image for inspection only; run alone.\nQuotes preserve spaces and semicolons. No automatic retries.\n--doctor [--pid PID] diagnoses setup without input (exit 2: action needed).\n--list lists receiver processes; --self-test sends no input.");return 0;
            }
            if(args.Length==1 && args[0]=="--list") {
                foreach(string name in new[]{"AirWindowsNative","AirWindows"})foreach(var p in Process.GetProcessesByName(name))using(p)Output(new{pid=p.Id,path=p.MainModule.FileName});return 0;
            }
            int pid=0;string path=null;bool keep=false;int at=0;
            while(at<args.Length && args[at].StartsWith("--",StringComparison.Ordinal)) {
                string option=args[at++];
                if(option=="--keep-focus"){keep=true;continue;}
                if(at==args.Length)throw new ArgumentException("Missing option value");
                if(option=="--pid"){if(pid!=0 || !Int32.TryParse(args[at++],out pid) || pid<1)throw new ArgumentException("Use one positive receiver PID");}
                else if(option=="--targets"){if(path!=null)throw new ArgumentException("Use one target map");path=args[at++];}
                else throw new ArgumentException("Unknown option: "+option);
            }
            if(pid<1 || at!=args.Length-1)throw new ArgumentException("Use --pid PID followed by one quoted command batch");
            var plan=new Plan(args[at]);var targets=new Targets(path);
            foreach(var c in plan.Commands)if(c.Name=="go" && !targets.Values.ContainsKey(c.Args[0]))throw new ArgumentException("Unknown target: "+c.Args[0]);
            using(var lease=plan.Input?new InputLease(pid):null)return Execute(pid,plan,targets,keep);
        } catch(Exception e) {Console.Error.WriteLine(Frames.Json.Serialize(new{status="error",error=e.Message,replayed=false}));return 1;}
    }
}
