// Post-analysis summary: function count, RTTI-recovered classes, and callers of key imports.
// @category Dweep
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.util.*;

public class Summary extends GhidraScript {
    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        int total = 0, named = 0;
        for (Function f : fm.getFunctions(true)) {
            total++;
            if (!f.getName().startsWith("FUN_")) named++;
        }
        println("functions: " + total + " (named " + named + ")");

        SymbolTable st = currentProgram.getSymbolTable();
        TreeSet<String> classes = new TreeSet<>();
        Iterator<GhidraClass> it = st.getClassNamespaces();
        while (it.hasNext()) classes.add(it.next().getName(true));
        println("classes (" + classes.size() + "):");
        for (String c : classes) if (!c.startsWith("std::")) println("  " + c);

        String[] imports = { "DirectDrawCreate", "DirectSoundCreate", "mciSendStringA", "RegSetValueExA", "RegQueryValueExA", "FindResourceA", "CreateFileA" };
        for (String name : imports) {
            for (Symbol s : st.getExternalSymbols(name)) {
                TreeSet<String> callers = new TreeSet<>();
                for (Reference r : getReferencesTo(s.getAddress())) {
                    // references go through the IAT thunk; follow one level
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f != null) callers.add(f.getName(true) + "@" + f.getEntryPoint());
                    else for (Reference r2 : getReferencesTo(r.getFromAddress())) {
                        Function g = getFunctionContaining(r2.getFromAddress());
                        if (g != null) callers.add(g.getName(true) + "@" + g.getEntryPoint());
                    }
                }
                println(name + " <- " + callers);
            }
        }
    }
}
