using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

// Where the receiver keeps settings.ini, debug.txt and crashes\. An unpackaged receiver
// (ZIP or source build) uses %LOCALAPPDATA%\AirWindows. A receiver installed from the
// Microsoft Store (MSIX) uses its package's local data folder, which Windows keeps at
// %LOCALAPPDATA%\Packages\<package family name>\LocalState and which any program of the
// same user can read.
static class DataFolder {
    const int ErrorInsufficientBuffer=122;
    const uint QueryLimitedInformation=0x1000;
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] static extern int GetCurrentPackageFamilyName(ref uint length,StringBuilder name);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] static extern int GetPackageFamilyName(IntPtr process,ref uint length,StringBuilder name);
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);

    static string Read(Func<uint,StringBuilder,Tuple<int,uint>> call) {
        var first=call(0,null);
        if(first.Item1!=ErrorInsufficientBuffer || first.Item2==0)return null; // APPMODEL_ERROR_NO_PACKAGE and other failures
        var name=new StringBuilder((int)first.Item2);
        var second=call(first.Item2,name);
        return second.Item1==0?name.ToString():null;
    }
    // This iosctl's own package family (started through the iosctl alias of a Store install), or null.
    internal static string OwnFamily() {
        return Read((size,name)=>{uint length=size;int rc=GetCurrentPackageFamilyName(ref length,name);return Tuple.Create(rc,length);});
    }
    const int AppModelErrorNoPackage=15700;
    // The receiver's package family, or null when it is unpackaged. `known` is false when the
    // process could not be inspected (it exited, or access was denied), so null proves nothing.
    internal static string ProcessFamily(int pid,out bool known,out string problem) {
        known=false;problem=null;
        IntPtr handle=OpenProcess(QueryLimitedInformation,false,pid);
        if(handle==IntPtr.Zero){problem="the receiver process could not be opened (error "+Marshal.GetLastWin32Error()+")";return null;}
        try{
            uint length=0;
            int rc=GetPackageFamilyName(handle,ref length,null);
            if(rc==AppModelErrorNoPackage){known=true;return null;}
            if(rc!=ErrorInsufficientBuffer || length==0){problem="its package could not be read (error "+rc+")";return null;}
            var name=new StringBuilder((int)length);
            rc=GetPackageFamilyName(handle,ref length,name);
            if(rc!=0){problem="its package could not be read (error "+rc+")";return null;}
            known=true;return name.ToString();
        }finally{CloseHandle(handle);}
    }
    internal static string ProcessFamily(int pid) { bool known;string problem;return ProcessFamily(pid,out known,out problem); }
    internal static string Classic() {
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"AirWindows");
    }
    internal static string ForFamily(string family) {
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"Packages",family,"LocalState");
    }
    // pid 0: no receiver selected; this iosctl's own package decides. When the receiver cannot
    // be inspected, basis is receiver_unknown and `note` says what was assumed.
    internal static string Resolve(int pid,out string basis,out string note) {
        note=null;
        if(pid<=0) {
            string own=OwnFamily();
            basis=own!=null?"iosctl_package":"unpackaged";
            return own!=null?ForFamily(own):Classic();
        }
        bool known;string problem;
        string family=ProcessFamily(pid,out known,out problem);
        if(family!=null){basis="receiver_package";return ForFamily(family);}
        if(known){basis="receiver_unpackaged";return Classic();}
        basis="receiver_unknown";
        string fallback=OwnFamily();
        note=problem+"; assumed "+(fallback!=null?"the same package as this iosctl":"an unpackaged receiver (ZIP or source build)");
        return fallback!=null?ForFamily(fallback):Classic();
    }
    internal static string Resolve(int pid,out string basis) { string note;return Resolve(pid,out basis,out note); }
}

