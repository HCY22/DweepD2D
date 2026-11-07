// List defined strings that look like "CClass::Method" and the functions that reference them.
// @category Dweep
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import java.util.*;
import java.util.regex.*;

public class MethodStrings extends GhidraScript {
    @Override
    public void run() throws Exception {
        Pattern p = Pattern.compile("(C[A-Z]\\w+::~?\\w+)");
        TreeMap<String, TreeSet<String>> byFunc = new TreeMap<>();
        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext()) {
            Data d = it.next();
            if (!d.hasStringValue()) continue;
            String s = String.valueOf(d.getValue());
            Matcher m = p.matcher(s);
            if (!m.find()) continue;
            for (Reference r : getReferencesTo(d.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                String key = f == null ? "(no function) " + r.getFromAddress() : f.getEntryPoint() + " " + f.getName();
                byFunc.computeIfAbsent(key, k -> new TreeSet<>()).add(m.group(1));
            }
        }
        for (Map.Entry<String, TreeSet<String>> e : byFunc.entrySet()) println(e.getKey() + "  " + e.getValue());
    }
}
