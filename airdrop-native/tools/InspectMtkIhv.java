// Static inspection only: never loads the DLL into a running process.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;
import java.util.*;

public class InspectMtkIhv extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            out.println("Program: " + currentProgram.getExecutablePath());
            out.println("SHA256: " + currentProgram.getExecutableSHA256());
            List<Function> selected = new ArrayList<>();
            for (int i = 1; i < args.length; i++) {
                Function f = currentProgram.getFunctionManager().getFunctionAt(toAddr(args[i]));
                if (f == null) throw new IllegalArgumentException("No function at " + args[i]);
                selected.add(f);
            }
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                if (f.getName().toLowerCase().contains("ihv")) {
                    out.println("EXPORT/CANDIDATE " + f.getEntryPoint() + " " + f.getName());
                    if (args.length == 1 && (f.getName().toLowerCase().contains("control") ||
                        f.getName().toLowerCase().contains("initservice"))) selected.add(f);
                }
            }
            Set<Function> all = new LinkedHashSet<>(selected);
            for (Function f : selected) all.addAll(f.getCalledFunctions(monitor));
            int count = 0;
            for (Function f : all) {
                if (++count > 20) break;
                out.println("\nFUNCTION " + f.getEntryPoint() + " " + f.getName());
                var result = d.decompileFunction(f, 45, monitor);
                if (result.decompileCompleted() && result.getDecompiledFunction() != null)
                    out.println(result.getDecompiledFunction().getC());
                else out.println("DECOMPILE FAILED: " + result.getErrorMessage());
            }
            DataIterator data = currentProgram.getListing().getDefinedData(true);
            count = 0;
            while (data.hasNext() && count < 100) {
                Data value = data.next();
                Object v = value.getValue();
                if (!(v instanceof String)) continue;
                String s = (String)v;
                if (s.toLowerCase().matches(".*(monitor|oid_|ioctl|promiscuous|testmode|raw.?frame).*")) {
                    out.println("STRING " + value.getAddress() + " " + s); count++;
                }
            }
        } finally { d.dispose(); }
    }
}