// Read-only setup diagnosis. Reuse the frame pipe; no service, input or repairs.
static class Doctor {
    internal sealed class Finding {
        public string code, next;
        public bool ready;
    }
    static Finding Result(string code,string next,bool ready=false) {
        return new Finding{code=code,next=next,ready=ready};
    }
    // How to start the receiver with frame access: the Store install has a Start menu entry
    // (which passes --frames) and an airwindows alias; the ZIP has Start-AirWindows.cmd.
    internal static string StartHint(bool packaged) {
        return packaged?"Start AirWindows from the Start menu, or run `airwindows --frames`."
                       :"Extract the package and open Start-AirWindows.cmd. From a Microsoft Store install, start AirWindows from the Start menu, or run `airwindows --frames`.";
    }
    internal static Finding Assess(Dictionary<string,object> m,int pid,long now) {
        Frames.ValidateStatus(m,pid,now);
        if(!Frames.Flag(m,"mirror_active"))return Result("not_mirroring","On iPhone: Control Center > Screen Mirroring > the receiver name shown in AirWindows.");
        if(Frames.Flag(m,"mirror_paused"))return Result("mirror_paused","Unlock the iPhone and resume Screen Mirroring.");
        if(!Frames.Flag(m,"ok"))return Result("no_picture","Wait for the first picture. If it stays blank, stop and reconnect Screen Mirroring.");
        Frames.Validate(m,pid,now,true);
        long tick=Frames.Number(m,"control_tick_ms");
        if(now<tick || now-tick>1000)return Result("control_status_stale","The receiver is not updating its control status. Wait briefly; if it persists, finish active work and restart AirWindows.");
        if(!Frames.Flag(m,"control_ready"))return Result("control_not_ready","Enable AssistiveTouch; pair this PC from iPhone Settings > Bluetooth, then choose the mirrored phone from the phone button in AirWindows.");
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
        if(candidates.Length==0)throw new InvalidOperationException("No AirWindows receiver is running. "+StartHint(DataFolder.OwnFamily()!=null));
        if(candidates.Length>1)throw new InvalidOperationException("Multiple receivers are running. Close extra receivers after finishing active work, or inspect one with --doctor --pid PID. No receiver was selected.");
        return candidates[0];
    }
    internal static int Run(int requested) {
        var candidates=new List<int>();var receivers=new List<object>();
        foreach(string name in new[]{"AirWindowsNative","AirWindows"})foreach(var p in Process.GetProcessesByName(name))using(p) {
            string path=null;
            try{path=p.MainModule.FileName;}catch(System.ComponentModel.Win32Exception){}catch(InvalidOperationException){}
            candidates.Add(p.Id);receivers.Add(new{pid=p.Id,path=path,package=DataFolder.ProcessFamily(p.Id)});
        }
        int pid;
        string basis,folder,note;
        try{pid=Select(candidates.ToArray(),requested);}
        catch(InvalidOperationException e){
            folder=DataFolder.Resolve(0,out basis,out note);
            Program.Output(new{command="doctor",ready=false,code="receiver_selection",next=e.Message,receivers=receivers,
                data_folder=folder,data_folder_basis=basis,data_folder_note=note,data_folder_exists=Directory.Exists(folder),input_sent=false});return 2;
        }
        folder=DataFolder.Resolve(pid,out basis,out note);
        bool packaged=basis=="receiver_package";
        try {
            using(var frame=Frames.Read(pid,false,true)) {
                var result=Assess(frame.Meta,pid,(long)Frames.GetTickCount64());
                Program.Output(new{command="doctor",pid=pid,ready=result.ready,code=result.code,next=result.next,receivers=receivers,
                    data_folder=folder,data_folder_basis=basis,data_folder_note=note,data_folder_exists=Directory.Exists(folder),input_sent=false});
                return result.ready?0:2;
            }
        }catch(Exception e) {
            Program.Output(new{command="doctor",pid=pid,ready=false,code="frame_interface_unavailable",detail=e.Message,
                next="Use the receiver and iosctl from the same current version. Start the receiver with frame access (--frames): "+StartHint(packaged || DataFolder.OwnFamily()!=null)+" Finish active work before restarting it.",
                receivers=receivers,data_folder=folder,data_folder_basis=basis,data_folder_note=note,data_folder_exists=Directory.Exists(folder),input_sent=false});return 2;
        }
    }
}
