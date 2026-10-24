// Headless Ghidra post-script for tools/eedecomp.py: decompiles the functions at the given addresses of an already
// analysed program (the EE project tools/ghidra_analyze.py made) into one C file.
// Arguments: <output .c file> <hex address>... The output is derived from game code: keep it under build/analysis/.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class DecompileList extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (int i = 1; i < args.length; ++i) {
                Address a = toAddr(Long.parseLong(args[i].replace("0x", ""), 16));
                Function f = getFunctionContaining(a);
                if (f == null) {
                    out.println("// " + args[i] + ": no function");
                    continue;
                }
                DecompileResults r = decompiler.decompileFunction(f, 120, monitor);
                out.println("// " + f.getEntryPoint() + " " + f.getName());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
            }
        }
    }
}
