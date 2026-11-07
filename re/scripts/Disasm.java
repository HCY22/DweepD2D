// Disassemble. Args: "ADDR" (whole function containing ADDR) or "ADDR:N" (first N instructions from ADDR).
// @category Dweep
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;

public class Disasm extends GhidraScript {
    @Override
    public void run() throws Exception {
        Listing listing = currentProgram.getListing();
        java.util.List<String> items = new java.util.ArrayList<>();
        for (String a : getScriptArgs()) for (String s : a.trim().split("\\s+")) if (!s.isEmpty()) items.add(s);
        for (String a : items) {
            String[] p = a.split(":");
            Address start = toAddr(p[0]);
            int limit = p.length > 1 ? Integer.parseInt(p[1]) : Integer.MAX_VALUE;
            Function f = getFunctionContaining(start);
            println("---- " + a + (f != null ? " in " + f.getName() : ""));
            InstructionIterator it = p.length > 1 ? listing.getInstructions(start, true)
                                                  : listing.getInstructions(f.getBody(), true);
            int n = 0;
            while (it.hasNext() && n++ < limit) {
                Instruction ins = it.next();
                StringBuilder hex = new StringBuilder();
                for (byte b : ins.getBytes()) hex.append(String.format("%02X", b));
                println(ins.getAddress() + "  " + String.format("%-16s", hex) + " " + ins);
            }
        }
    }
}
