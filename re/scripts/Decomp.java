// Decompile functions. Args: list of items, each either a hex address (function containing it)
// or "callers:HEX" (every function that references HEX). Output goes to the script log.
// @category Dweep
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.util.*;

public class Decomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        LinkedHashSet<Function> todo = new LinkedHashSet<>();
        for (String a : getScriptArgs()) {
            if (a.startsWith("range:")) {          // range:START-END  every function whose entry lies inside
                String[] p = a.substring(6).split("-");
                Address lo = toAddr(p[0]), hi = toAddr(p[1]);
                for (Function f : currentProgram.getFunctionManager().getFunctions(lo, true)) {
                    if (f.getEntryPoint().compareTo(hi) > 0) break;
                    todo.add(f);
                }
            } else if (a.startsWith("callers:")) {
                Address t = toAddr(a.substring(8));
                for (Reference r : getReferencesTo(t)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f != null) todo.add(f);
                }
            } else {
                Function f = getFunctionContaining(toAddr(a));
                if (f != null) todo.add(f);
            }
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (Function f : todo) {
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            println("\n===== " + f.getName() + " @ " + f.getEntryPoint() + " (size " + f.getBody().getNumAddresses() + ")");
            println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "decompile failed: " + r.getErrorMessage());
        }
        di.dispose();
    }
}
