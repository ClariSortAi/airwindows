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
            {"control_peer","0000000000000001"},{"control_sender","0000000000000002"}};
    }
    static byte[] Ppm() {
        byte[] header=Encoding.ASCII.GetBytes("P6\n2 2\n255\n");var data=new byte[header.Length+12];header.CopyTo(data,0);
        new byte[]{255,0,0,0,255,0,0,0,255,255,255,255}.CopyTo(data,header.Length);return data;
    }
    static void PipeRoundTrip(bool stall) {
        int pid=Process.GetCurrentProcess().Id;Exception error=null;
        using(var server=new NamedPipeServerStream("AirWindows.Frames."+pid,PipeDirection.InOut,1,PipeTransmissionMode.Byte,PipeOptions.Asynchronous)) {
            var worker=new Thread(()=>{
                try {
                    server.WaitForConnection();
                    if(stall){Thread.Sleep(2100);return;}
                    byte[] meta=Encoding.UTF8.GetBytes(Frames.Json.Serialize(Meta(pid,(long)Frames.GetTickCount64()))),ppm=Ppm();
                    byte[] size=BitConverter.GetBytes(meta.Length);server.Write(size,0,4);server.Write(meta,0,meta.Length);server.Write(ppm,0,ppm.Length);
                    if(server.ReadByte()!=75)throw new Exception("Missing frame acknowledgement");
                } catch(Exception e){error=e;}
            });worker.IsBackground=true;worker.Start();
            if(stall)Reject(()=>{using(var f=Frames.Read(pid)){}},"bounded stalled-pipe read");
            else using(var frame=Frames.Read(pid))Check(frame.Width==2 && frame.Image.GetPixel(0,0).R==255,"real pipe and RGB decoding");
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
            string dir=Path.Combine(Path.GetTempPath(),"AirWindows-iosctl-test-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(dir);
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
        PipeRoundTrip(false);PipeRoundTrip(true);
        Program.Output(new{status="passed",checks=count,desktop_input=false,bluetooth=false});return 0;
    }
}
