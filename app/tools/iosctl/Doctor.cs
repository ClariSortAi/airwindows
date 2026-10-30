using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;

// Read-only setup diagnosis. Reuse the frame pipe; no service, input or repairs.
static class Doctor {
    internal sealed class Finding {
        public string code, next;
        public bool ready;
    }
    static Finding Result(string code,string next,bool ready=false) {
        return new Finding{code=code,next=next,ready=ready};
    }
    internal static Finding Assess(Dictionary<string,object> m,int pid,long now) {
        Frames.ValidateStatus(m,pid,now);
        if(!Frames.Flag(m,"mirror_active"))return Result("not_mirroring","On iPhone: Control Center > Screen Mirroring > the receiver name shown in AirWindows.");
        if(Frames.Flag(m,"mirror_paused"))return Result("mirror_paused","Unlock the iPhone and resume Screen Mirroring.");
        if(!Frames.Flag(m,"ok"))return Result("no_picture","Wait for the first picture. If it stays blank, stop and reconnect Screen Mirroring.");
        Frames.Validate(m,pid,now,true);
        long tick=Frames.Number(m,"control_tick_ms");
        if(now<tick || now-tick>1000)return Result("control_status_stale","The receiver is not updating its control status. Wait briefly; if it persists, finish active work and restart AirWindows.");
        if(!Frames.Flag(m,"control_ready"))return Result("control_not_ready","Enable AssistiveTouch; pair this PC from iPhone Settings > Bluetooth, then select the mirrored phone and click Connect in AirWindows.");
        Frames.RequireControl(m,now);
        if(Frames.Flag(m,"control_busy"))return Result("input_busy","Wait for the current input to finish. Inspect the result before starting another batch; do not replay it.");
        if(now-Frames.Number(m,"received_tick_ms")>1000 || now-Frames.Number(m,"decoded_tick_ms")>1000)
            return Result("idle_image","Use snapshot PATH.png to inspect the retained image. For input or result verification, obtain a fresh shot or use your live desktop tool.");
        Frames.Validate(m,pid,now);
        return Result("ready","Use preflight, then observe, act once and verify. Physical input and Button 2 Home mapping still require a phone check.",true);
    }
    internal static int Select(int[] candidates,int requested) {
        if(requested!=0) {
            if(!candidates.Contains(requested))throw new InvalidOperationException("The requested PID is not a running AirWindows receiver. Run --list again.");
            return requested;
        }
        if(candidates.Length==0)throw new InvalidOperationException("No AirWindows receiver is running. Extract the package and open Start-AirWindows.cmd.");
        if(candidates.Length>1)throw new InvalidOperationException("Multiple receivers are running. Close extra receivers after finishing active work, or inspect one with --doctor --pid PID. No receiver was selected.");
        return candidates[0];
    }
    internal static int Run(int requested) {
        var candidates=new List<int>();var receivers=new List<object>();
        foreach(string name in new[]{"AirWindowsNative","AirWindows"})foreach(var p in Process.GetProcessesByName(name))using(p) {
            string path=null;
            try{path=p.MainModule.FileName;}catch(System.ComponentModel.Win32Exception){}catch(InvalidOperationException){}
            candidates.Add(p.Id);receivers.Add(new{pid=p.Id,path=path});
        }
        int pid;
        try{pid=Select(candidates.ToArray(),requested);}
        catch(InvalidOperationException e){Program.Output(new{command="doctor",ready=false,code="receiver_selection",next=e.Message,receivers=receivers,input_sent=false});return 2;}
        try {
            using(var frame=Frames.Read(pid,false,true)) {
                var result=Assess(frame.Meta,pid,(long)Frames.GetTickCount64());
                Program.Output(new{command="doctor",pid=pid,ready=result.ready,code=result.code,next=result.next,receivers=receivers,input_sent=false});
                return result.ready?0:2;
            }
        }catch(Exception e) {
            Program.Output(new{command="doctor",pid=pid,ready=false,code="frame_interface_unavailable",detail=e.Message,
                next="Use the receiver and iosctl from the same current package. Start the receiver with Start-AirWindows.cmd (--frames); finish active work before restarting it.",input_sent=false});return 2;
        }
    }
}
