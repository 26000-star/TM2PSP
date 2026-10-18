// Builds the full TrackMania Turbo ARM9 memory map from a dsd project and applies dsd's symbols.
// Meant to run as a headless -preScript on extract/arm9/arm9.bin imported at 0x02000000
// (BinaryLoader, ARM:LE:32:v5t), so auto-analysis starts from exact function entries and ARM/Thumb modes.
//
// Script args: <project root> [<fidb file>]
//@category TM2PSP

import java.io.File;
import java.math.BigInteger;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.feature.fid.db.FidFile;
import ghidra.feature.fid.db.FidFileManager;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class DsdImport extends GhidraScript {

	private static final Pattern SYMBOL = Pattern.compile(
		"^(\\S+) kind:(function|data|bss|label)(?:\\(([^)]*)\\))?\\s+addr:0x([0-9a-fA-F]+)");

	private Memory mem;
	private Register tmode;
	private int functions, failedFunctions, contextConflicts, labels, internalBranches;

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		File root = new File(args[0]);
		File extract = new File(root, "extract");
		File config = new File(root, "config/arm9");
		mem = currentProgram.getMemory();
		tmode = currentProgram.getRegister("TMode");

		if (args.length > 1) {
			attachFid(new File(args[1]));
		}

		// --- Memory map -----------------------------------------------------------------
		MemoryBlock main = mem.getBlock(toAddr(0x02000000L));
		main.setName("arm9_main");
		main.setExecute(true);
		main.setWrite(true);

		Map<String, String> arm9 = flatYaml(new File(extract, "arm9/arm9.yaml"));
		long bssStart = Long.parseLong(arm9.get("bss_start"));
		long bssEnd = Long.parseLong(arm9.get("bss_end"));
		mem.createUninitializedBlock("arm9_bss", toAddr(bssStart), bssEnd - bssStart, false).setWrite(true);

		addAutoload(new File(extract, "arm9/itcm.bin"), flatYaml(new File(extract, "arm9/itcm.yaml")), "itcm");
		addAutoload(new File(extract, "arm9/dtcm.bin"), flatYaml(new File(extract, "arm9/dtcm.yaml")), "dtcm");

		// Hardware regions, so register and VRAM references resolve instead of dangling.
		hardware("shared_wram", 0x03000000L, 0x00008000L);
		hardware("arm7_wram", 0x03800000L, 0x00010000L);
		hardware("io", 0x04000000L, 0x00110000L);
		hardware("palette", 0x05000000L, 0x00000800L);
		hardware("vram", 0x06000000L, 0x00A00000L);
		hardware("oam", 0x07000000L, 0x00000800L);
		hardware("bios", 0xFFFF0000L, 0x00008000L);

		// Overlays share load addresses, so each one gets its own Ghidra overlay space (code + bss).
		Map<Integer, AddressSpace> overlaySpaces = new HashMap<>();
		for (Map<String, String> ov : overlayList(new File(extract, "arm9_overlays/overlays.yaml"))) {
			int id = Integer.parseInt(ov.get("id"));
			long base = Long.parseLong(ov.get("base_address"));
			int code = Integer.parseInt(ov.get("code_size"));
			int bss = Integer.parseInt(ov.get("bss_size"));
			byte[] bytes = new byte[code + bss];
			byte[] file = Files.readAllBytes(new File(extract, "arm9_overlays/" + ov.get("file_name")).toPath());
			System.arraycopy(file, 0, bytes, 0, Math.min(code, file.length));
			String name = String.format("ov%03d", id);
			MemoryBlock b = mem.createInitializedBlock(name, toAddr(base),
				new java.io.ByteArrayInputStream(bytes), bytes.length, monitor, true);
			b.setExecute(true);
			b.setWrite(true);
			overlaySpaces.put(id, b.getStart().getAddressSpace());
		}

		// --- Symbols --------------------------------------------------------------------
		AddressSpace def = currentProgram.getAddressFactory().getDefaultAddressSpace();
		List<Sym> syms = new ArrayList<>();
		readSymbols(new File(config, "symbols.txt"), def, syms);
		readSymbols(new File(config, "itcm/symbols.txt"), def, syms);
		readSymbols(new File(config, "dtcm/symbols.txt"), def, syms);
		for (Map.Entry<Integer, AddressSpace> e : overlaySpaces.entrySet()) {
			readSymbols(new File(config, String.format("overlays/ov%03d/symbols.txt", e.getKey())), e.getValue(), syms);
		}
		applySymbols(syms);

		println(String.format("DsdImport: %d functions created (%d failed, %d context conflicts), %d labels, %d overlays, %d internal BL branches",
			functions, failedFunctions, contextConflicts, labels, overlaySpaces.size(), internalBranches));
	}

	private void attachFid(File fidb) throws Exception {
		FidFileManager fm = FidFileManager.getInstance();
		for (FidFile f : fm.getFidFiles()) {
			if (f.getPath().equals(fidb.toPath()) || f.getName().equals(fidb.getName())) {
				f.setActive(true);
				println("DsdImport: FID database already attached: " + f.getName());
				return;
			}
		}
		FidFile f = fm.addUserFidFile(fidb);
		println("DsdImport: attached FID database " + (f != null ? f.getName() : "(failed) " + fidb));
	}

	private void addAutoload(File bin, Map<String, String> yaml, String name) throws Exception {
		long base = Long.parseLong(yaml.get("base_address"));
		int code = Integer.parseInt(yaml.get("code_size"));
		int bss = Integer.parseInt(yaml.get("bss_size"));
		byte[] file = Files.readAllBytes(bin.toPath());
		byte[] bytes = new byte[code + bss];
		System.arraycopy(file, 0, bytes, 0, Math.min(code, file.length));
		MemoryBlock b = mem.createInitializedBlock(name, toAddr(base),
			new java.io.ByteArrayInputStream(bytes), bytes.length, monitor, false);
		b.setExecute(name.equals("itcm"));
		b.setWrite(true);
	}

	private void hardware(String name, long start, long size) throws Exception {
		MemoryBlock b = mem.createUninitializedBlock(name, toAddr(start), size, false);
		b.setVolatile(true);
		b.setWrite(true);
	}

	private record Sym(String name, String kind, boolean thumb, Address addr, long size) {
		boolean code() {
			return kind.equals("function") || kind.equals("label");
		}
	}

	private static final Pattern PLACEHOLDER =
		Pattern.compile("^(func|data|bss)_(ov\\d+_)?[0-9a-f]{8}(_unk)?$");

	/** dsd's auto-generated names (func_02012345, data_ov003_020f7000, ...), which carry no information. */
	private static boolean isPlaceholder(String name) {
		return PLACEHOLDER.matcher(name).matches();
	}

	private void readSymbols(File file, AddressSpace space, List<Sym> out) throws Exception {
		if (!file.exists()) {
			return;
		}
		for (String line : Files.readAllLines(file.toPath())) {
			Matcher m = SYMBOL.matcher(line);
			if (m.find()) {
				String attrs = m.group(3) == null ? "" : m.group(3);
				Matcher sz = Pattern.compile("size=0x([0-9a-fA-F]+)").matcher(attrs);
				out.add(new Sym(m.group(1), m.group(2), attrs.startsWith("thumb"),
					space.getAddress(Long.parseLong(m.group(4), 16)), sz.find() ? Long.parseLong(sz.group(1), 16) : 0));
			}
		}
	}

	/**
	 * Pass 1 sets the ARM/Thumb mode at every code entry before anything is disassembled, so flows
	 * never cross an entry with the wrong mode. Pass 2 disassembles and creates the functions.
	 * Pass 3 names data.
	 */
	private void applySymbols(List<Sym> syms) throws Exception {
		for (Sym s : syms) {
			if (s.code()) {
				try {
					currentProgram.getProgramContext().setValue(tmode, s.addr(), s.addr(),
						s.thumb() ? BigInteger.ONE : BigInteger.ZERO);
				}
				catch (Exception e) {
					contextConflicts++;
				}
			}
		}
		for (Sym s : syms) {
			if (s.kind().equals("function")) {
				disassemble(s.addr());
			}
		}
		// Thumb CodeWarrior uses BL for long branches inside a function. Those are jumps, not calls:
		// override their flow so Ghidra keeps one function body instead of splitting it at the target.
		for (Sym s : syms) {
			if (!s.kind().equals("function") || s.size() == 0) {
				continue;
			}
			Address end = s.addr().add(s.size() - 1);
			InstructionIterator ii = currentProgram.getListing().getInstructions(new AddressSet(s.addr(), end), true);
			while (ii.hasNext()) {
				Instruction ins = ii.next();
				if (!ins.getFlowType().isCall()) {
					continue;
				}
				for (Address t : ins.getFlows()) {
					if (t.compareTo(s.addr()) > 0 && t.compareTo(end) <= 0) {
						ins.setFlowOverride(FlowOverride.BRANCH);
						internalBranches++;
						break;
					}
				}
			}
		}
		for (Sym s : syms) {
			monitor.checkCancelled();
			if (!s.kind().equals("function")) {
				continue;
			}
			try {
				Function f = getFunctionAt(s.addr());
				if (f == null) {
					f = createFunction(s.addr(), null);
				}
				if (f == null) {
					failedFunctions++;
				}
				else {
					// Placeholder names stay Ghidra defaults (FUN_<addr>) so Function ID can still rename them.
					if (!isPlaceholder(s.name())) {
						f.setName(s.name(), SourceType.IMPORTED);
					}
					functions++;
				}
			}
			catch (Exception e) {
				failedFunctions++;
				if (failedFunctions <= 20) {
					println("DsdImport: function " + s.name() + " @ " + s.addr() + ": " + e.getMessage());
				}
			}
		}
		for (Sym s : syms) {
			if (s.kind().equals("function") || isPlaceholder(s.name())) {
				continue;
			}
			try {
				createLabel(s.addr(), s.name(), true, SourceType.IMPORTED);
				labels++;
			}
			catch (Exception e) {
				// Duplicate or invalid names are not worth failing the import over.
			}
		}
	}

	/** Reads "key: value" lines of a flat dsd yaml file. */
	private static Map<String, String> flatYaml(File f) throws Exception {
		Map<String, String> out = new HashMap<>();
		for (String line : Files.readAllLines(f.toPath())) {
			int i = line.indexOf(':');
			if (i > 0 && !line.startsWith(" ")) {
				out.put(line.substring(0, i).trim(), line.substring(i + 1).trim());
			}
		}
		return out;
	}

	/** Reads the "overlays:" list of dsd's overlays.yaml ("  - id: 0" starts an entry). */
	private static List<Map<String, String>> overlayList(File f) throws Exception {
		List<Map<String, String>> out = new ArrayList<>();
		Map<String, String> cur = null;
		for (String line : Files.readAllLines(f.toPath())) {
			String t = line.trim();
			if (t.startsWith("- ")) {
				cur = new HashMap<>();
				out.add(cur);
				t = t.substring(2);
			}
			int i = t.indexOf(':');
			if (cur != null && i > 0) {
				cur.put(t.substring(0, i).trim(), t.substring(i + 1).trim());
			}
		}
		return out;
	}
}
