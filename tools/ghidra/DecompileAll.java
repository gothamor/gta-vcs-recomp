// Headless Ghidra post-script for tools/irxdis.py --decompile: names the functions listed in a symbols file
// ("hex_address name" per line), creates any Ghidra missed, and writes every function's decompiled C to one file.
// Arguments: <symbols file> <output .c file>. The output is derived from game code: keep it under build/analysis/.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.PrintWriter;

public class DecompileAll extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (BufferedReader in = new BufferedReader(new FileReader(args[0]))) {
            String line;
            while ((line = in.readLine()) != null) {
                String[] parts = line.trim().split("\\s+");
                if (parts.length < 2)
                    continue;
                Address a = toAddr(Long.parseLong(parts[0], 16));
                disassemble(a);
                Function f = getFunctionAt(a);
                if (f == null)
                    f = createFunction(a, parts[1]);
                if (f != null)
                    f.setName(parts[1], SourceType.USER_DEFINED);
            }
        }
        analyzeChanges(currentProgram);

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[1])) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.getName().startsWith("imp_"))
                    continue; // import stubs
                DecompileResults r = decompiler.decompileFunction(f, 60, monitor);
                out.println("// " + f.getEntryPoint());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
            }
        }
    }
}
