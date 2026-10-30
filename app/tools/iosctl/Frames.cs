// AirWindows GPL-3.0-or-later. Phone-only capture; never captures the desktop.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Web.Script.Serialization;

sealed class PhoneFrame : IDisposable {
    public Dictionary<string,object> Meta;
    public Bitmap Image;
    public int Width { get { return Image.Width; } }
    public int Height { get { return Image.Height; } }
    public void Dispose() { if(Image!=null) Image.Dispose(); }
}

static class Frames {
    internal static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength=32768 };
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetNamedPipeServerProcessId(IntPtr pipe,out uint pid);
    [DllImport("kernel32.dll")] internal static extern ulong GetTickCount64();
    internal static long Number(Dictionary<string,object> m,string key) {
        object value;
        if(!m.TryGetValue(key,out value) || !(value is int || value is long)) throw new InvalidDataException("Invalid integer: "+key);
        return Convert.ToInt64(value);
    }
    internal static string Text(Dictionary<string,object> m,string key) {
        object value;
        if(!m.TryGetValue(key,out value) || !(value is string)) throw new InvalidDataException("Invalid string: "+key);
        return (string)value;
    }
    internal static bool Flag(Dictionary<string,object> m,string key) {
        object value;
        if(!m.TryGetValue(key,out value) || !(value is bool)) throw new InvalidDataException("Invalid flag: "+key);
        return (bool)value;
    }
    internal static void ValidateStatus(Dictionary<string,object> m,int pid,long now) {
        if(!m.ContainsKey("status_tick_ms"))throw new InvalidDataException("Diagnostics and idle snapshots require AirWindows 0.3.1-rc.2 or later");
        if(Number(m,"protocol")!=1 || Text(m,"source")!="airplay" || Number(m,"receiver_pid")!=pid)throw new InvalidDataException("Wrong receiver or status protocol");
        ulong instance;
        if(!UInt64.TryParse(Text(m,"instance"),out instance) || instance==0)throw new InvalidDataException("Invalid receiver instance");
        long age=now-Number(m,"status_tick_ms");
        if(age<0 || age>1000)throw new InvalidDataException("Receiver status is stale");
    }
    internal static void Validate(Dictionary<string,object> m,int pid,long now,bool retained=false) {
        if(!Flag(m,"ok")) throw new InvalidDataException("No fresh phone image: "+Text(m,"error"));
        if(retained) {
            ValidateStatus(m,pid,now);
            if(!Flag(m,"mirror_active") || Flag(m,"mirror_paused"))throw new InvalidDataException("No active mirror snapshot");
        }
        if(Number(m,"protocol")!=1 || Text(m,"source")!="airplay" || Text(m,"format")!="ppm") throw new InvalidDataException("Unsupported frame source/protocol");
        if(Number(m,"receiver_pid")!=pid) throw new InvalidDataException("Receiver PID changed");
        ulong instance;
        if(!UInt64.TryParse(Text(m,"instance"),out instance) || instance==0) throw new InvalidDataException("Invalid receiver instance");
        foreach(string key in new[]{"generation","sequence","received_tick_ms","decoded_tick_ms"})
            if(Number(m,key)<=0) throw new InvalidDataException("Invalid "+key);
        foreach(string key in new[]{"received_tick_ms","decoded_tick_ms"}) {
            long age=now-Number(m,key);
            if(age<0 || (!retained && age>1000)) throw new InvalidDataException("Frame is stale or has an invalid clock; use snapshot for inspection only");
        }
        if(Number(m,"received_tick_ms")>Number(m,"decoded_tick_ms")) throw new InvalidDataException("Decode precedes receipt");
        long w=Number(m,"width"), h=Number(m,"height"), bytes=Number(m,"image_bytes");
        if(w<1 || h<1 || w>4096 || h>4096 || bytes<1 || bytes>51000000) throw new InvalidDataException("Frame exceeds limits");
    }
    internal static void RequireControl(Dictionary<string,object> m,long now) {
        if(!m.ContainsKey("control_ready")) throw new InvalidDataException("Input requires AirWindows 0.3.1 or later");
        if(!Flag(m,"control_ready")) throw new InvalidDataException("Selected phone control is not ready");
        long age=now-Number(m,"control_tick_ms");
        if(age<0 || age>1000 || Number(m,"control_epoch")<1) throw new InvalidDataException("Control status is stale");
        foreach(string key in new[]{"control_peer","control_sender"}) {
            string token=Text(m,key);
            if(token.Length!=16 || !System.Text.RegularExpressions.Regex.IsMatch(token,"^[0-9a-f]{16}$")) throw new InvalidDataException("Missing selected phone identity");
        }
    }
    internal static bool Drained(Dictionary<string,object> m,long after) {
        if(Number(m,"control_tick_ms")<after)return false;
        if(!Flag(m,"control_focused"))throw new InvalidOperationException("Receiver stopped accepting focused input; inspect the result before another action");
        return !Flag(m,"control_busy");
    }
    static byte[] ReadExact(Stream pipe,int size,Stopwatch clock) {
        var data=new byte[size];int offset=0;
        while(offset<size) {
            var task=pipe.ReadAsync(data,offset,size-offset);
            if(!task.Wait(Math.Max(0,1800-(int)clock.ElapsedMilliseconds))) {
                pipe.Dispose(); // cancel pending I/O before its completion resources die
                try{task.Wait(1000);}catch(AggregateException){}
                throw new TimeoutException("Frame read timed out");
            }
            int count=task.GetAwaiter().GetResult();
            if(count==0) throw new EndOfStreamException("Incomplete frame");
            offset+=count;
        }
        return data;
    }
    internal static Bitmap Decode(byte[] ppm,int width,int height) {
        byte[] header=Encoding.ASCII.GetBytes("P6\n"+width+" "+height+"\n255\n");
        if(ppm.Length!=header.Length+checked(width*height*3)) throw new InvalidDataException("Frame payload size mismatch");
        for(int i=0;i<header.Length;i++) if(ppm[i]!=header[i]) throw new InvalidDataException("PPM header mismatch");
        var image=new Bitmap(width,height,PixelFormat.Format24bppRgb);
        try {
            var bits=image.LockBits(new Rectangle(0,0,width,height),ImageLockMode.WriteOnly,PixelFormat.Format24bppRgb);
            try {
                byte[] row=new byte[width*3];
                for(int y=0;y<height;y++) {
                    for(int x=0;x<width;x++) { int p=header.Length+(y*width+x)*3;row[x*3]=ppm[p+2];row[x*3+1]=ppm[p+1];row[x*3+2]=ppm[p]; }
                    Marshal.Copy(row,0,IntPtr.Add(bits.Scan0,y*bits.Stride),row.Length);
                }
            } finally { image.UnlockBits(bits); }
            return image;
        } catch { image.Dispose();throw; }
    }
    internal static PhoneFrame Read(int pid,bool retained=false,bool inspect=false) {
        using(var pipe=new NamedPipeClientStream(".","AirWindows.Frames."+pid,PipeDirection.InOut,PipeOptions.Asynchronous)) {
            pipe.Connect(1200);
            uint actual;
            if(!GetNamedPipeServerProcessId(pipe.SafePipeHandle.DangerousGetHandle(),out actual) || actual!=pid) throw new InvalidDataException("Wrong frame-pipe server");
            var clock=Stopwatch.StartNew();
            uint size=BitConverter.ToUInt32(ReadExact(pipe,4,clock),0);
            if(size<2 || size>16384) throw new InvalidDataException("Invalid metadata length");
            var meta=Json.Deserialize<Dictionary<string,object>>(Encoding.UTF8.GetString(ReadExact(pipe,(int)size,clock)));
            if(inspect)ValidateStatus(meta,pid,(long)GetTickCount64());
            else Validate(meta,pid,(long)GetTickCount64(),retained);
            long bytes=Number(meta,"image_bytes");
            if(bytes<0 || bytes>51000000)throw new InvalidDataException("Frame exceeds limits");
            byte[] data=ReadExact(pipe,(int)Number(meta,"image_bytes"),clock);
            var ack=pipe.WriteAsync(new byte[]{75},0,1);
            if(!ack.Wait(300)) {
                pipe.Dispose();try{ack.Wait(1000);}catch(AggregateException){}
                throw new TimeoutException("Frame acknowledgement timed out");
            }
            ack.GetAwaiter().GetResult();
            if(inspect) {
                ValidateStatus(meta,pid,(long)GetTickCount64());
                return new PhoneFrame{Meta=meta};
            }
            Validate(meta,pid,(long)GetTickCount64(),retained);
            var image=Decode(data,(int)Number(meta,"width"),(int)Number(meta,"height"));
            try { Validate(meta,pid,(long)GetTickCount64(),retained);return new PhoneFrame{Meta=meta,Image=image}; }
            catch { image.Dispose();throw; }
        }
    }
}

sealed class FrameSession {
    string identity;
    internal void Check(PhoneFrame frame,bool input) {
        var m=frame.Meta;
        string key=Frames.Text(m,"instance")+":"+Frames.Number(m,"generation")+":"+frame.Width+":"+frame.Height;
        if(input) {
            Frames.RequireControl(m,(long)Frames.GetTickCount64());
            key+=":"+Frames.Number(m,"control_epoch")+":"+Frames.Text(m,"control_peer")+":"+Frames.Text(m,"control_sender");
        }
        if(identity==null) identity=key;
        else if(identity!=key) throw new InvalidDataException("Phone, receiver or geometry changed; observe again before new input");
    }
}
