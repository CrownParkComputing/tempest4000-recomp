package com.crownparkcomputing.ps4runtime;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** Synthetic ZIP/ELF fixtures; no Android runtime, driver execution or game files. */
public final class DriverZipImporterTest {
    private static final String LIB = "libvulkan_freedreno.so";
    private static byte[] elf() {
        byte[] b = new byte[128]; b[0] = 127; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
        b[4] = 2; b[5] = 1; b[6] = 1; b[16] = 3; b[18] = (byte)183;
        return b;
    }
    private static byte[] zip(byte[] meta, byte[] library) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(out)) {
            if (meta != null) { z.putNextEntry(new ZipEntry("meta.json")); z.write(meta); z.closeEntry(); }
            if (library != null) { z.putNextEntry(new ZipEntry(LIB)); z.write(library); z.closeEntry(); }
        }
        return out.toByteArray();
    }
    private static void check(boolean ok, String why) { if (!ok) throw new AssertionError(why); }
    private static void rejects(byte[] archive, File cache, File root, DriverZipImporter.Parser parser) throws Exception {
        try { DriverZipImporter.install(new ByteArrayInputStream(archive), cache, root, 33, parser); }
        catch (IOException expected) { return; }
        throw new AssertionError("Invalid import accepted");
    }
    public static void main(String[] args) throws Exception {
        Path temporary = Files.createTempDirectory("ps4-driver-check-");
        try {
            File cache = temporary.resolve("cache").toFile(), root = temporary.resolve("drivers").toFile();
            DriverZipImporter.Parser metadata = json -> new DriverZipImporter.Metadata(LIB, "Test driver", 28);
            byte[] valid = zip("{}".getBytes(), elf());
            DriverZipImporter.Result first = DriverZipImporter.install(new ByteArrayInputStream(valid), cache, root, 33, metadata);
            check(Arrays.equals(elf(), Files.readAllBytes(first.library.toPath())), "Imported bytes differ");
            long modified = first.library.lastModified();
            DriverZipImporter.Result repeated = DriverZipImporter.install(new ByteArrayInputStream(valid), cache, root, 33, metadata);
            check(first.library.equals(repeated.library) && first.library.lastModified() == modified, "Repeated import rewrote immutable driver");
            byte[] another = elf(); another[100] = 12;
            DriverZipImporter.Result second = DriverZipImporter.install(new ByteArrayInputStream(zip("{}".getBytes(), another)), cache, root, 33, metadata);
            check(!first.library.equals(second.library), "Different driver overwrote loaded path");
            rejects(zip(null, elf()), cache, root, metadata);
            rejects(zip("{}".getBytes(), null), cache, root, metadata);
            rejects(valid, cache, root, json -> { throw new IOException("bad metadata"); });
            rejects(valid, cache, root, json -> new DriverZipImporter.Metadata("../escape.so", "bad", 28));
            rejects(valid, cache, root, json -> new DriverZipImporter.Metadata("/absolute.so", "bad", 28));
            rejects(valid, cache, root, json -> new DriverZipImporter.Metadata(LIB, "too new", 34));
            rejects(zip(new byte[65537], elf()), cache, root, metadata);
            byte[] x86 = elf(); x86[18] = 62;
            rejects(zip("{}".getBytes(), x86), cache, root, metadata);
            byte[] executable = elf(); executable[16] = 2;
            rejects(zip("{}".getBytes(), executable), cache, root, metadata);
            rejects(zip("{}".getBytes(), new byte[20]), cache, root, metadata);
            check(Arrays.equals(elf(), Files.readAllBytes(first.library.toPath())), "Rejected import changed previous driver");
            check(cache.listFiles().length == 0, "Import cache was not cleaned");
            check(Arrays.stream(root.listFiles()).noneMatch(f -> f.getName().startsWith("import-")), "Staging directories leaked");
            if (args.length != 0) {
                try (InputStream in = new FileInputStream(args[0])) {
                    DriverZipImporter.Result actual = DriverZipImporter.install(in, cache, root, 33, metadata);
                    if (args.length > 1) check(args[1].equals(actual.sha256), "Real driver hash differs");
                }
            }
            System.out.println("Driver ZIP checks: PASS (valid/deduplicated imports, immutable paths, malformed archives, traversal, ABI/API guards, cleanup)");
        } finally {
            try (java.util.stream.Stream<Path> paths = Files.walk(temporary)) {
                paths.sorted(Comparator.reverseOrder()).forEach(p -> { try { Files.deleteIfExists(p); } catch (IOException e) { throw new UncheckedIOException(e); } });
            }
        }
    }
}
