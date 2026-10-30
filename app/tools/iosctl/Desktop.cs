// Focused desktop input adapted from Claude Code's iosctl. No Bluetooth service
// is created here: AirWindows retains identity, queue and HID ownership.
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;

sealed class Desktop : IDisposable {
    [StructLayout(LayoutKind.Sequential)] internal struct Rect { public int Left,Top,Right,Bottom; }
    [StructLayout(LayoutKind.Sequential)] internal struct Mouse { public int x,y;public uint data,flags,time;public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] internal struct Keyboard { public ushort vk,scan;public uint flags,time;public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit,Size=40)] internal struct Input {
        [FieldOffset(0)] public uint type;
        [FieldOffset(8)] public Mouse mouse;
        [FieldOffset(8)] public Keyboard keyboard;
    }
    [StructLayout(LayoutKind.Sequential)] struct GuiInfo { public uint size,flags; public IntPtr active,focus,capture,menu,move,caret;public Rect caretRect; }
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr FindWindowEx(IntPtr parent,IntPtr after,string name,string title);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr window,out Rect rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr window,ref Point point);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint thread,ref GuiInfo info);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] static extern short GetAsyncKeyState(int key);
    [DllImport("user32.dll",SetLastError=true)] static extern uint SendInput(uint count,Input[] inputs,int size);
    const uint MoveFlag=1,LeftDown=2,LeftUp=4,RightDown=8,RightUp=16,KeyUp=2,Unicode=4;
    readonly Process process;
    readonly long started;
    readonly IntPtr main,video,previous;
    readonly uint thread;
    readonly bool keepFocus;
    Rectangle rectangle;
    int width,height;
    uint heldMouse;
    Keyboard? heldKey;
    bool acquired;
    readonly Stopwatch heldClock=new Stopwatch();
    internal string RestoreWarning;
    internal bool Holding { get { return heldMouse!=0; } }

    internal Desktop(int pid,bool keep) {
        process=Process.GetProcessById(pid);started=process.StartTime.ToFileTimeUtc();
        if(process.ProcessName!="AirWindowsNative" && process.ProcessName!="AirWindows") throw new ArgumentException("PID is not an AirWindows receiver");
        main=process.MainWindowHandle;
        if(main==IntPtr.Zero) throw new InvalidOperationException("Receiver window not found");
        video=FindWindowEx(main,IntPtr.Zero,"AirWindowsVideoWindow",null);
        uint actual;thread=GetWindowThreadProcessId(video,out actual);
        if(video==IntPtr.Zero || actual!=pid) throw new InvalidOperationException("Receiver video window not found");
        previous=GetForegroundWindow();keepFocus=keep;
    }
    Rectangle VideoRect() {
        if(process.HasExited || process.StartTime.ToFileTimeUtc()!=started) throw new InvalidOperationException("Receiver exited or changed");
        uint actual;GetWindowThreadProcessId(video,out actual);
        if(actual!=process.Id || !IsWindowVisible(video) || IsIconic(main)) throw new InvalidOperationException("Receiver is hidden, minimized or replaced");
        Rect r;Point origin=new Point(0,0);
        if(!GetClientRect(video,out r) || !ClientToScreen(video,ref origin) || r.Right<1 || r.Bottom<1) throw new InvalidOperationException("Invalid video rectangle");
        return new Rectangle(origin.X,origin.Y,r.Right,r.Bottom);
    }
    internal static Point Map(Rectangle rect,int w,int h,double x,double y) {
        if(w<1 || h<1 || rect.Width<1 || rect.Height<1 || Double.IsNaN(x) || Double.IsNaN(y) || x<0 || y<0 || x>=w || y>=h) throw new ArgumentException("Coordinates are outside the current phone image");
        double scale=Math.Min(rect.Width/(double)w,rect.Height/(double)h);
        double left=rect.Left+(rect.Width-w*scale)/2,top=rect.Top+(rect.Height-h*scale)/2;
        return new Point((int)Math.Floor(left+(x+.5)*scale),(int)Math.Floor(top+(y+.5)*scale));
    }
    internal void Prepare(PhoneFrame frame) {
        if(heldMouse==0) { rectangle=VideoRect();width=frame.Width;height=frame.Height; }
        else if(width!=frame.Width || height!=frame.Height) throw new InvalidOperationException("Phone rotated during held input");
        if(!acquired) {
            if((GetAsyncKeyState(1)&0x8000)!=0 || (GetAsyncKeyState(2)&0x8000)!=0)throw new InvalidOperationException("Release the mouse buttons before starting scripted input");
            acquired=true;
            if(GetForegroundWindow()!=main && !SetForegroundWindow(main)) throw new InvalidOperationException("Windows denied focus; click AirWindows and try a new action");
            var timer=Stopwatch.StartNew();while(GetForegroundWindow()!=main && timer.ElapsedMilliseconds<500)Thread.Sleep(10);
        }
        Guard(false);
    }
    void Guard(bool keyboard) {
        if(GetForegroundWindow()!=main) throw new InvalidOperationException("Focus changed; input stopped without replay");
        if(VideoRect()!=rectangle) throw new InvalidOperationException("Window moved or resized during input; observe again");
        if(heldMouse!=0 && heldClock.ElapsedMilliseconds>1500) throw new TimeoutException("Held input exceeded 1.5 seconds");
        if(keyboard) {
            foreach(int key in new[]{16,17,18,91,92})if((GetAsyncKeyState(key)&0x8000)!=0)throw new InvalidOperationException("A modifier key is held; typing stopped");
            var info=new GuiInfo{size=(uint)Marshal.SizeOf(typeof(GuiInfo))};
            if(!GetGUIThreadInfo(thread,ref info) || info.focus!=video) throw new InvalidOperationException("Click the intended phone text field before typing");
        }
    }
    static void Emit(Input input) {
        if(SendInput(1,new[]{input},Marshal.SizeOf(typeof(Input)))!=1) throw new Win32Exception(Marshal.GetLastWin32Error(),"Windows did not accept input; outcome unknown, do not replay");
    }
    void Pointer(uint flags,double x,double y) {
        Guard(false);Point p=Map(rectangle,width,height,x,y);
        if(WindowFromPoint(p)!=video) throw new InvalidOperationException("Another window covers the phone target");
        int left=GetSystemMetrics(76),top=GetSystemMetrics(77),w=GetSystemMetrics(78),h=GetSystemMetrics(79);
        if(w<2 || h<2 || p.X<left || p.Y<top || p.X>=left+w || p.Y>=top+h) throw new InvalidOperationException("Phone target lies outside the desktop");
        var input=new Input{type=0};
        input.mouse.x=(int)Math.Round((p.X-left)*65535.0/(w-1));input.mouse.y=(int)Math.Round((p.Y-top)*65535.0/(h-1));
        input.mouse.flags=flags|0x8000|0x4000;
        Emit(input);
    }
    internal void Move(double x,double y) { Guard(true);Pointer(MoveFlag,x,y); }
    internal void Down(double x,double y,bool right=false) {
        if(heldMouse!=0)throw new InvalidOperationException("A mouse button is already held");
        Pointer(MoveFlag,x,y);Thread.Sleep(40);
        heldMouse=right?RightUp:LeftUp;heldClock.Restart();Pointer(right?RightDown:LeftDown,x,y);
    }
    internal void Up(double x,double y) {
        if(heldMouse==0)throw new InvalidOperationException("No mouse button is held");
        Pointer(MoveFlag,x,y);Release();
    }
    // This finally is shared by taps, drags and key events and is tested without
    // sending any desktop input. Release also runs if a press only partly fails.
    internal static void WithRelease(Action press,Action body,Action release) {
        try { press();body(); } finally { release(); }
    }
    internal void Tap(double x,double y,bool right=false) {
        WithRelease(()=>Down(x,y,right),()=>Thread.Sleep(80),Release);
    }
    internal void Swipe(double x1,double y1,double x2,double y2,int duration,int dwell,bool linear) {
        // Validate the destination before the first press.
        Map(rectangle,width,height,x2,y2);
        WithRelease(()=>Down(x1,y1),()=>{
            Thread.Sleep(dwell);int steps=Math.Max(6,duration/35);
            for(int i=1;i<=steps;i++) {
                double t=i/(double)steps,e=linear?t:Math.Sin(t*Math.PI/2);
                Pointer(MoveFlag,x1+(x2-x1)*e,y1+(y2-y1)*e);Thread.Sleep(Math.Max(1,duration/steps));
            }
        },Release);
    }
    void Key(ushort vk,ushort scan,uint flags) {
        Guard(true);
        var key=new Keyboard{vk=vk,scan=scan,flags=flags};
        WithRelease(()=>{heldKey=key;Emit(new Input{type=1,keyboard=key});},()=>Thread.Sleep(12),ReleaseKey);
    }
    internal void Type(string text) {
        foreach(char ch in text) {
            if(ch=='\n')Key(13,0,0);else if(ch=='\t')Key(9,0,0);else Key(0,ch,Unicode);
            Thread.Sleep(30);
        }
    }
    internal void Press(ushort vk) { Key(vk,0,0); }
    void ReleaseKey() {
        if(!heldKey.HasValue)return;
        var key=heldKey.Value;key.flags|=KeyUp;Emit(new Input{type=1,keyboard=key});heldKey=null;
    }
    internal void Release() {
        if(heldMouse==0)return;
        var input=new Input{type=0};input.mouse.flags=heldMouse;Emit(input);heldMouse=0;heldClock.Reset();
    }
    public void Dispose() {
        try { Release();ReleaseKey(); }
        catch(Exception e) { RestoreWarning="Button/key release was not confirmed: "+e.Message; }
        if(acquired && !keepFocus && previous!=IntPtr.Zero && previous!=main && GetForegroundWindow()==main) {
            if(!SetForegroundWindow(previous))RestoreWarning="Input finished; Windows refused to restore the previous foreground window. Click it to return.";
        }
        process.Dispose();
    }
}
