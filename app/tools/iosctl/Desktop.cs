// Input for one batch, adapted from Claude Code's iosctl. No Bluetooth service is created
// here: PhonePort retains identity, queue and HID ownership.
//
// Two ways to deliver it:
//   background (default)  the window messages a mouse and keyboard produce are posted to the
//                         receiver's video window. The PC's pointer, keyboard and foreground
//                         window are not used, so the person at the PC keeps working.
//   foreground            real mouse and keyboard events while PhonePort is in front.
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
    [StructLayout(LayoutKind.Sequential)] struct LastInput { public uint size,tick; }
    [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LastInput info);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr FindWindowEx(IntPtr parent,IntPtr after,string name,string title);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr window,out Rect rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(IntPtr window,ref Point point);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr window,uint flags);
    [DllImport("user32.dll")] static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint thread,ref GuiInfo info);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] static extern short GetAsyncKeyState(int key);
    [DllImport("user32.dll",SetLastError=true)] static extern uint SendInput(uint count,Input[] inputs,int size);
    [DllImport("user32.dll",SetLastError=true)] static extern bool PostMessage(IntPtr window,uint message,IntPtr wParam,IntPtr lParam);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint from,uint to,bool attach);
    [DllImport("user32.dll")] static extern IntPtr SetFocus(IntPtr window);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    internal const uint MoveFlag=1,LeftDown=2,LeftUp=4,RightDown=8,RightUp=16;
    const uint KeyUp=2,Unicode=4;
    const uint CharMessage=0x0102,KeyDownMessage=0x0100,KeyUpMessage=0x0101;
    readonly Process process;
    readonly long started;
    readonly IntPtr main,video,previous;
    readonly uint thread;
    readonly bool keepFocus,background;
    Rectangle rectangle;
    int width,height;
    uint heldMouse;
    Keyboard? heldKey;
    bool acquired;
    // Background only: the focus PhonePort had inside its own window before this batch, and
    // whether this batch changed it. The last posted pointer position is where a release goes.
    IntPtr priorFocus;
    bool armed;
    Point last;
    readonly Stopwatch heldClock=new Stopwatch();
    internal string RestoreWarning;
    internal bool Holding { get { return heldMouse!=0; } }

    internal Desktop(int pid,bool keep,bool foreground) {
        process=Process.GetProcessById(pid);started=process.StartTime.ToFileTimeUtc();
        if(!Receiver.IsReceiverName(process.ProcessName)) throw new ArgumentException("PID is not a PhonePort receiver");
        main=process.MainWindowHandle;
        if(main==IntPtr.Zero) throw new InvalidOperationException("Receiver window not found");
        video=FindWindowEx(main,IntPtr.Zero,"AirWindowsVideoWindow",null);
        uint actual;thread=GetWindowThreadProcessId(video,out actual);
        if(video==IntPtr.Zero || actual!=pid) throw new InvalidOperationException("Receiver video window not found");
        previous=GetForegroundWindow();keepFocus=keep;background=!foreground;
    }
    // Foreground: where the picture area is on the desktop. Background: its own client area,
    // which stays the same wherever the window is and whatever covers it.
    Rectangle VideoRect() {
        if(process.HasExited || process.StartTime.ToFileTimeUtc()!=started) throw new InvalidOperationException("Receiver exited or changed");
        uint actual;GetWindowThreadProcessId(video,out actual);
        Rect r;
        if(background) {
            if(actual!=process.Id) throw new InvalidOperationException("Receiver window was replaced");
            // Windows gives no focus to a minimized or hidden window, and the receiver turns
            // phone control on only with it. Behind other windows is fine.
            if(IsIconic(main)) throw new InvalidOperationException("PhonePort is minimized; restore its window, which does not need to be in front");
            if(!IsWindowVisible(video)) throw new InvalidOperationException("PhonePort's window is hidden; show it, which does not need to be in front");
            if(!GetClientRect(video,out r) || r.Right<1 || r.Bottom<1) throw new InvalidOperationException("Invalid video rectangle");
            return new Rectangle(0,0,r.Right,r.Bottom);
        }
        if(actual!=process.Id || !IsWindowVisible(video) || IsIconic(main)) throw new InvalidOperationException("Receiver is hidden, minimized or replaced");
        Point origin=new Point(0,0);
        if(!GetClientRect(video,out r) || !ClientToScreen(video,ref origin) || r.Right<1 || r.Bottom<1) throw new InvalidOperationException("Invalid video rectangle");
        return new Rectangle(origin.X,origin.Y,r.Right,r.Bottom);
    }
    internal static Point Map(Rectangle rect,int w,int h,double x,double y) {
        if(w<1 || h<1 || rect.Width<1 || rect.Height<1 || Double.IsNaN(x) || Double.IsNaN(y) || x<0 || y<0 || x>=w || y>=h) throw new ArgumentException("Coordinates are outside the current phone image");
        double scale=Math.Min(rect.Width/(double)w,rect.Height/(double)h);
        double left=rect.Left+(rect.Width-w*scale)/2,top=rect.Top+(rect.Height-h*scale)/2;
        return new Point((int)Math.Floor(left+(x+.5)*scale),(int)Math.Floor(top+(y+.5)*scale));
    }
    // A client point as a mouse message carries it: x in the low word, y in the high word.
    internal static IntPtr Pack(Point point) { return new IntPtr(((point.Y&0xFFFF)<<16)|(point.X&0xFFFF)); }
    // The window message for one pointer event; 0 for a flag that has none.
    internal static uint MouseMessage(uint flags) {
        switch(flags) {
            case MoveFlag:return 0x0200;case LeftDown:return 0x0201;case LeftUp:return 0x0202;
            case RightDown:return 0x0204;case RightUp:return 0x0205;default:return 0;
        }
    }
    IntPtr ReceiverFocus() {
        var info=new GuiInfo{size=(uint)Marshal.SizeOf(typeof(GuiInfo))};
        if(!GetGUIThreadInfo(thread,ref info)) throw new InvalidOperationException("Could not read PhonePort's focus; no input was sent");
        return info.focus;
    }
    // Sets the focus inside PhonePort's own window. This is not the desktop's foreground
    // window, which stays where the person left it.
    void SetReceiverFocus(IntPtr target) {
        uint self=GetCurrentThreadId();
        if(!AttachThreadInput(self,thread,true))throw new InvalidOperationException("Could not reach the receiver's input queue; no press was sent");
        try { SetFocus(target); } finally { AttachThreadInput(self,thread,false); }
    }
    // How long after the person's last mouse or keyboard use, with PhonePort in front, the
    // phone still counts as theirs. People pause to read between taps, so this is generous.
    internal const int PersonHoldMs=60000;
    // Pure rule, covered by the self-test: PhonePort is the front window and the person used
    // the mouse or keyboard within the hold time. Background input sends no real input
    // events, so the idle clock measures only the person.
    internal static bool PersonHasPhone(bool phonePortInFront,long idleMs) { return phonePortInFront && idleMs>=0 && idleMs<PersonHoldMs; }
    static long PersonIdleMs() {
        var info=new LastInput{size=(uint)Marshal.SizeOf(typeof(LastInput))};
        if(!GetLastInputInfo(ref info))throw new InvalidOperationException("Could not read when the mouse or keyboard was last used; input stopped without replay");
        return unchecked((uint)Environment.TickCount-info.tick);
    }
    // The person at the PC comes first. On 2026-11-20 a scripted tap and Home landed while
    // the owner was using the mirrored phone himself, six seconds after his last click.
    // Scripted input now stands back while PhonePort is the front window and the person is
    // using the PC, and while a mouse button is held on the PhonePort window.
    void PersonCheck() {
        if((GetAsyncKeyState(1)&0x8000)!=0 || (GetAsyncKeyState(2)&0x8000)!=0) {
            Point cursor;
            if(!GetCursorPos(out cursor))throw new InvalidOperationException("A mouse button is pressed and the pointer could not be located; input stopped without replay");
            IntPtr under=WindowFromPoint(cursor);
            if(under!=IntPtr.Zero && GetAncestor(under,2)==main)
                throw new InvalidOperationException("A mouse button is pressed on the PhonePort window: someone is using the phone. Input stopped without replay");
        }
        if(PersonHasPhone(GetForegroundWindow()==main,PersonIdleMs()))
            throw new InvalidOperationException("The PhonePort window is in front and the mouse or keyboard was used in the last minute: someone is using the phone. Input stopped without replay");
    }
    // Background: turn phone control on in the receiver without taking the desktop's
    // foreground. The receiver arms control when its video window gets the focus message.
    void Arm() {
        IntPtr focus=ReceiverFocus();
        // PhonePort was left in front with control on and nobody is at the PC (PersonCheck
        // has passed): use it as it is and change nothing.
        if(focus==video && GetForegroundWindow()==main)return;
        // Focus left on the video window by an earlier run tells the receiver nothing new;
        // take it away first so the receiver sees a fresh focus message.
        priorFocus=focus==video?IntPtr.Zero:focus;
        armed=true;
        if(focus==video)SetReceiverFocus(IntPtr.Zero);
        SetReceiverFocus(video);
        var timer=Stopwatch.StartNew();
        while(ReceiverFocus()!=video) {
            if(timer.ElapsedMilliseconds>500)throw new InvalidOperationException("PhonePort did not turn phone control on; no input was sent");
            Thread.Sleep(10);
        }
    }
    internal void Prepare(PhoneFrame frame) {
        if(heldMouse==0) { rectangle=VideoRect();width=frame.Width;height=frame.Height; }
        else if(width!=frame.Width || height!=frame.Height) throw new InvalidOperationException("Phone rotated during held input");
        if(!acquired) {
            if(background) { PersonCheck();acquired=true;Arm(); }
            else {
                if((GetAsyncKeyState(1)&0x8000)!=0 || (GetAsyncKeyState(2)&0x8000)!=0)throw new InvalidOperationException("Release the mouse buttons before starting scripted input");
                acquired=true;
                if(GetForegroundWindow()!=main && !SetForegroundWindow(main)) throw new InvalidOperationException("Windows denied focus; click PhonePort and try a new action");
                var timer=Stopwatch.StartNew();while(GetForegroundWindow()!=main && timer.ElapsedMilliseconds<500)Thread.Sleep(10);
            }
        }
        Guard(false);
    }
    void Guard(bool keyboard) {
        if(background) {
            PersonCheck();
            if(VideoRect()!=rectangle) throw new InvalidOperationException("PhonePort's picture area changed size during input; observe again");
            if(heldMouse!=0 && heldClock.ElapsedMilliseconds>1500) throw new TimeoutException("Held input exceeded 1.5 seconds");
            // Moves and keys reach the phone only while control is on in the receiver.
            if(keyboard && ReceiverFocus()!=video) throw new InvalidOperationException("Phone control was turned off in PhonePort during input; input stopped without replay");
            return;
        }
        if(GetForegroundWindow()!=main) throw new InvalidOperationException("Focus changed; input stopped without replay");
        if(VideoRect()!=rectangle) throw new InvalidOperationException("Window moved or resized during input; observe again");
        if(heldMouse!=0 && heldClock.ElapsedMilliseconds>1500) throw new TimeoutException("Held input exceeded 1.5 seconds");
        if(keyboard) {
            foreach(int key in new[]{16,17,18,91,92})if((GetAsyncKeyState(key)&0x8000)!=0)throw new InvalidOperationException("A modifier key is held; typing stopped");
            if(ReceiverFocus()!=video) throw new InvalidOperationException("Click the intended phone text field before typing");
        }
    }
    static void Emit(Input input) {
        if(SendInput(1,new[]{input},Marshal.SizeOf(typeof(Input)))!=1) throw new Win32Exception(Marshal.GetLastWin32Error(),"Windows did not accept input; outcome unknown, do not replay");
    }
    void Post(uint message,IntPtr wParam,IntPtr lParam) {
        if(PostMessage(video,message,wParam,lParam))return;
        int code=Marshal.GetLastWin32Error();
        // 5: Windows blocks messages from a normal process to one running as administrator.
        throw new Win32Exception(code,code==5?"Windows refused to deliver input to PhonePort, which may be running as administrator; outcome unknown, do not replay"
            :"PhonePort did not accept input; outcome unknown, do not replay");
    }
    void Pointer(uint flags,double x,double y) {
        Guard(false);Point p=Map(rectangle,width,height,x,y);
        if(background) {
            last=p;
            // The button word says which button is down, as it does for a real mouse.
            int buttons=flags==LeftDown || (flags==MoveFlag && heldMouse==LeftUp)?1:flags==RightDown || (flags==MoveFlag && heldMouse==RightUp)?2:0;
            Post(MouseMessage(flags),new IntPtr(buttons),Pack(p));
            return;
        }
        if(WindowFromPoint(p)!=video) throw new InvalidOperationException("Another window covers the phone target");
        int left=GetSystemMetrics(76),top=GetSystemMetrics(77),w=GetSystemMetrics(78),h=GetSystemMetrics(79);
        if(w<2 || h<2 || p.X<left || p.Y<top || p.X>=left+w || p.Y>=top+h) throw new InvalidOperationException("Phone target lies outside the desktop");
        var input=new Input{type=0};
        input.mouse.x=(int)Math.Round((p.X-left)*65535.0/(w-1));input.mouse.y=(int)Math.Round((p.Y-top)*65535.0/(h-1));
        input.mouse.flags=flags|0x8000|0x4000;
        Emit(input);
    }
    internal void Move(double x,double y) { Guard(true);Pointer(MoveFlag,x,y); }
    // Moves the phone's pointer with no button so a static screen produces new
    // frames. The receiver forwards moves only while the video has focus.
    internal void Wake() {
        if(!background)SetReceiverFocus(video); // background: Prepare has already done it
        Guard(true);
        Pointer(MoveFlag,width/2.0,height/2.0);Thread.Sleep(60);
        Pointer(MoveFlag,width/2.0+16,height/2.0);
    }
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
        if(background) {
            // A character goes as the message a pressed key ends up as. Enter, Tab, Backspace
            // and the arrows go as key down and up, which the receiver maps itself.
            if((flags&Unicode)!=0 || vk==32) { Post(CharMessage,new IntPtr((flags&Unicode)!=0?scan:(ushort)32),new IntPtr(1));return; }
            WithRelease(()=>Post(KeyDownMessage,new IntPtr(vk),new IntPtr(1)),()=>Thread.Sleep(12),()=>Post(KeyUpMessage,new IntPtr(vk),new IntPtr(0xC0000001L)));
            return;
        }
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
        if(background)Post(MouseMessage(heldMouse),IntPtr.Zero,Pack(last));
        else { var input=new Input{type=0};input.mouse.flags=heldMouse;Emit(input); }
        heldMouse=0;heldClock.Reset();
    }
    public void Dispose() {
        try { Release();ReleaseKey(); }
        catch(Exception e) { RestoreWarning="Button/key release was not confirmed: "+e.Message; }
        if(background) {
            // Put PhonePort's own focus back where it was, which turns phone control off
            // again. Not if the person has taken the phone meanwhile: the focus is theirs.
            try { if(armed && ReceiverFocus()==video && !PersonHasPhone(GetForegroundWindow()==main,PersonIdleMs()))SetReceiverFocus(priorFocus); }
            catch(Exception e) { RestoreWarning=(RestoreWarning==null?"":RestoreWarning+" ")+"Input finished; phone control stayed on in PhonePort: "+e.Message; }
        }
        else if(acquired && !keepFocus && previous!=IntPtr.Zero && previous!=main && GetForegroundWindow()==main) {
            if(!SetForegroundWindow(previous))RestoreWarning="Input finished; Windows refused to restore the previous foreground window. Click it to return.";
        }
        process.Dispose();
    }
}
