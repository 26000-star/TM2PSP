// Exports one JSON object per function (JSON Lines) for offline analysis in tools/analysis:
// entry, block, mode, size, callees, referenced strings, hardware registers touched and
// function pointers taken. Script arg: output path.
//@category TM2PSP

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;

public class ExportAnalysis extends GhidraScript {

	@Override
	public void run() throws Exception {
		Register tmode = currentProgram.getRegister("TMode");
		int n = 0;
		try (PrintWriter w = new PrintWriter(getScriptArgs()[0], "UTF-8")) {
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			while (it.hasNext()) {
				monitor.checkCancelled();
				Function f = it.next();
				Address entry = f.getEntryPoint();
				MemoryBlock blk = getMemoryBlock(entry);
				AddressSetView body = f.getBody();

				Set<String> callees = new TreeSet<>();
				for (Function c : f.getCalledFunctions(monitor)) {
					callees.add(key(c.getEntryPoint()));
				}
				Set<String> strings = new LinkedHashSet<>();
				Set<String> hw = new TreeSet<>();
				Set<String> fptrs = new TreeSet<>();
				Set<String> data = new TreeSet<>();
				int insns = 0;
				InstructionIterator ii = currentProgram.getListing().getInstructions(body, true);
				while (ii.hasNext()) {
					Instruction ins = ii.next();
					insns++;
					for (Reference r : ins.getReferencesFrom()) {
						collect(r.getToAddress(), r.getReferenceType(), strings, hw, fptrs, data, 0);
					}
				}
				boolean thumb = tmode != null && currentProgram.getProgramContext()
						.getValue(tmode, entry, false) != null &&
					currentProgram.getProgramContext().getValue(tmode, entry, false).intValue() == 1;

				StringBuilder sb = new StringBuilder();
				sb.append("{\"addr\":\"").append(key(entry)).append('"');
				sb.append(",\"block\":\"").append(blk == null ? "?" : blk.getName()).append('"');
				sb.append(",\"name\":").append(json(f.getName()));
				sb.append(",\"thumb\":").append(thumb);
				sb.append(",\"size\":").append(body.getNumAddresses());
				sb.append(",\"insns\":").append(insns);
				sb.append(",\"callees\":").append(array(callees));
				sb.append(",\"callers\":").append(f.getSymbol().getReferenceCount());
				sb.append(",\"strings\":").append(array(strings));
				sb.append(",\"hw\":").append(array(hw));
				sb.append(",\"fptrs\":").append(array(fptrs));
				sb.append(",\"data\":").append(array(data));
				sb.append('}');
				w.println(sb);
				n++;
			}
		}
		println("ExportAnalysis: " + n + " functions written");
	}

	/** Follows one level of pointer (literal pool -> string/function) to find what the code really uses. */
	private void collect(Address to, RefType type, Set<String> strings, Set<String> hw, Set<String> fptrs,
			Set<String> data, int depth) {
		if (to == null || !to.isMemoryAddress()) {
			return;
		}
		MemoryBlock b = getMemoryBlock(to);
		if (b == null) {
			return;
		}
		if (b.isVolatile()) {
			hw.add(String.format("%08x", to.getOffset()));
			return;
		}
		Function target = getFunctionAt(to);
		if (target != null && !type.isFlow()) {
			fptrs.add(key(to));
			return;
		}
		if (type.isFlow()) {
			return;
		}
		Data d = getDataAt(to);
		if (d != null && d.hasStringValue()) {
			Object v = d.getValue();
			if (v != null) {
				strings.add(v.toString());
			}
			return;
		}
		if (depth == 0 && d != null && d.isPointer()) {
			Object v = d.getValue();
			if (v instanceof Address) {
				collect((Address) v, RefType.DATA, strings, hw, fptrs, data, 1);
				return;
			}
		}
		if (depth == 0) {
			data.add(key(to));
		}
	}

	private static String key(Address a) {
		String space = a.getAddressSpace().isOverlaySpace() ? a.getAddressSpace().getName() + ":" : "";
		return space + String.format("%08x", a.getOffset());
	}

	private static String array(Set<String> s) {
		StringBuilder sb = new StringBuilder("[");
		boolean first = true;
		for (String v : s) {
			if (!first) {
				sb.append(',');
			}
			sb.append(json(v));
			first = false;
		}
		return sb.append(']').toString();
	}

	private static String json(String s) {
		StringBuilder sb = new StringBuilder("\"");
		for (char c : s.toCharArray()) {
			switch (c) {
				case '"' -> sb.append("\\\"");
				case '\\' -> sb.append("\\\\");
				case '\n' -> sb.append("\\n");
				case '\r' -> sb.append("\\r");
				case '\t' -> sb.append("\\t");
				default -> {
					if (c < 0x20) {
						sb.append(String.format("\\u%04x", (int) c));
					}
					else {
						sb.append(c);
					}
				}
			}
		}
		return sb.append('"').toString();
	}
}
