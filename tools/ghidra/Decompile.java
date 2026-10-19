// Decompiles selected functions to C. Args: <output file> <selector> [<selector> ...]
// A selector is a function name, an address (0x02012345 or ov003:0x020f8000), or
// "str:<text>" for every function that references a string containing <text>, or
// "callers:<selector>" for the callers of the selected functions.
// With an output file ending in ".asm", writes the disassembly of the functions instead (address, bytes,
// instruction), which shows the arguments the decompiler sometimes drops.
//@category TM2PSP

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class Decompile extends GhidraScript {

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		Set<Function> targets = new LinkedHashSet<>();
		for (int i = 1; i < args.length; i++) {
			targets.addAll(select(args[i]));
		}
		if (args[0].endsWith(".asm")) {
			try (PrintWriter w = new PrintWriter(args[0], "UTF-8")) {
				for (Function f : targets) {
					w.printf("// ==== %s @ %s ====%n", f.getName(), f.getEntryPoint());
					InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
					while (it.hasNext()) {
						Instruction ins = it.next();
						w.printf("%s  %s%n", ins.getAddress(), ins.toString());
					}
				}
			}
			println("Decompile: " + targets.size() + " functions disassembled to " + args[0]);
			return;
		}
		DecompInterface di = new DecompInterface();
		DecompileOptions opts = new DecompileOptions();
		di.setOptions(opts);
		di.openProgram(currentProgram);
		try (PrintWriter w = new PrintWriter(args[0], "UTF-8")) {
			for (Function f : targets) {
				DecompileResults r = di.decompileFunction(f, 60, monitor);
				w.printf("// ==== %s @ %s (%s) ====%n", f.getName(), f.getEntryPoint(),
					getMemoryBlock(f.getEntryPoint()).getName());
				if (r != null && r.decompileCompleted()) {
					w.println(r.getDecompiledFunction().getC());
				}
				else {
					w.println("// decompilation failed: " + (r == null ? "?" : r.getErrorMessage()));
				}
			}
		}
		di.dispose();
		println("Decompile: " + targets.size() + " functions written to " + args[0]);
	}

	private List<Function> select(String sel) throws Exception {
		List<Function> out = new ArrayList<>();
		if (sel.startsWith("callers:")) {
			for (Function f : select(sel.substring(8))) {
				for (Reference r : getReferencesTo(f.getEntryPoint())) {
					Function c = getFunctionContaining(r.getFromAddress());
					if (c != null && !out.contains(c)) {
						out.add(c);
					}
				}
			}
			return out;
		}
		if (sel.startsWith("str:")) {
			String needle = sel.substring(4);
			DataIterator it = currentProgram.getListing().getDefinedData(true);
			while (it.hasNext()) {
				Data d = it.next();
				if (!d.hasStringValue() || d.getValue() == null || !d.getValue().toString().contains(needle)) {
					continue;
				}
				for (Reference r : getReferencesTo(d.getAddress())) {
					addContaining(out, r.getFromAddress());
					// Literal pools: follow one more level (pointer -> string).
					for (Reference r2 : getReferencesTo(r.getFromAddress())) {
						addContaining(out, r2.getFromAddress());
					}
				}
			}
			return out;
		}
		if (sel.matches("(?i)(\\w+:)?0x[0-9a-f]+")) {
			Address a;
			if (sel.contains(":") && !sel.startsWith("0x")) {
				String[] p = sel.split(":");
				AddressSpace sp = currentProgram.getAddressFactory().getAddressSpace(p[0]);
				a = sp.getAddress(Long.parseLong(p[1].substring(2), 16));
			}
			else {
				a = toAddr(Long.parseLong(sel.substring(2), 16));
			}
			Function f = getFunctionContaining(a);
			if (f != null) {
				out.add(f);
			}
			return out;
		}
		for (Symbol s : currentProgram.getSymbolTable().getSymbols(sel)) {
			Function f = getFunctionAt(s.getAddress());
			if (f != null) {
				out.add(f);
			}
		}
		return out;
	}

	private void addContaining(List<Function> out, Address a) {
		Function f = getFunctionContaining(a);
		if (f != null && !out.contains(f)) {
			out.add(f);
		}
	}
}
