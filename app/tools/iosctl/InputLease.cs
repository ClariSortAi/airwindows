using System;
using System.Threading;

// Serialize input helpers per receiver. Observation remains independent.
sealed class InputLease : IDisposable {
    Mutex gate;
    internal InputLease(int pid) {
        gate=new Mutex(false,"Local\\AirWindows.IosCtl.Input."+pid);
        try {
            if(!gate.WaitOne(0))throw new InvalidOperationException("Another iosctl command is controlling this receiver; observe before a new command");
        } catch(AbandonedMutexException) {
            gate.ReleaseMutex();gate.Dispose();gate=null;
            throw new InvalidOperationException("Previous input helper ended unexpectedly; inspect the phone and mouse-button state before continuing");
        } catch {gate.Dispose();gate=null;throw;}
    }
    public void Dispose() {if(gate!=null){gate.ReleaseMutex();gate.Dispose();gate=null;}}
}
