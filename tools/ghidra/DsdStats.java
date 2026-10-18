// Post-analysis report: functions per memory block, name sources, Function ID matches and a
// decompiler smoke test. Writes the report to the path given as the first script arg.
//@category TM2PSP

import java.io.PrintWriter;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class DsdStats extends GhidraScript {

	@Override
	public void run() throws Exception {
		Map<String, int[]> perBlock = new TreeMap<>(); // total, named, default (FUN_)
		Map<String, Integer> sources = new TreeMap<>();
		StringBuilder fid = new StringBuilder();
		int fidCount = 0;
		FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
		int total = 0;
		while (it.hasNext()) {
			Function f = it.next();
			MemoryBlock b = getMemoryBlock(f.getEntryPoint());
			String key = b == null ? "?" : b.getName();
			int[] c = perBlock.computeIfAbsent(key, k -> new int[3]);
			c[0]++;
			SourceType src = f.getSymbol().getSource();
			sources.merge(src.toString(), 1, Integer::sum);
			if (src == SourceType.DEFAULT) {
				c[2]++;
			}
			else {
				c[1]++;
			}
			// Function ID is the only analyzer here that names functions with ANALYSIS source.
			if (src == SourceType.ANALYSIS) {
				fidCount++;
				fid.append(String.format("  %s %s%n", f.getEntryPoint(), f.getName()));
			}
			total++;
		}

		// Decompiler smoke test over an evenly spaced sample of functions.
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		int sample = 0, ok = 0;
		int step = Math.max(1, total / 200);
		int i = 0;
		it = currentProgram.getFunctionManager().getFunctions(true);
		while (it.hasNext()) {
			Function f = it.next();
			if (i++ % step != 0) {
				continue;
			}
			sample++;
			DecompileResults r = di.decompileFunction(f, 30, monitor);
			if (r != null && r.decompileCompleted()) {
				ok++;
			}
		}
		di.dispose();

		try (PrintWriter w = new PrintWriter(getScriptArgs()[0], "UTF-8")) {
			w.printf("Program: %s%nFunctions: %d%n%n", currentProgram.getName(), total);
			w.println("Per block (total / named / unnamed FUN_):");
			for (Map.Entry<String, int[]> e : perBlock.entrySet()) {
				w.printf("  %-14s %6d %6d %6d%n", e.getKey(), e.getValue()[0], e.getValue()[1], e.getValue()[2]);
			}
			w.println("\nName sources:");
			for (Map.Entry<String, Integer> e : sources.entrySet()) {
				w.printf("  %-14s %6d%n", e.getKey(), e.getValue());
			}
			w.printf("%nFunction ID matches: %d%n%s", fidCount, fid);
			w.printf("%nDecompiler sample: %d/%d decompiled%n", ok, sample);
		}
		println("DsdStats: report written to " + getScriptArgs()[0]);
	}
}
