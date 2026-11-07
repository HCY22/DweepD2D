// For each function in a range (range:START-END) or listed address: signature, #call sites, calling functions.
// @category Dweep
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.util.*;

public class XRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        List<Function> fs = new ArrayList<>();
        for (String a : getScriptArgs()) {
            if (a.startsWith("range:")) {
                String[] p = a.substring(6).split("-");
                Address hi = toAddr(p[1]);
                for (Function f : currentProgram.getFunctionManager().getFunctions(toAddr(p[0]), true)) {
                    if (f.getEntryPoint().compareTo(hi) > 0) break;
                    fs.add(f);
                }
            } else fs.add(getFunctionContaining(toAddr(a)));
        }
        for (Function f : fs) {
            int n = 0;
            TreeSet<String> callers = new TreeSet<>();
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                n++;
                Function c = getFunctionContaining(r.getFromAddress());
                callers.add(c == null ? r.getFromAddress().toString() : c.getEntryPoint().toString());
            }
            println(f.getEntryPoint() + " size=" + f.getBody().getNumAddresses() + " refs=" + n + " params=" + f.getParameterCount()
                    + " callers=" + (callers.size() > 12 ? callers.size() + " functions" : callers.toString()));
        }
    }
}
