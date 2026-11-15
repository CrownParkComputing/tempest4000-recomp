package com.crownparkcomputing.ps4runtime;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.*;
import java.util.zip.*;

/** Import only a validated ARM64 driver, without extracting other ZIP members. */
public final class DriverZipImporter {
    public static final long MAX_ARCHIVE = 128L * 1024 * 1024;
    public static final long MAX_LIBRARY = 64L * 1024 * 1024;
    public interface Parser { Metadata parse(String json) throws Exception; }
    public static final class Metadata {
        public final String library, name;
        public final int minApi;
        public Metadata(String library, String name, int minApi) {
            this.library = library; this.name = name; this.minApi = minApi;
        }
    }
    public static final class Result {
        public final File library;
        public final String name, sha256;
        Result(File library, String name, String sha256) {
            this.library = library; this.name = name; this.sha256 = sha256;
        }
    }
    private static long copy(InputStream in, OutputStream out, long limit, MessageDigest hash) throws IOException {
        byte[] block = new byte[65536]; long count = 0; int n;
        while ((n = in.read(block)) != -1) {
            count += n;
            if (count > limit) throw new IOException("Driver exceeds the supported size limit");
            out.write(block, 0, n);
            if (hash != null) hash.update(block, 0, n);
        }
        return count;
    }
    private static String hex(byte[] bytes) {
        StringBuilder s = new StringBuilder();
        for (byte b : bytes) s.append(String.format("%02x", b & 255));
        return s.toString();
    }
    private static void clean(File folder) {
        File[] files = folder.listFiles();
        if (files != null) for (File f : files) { if (f.isDirectory()) clean(f); else f.delete(); }
        folder.delete();
    }
    public static Result install(InputStream source, File cache, File root, int api, Parser parser) throws IOException {
        Files.createDirectories(cache.toPath());
        Files.createDirectories(root.toPath());
        File archive = File.createTempFile("gpu-driver-", ".zip", cache);
        File staging = null;
        try {
            try (OutputStream out = new FileOutputStream(archive)) { copy(source, out, MAX_ARCHIVE, null); }
            try (ZipFile zip = new ZipFile(archive)) {
                ZipEntry metaEntry = zip.getEntry("meta.json");
                if (metaEntry == null || metaEntry.isDirectory()) throw new IOException("Driver ZIP needs meta.json");
                ByteArrayOutputStream metaBytes = new ByteArrayOutputStream();
                try (InputStream in = zip.getInputStream(metaEntry)) { copy(in, metaBytes, 65536, null); }
                String json = new String(metaBytes.toByteArray(), StandardCharsets.UTF_8);
                Metadata meta;
                try { meta = parser.parse(json); }
                catch (Exception e) { throw new IOException("Invalid driver metadata", e); }
                if (meta == null || meta.library == null || !meta.library.matches("[A-Za-z0-9][A-Za-z0-9._-]{0,119}\\.so"))
                    throw new IOException("Driver library must be a plain .so filename");
                if (meta.minApi < 0 || meta.minApi > api) throw new IOException("Driver requires a newer Android version");
                ZipEntry entry = zip.getEntry(meta.library);
                if (entry == null || entry.isDirectory()) throw new IOException("Driver library is missing");
                if (entry.getSize() > MAX_LIBRARY) throw new IOException("Driver library is too large");
                staging = Files.createTempDirectory(root.toPath(), "import-").toFile();
                File library = new File(staging, meta.library);
                MessageDigest hash;
                try { hash = MessageDigest.getInstance("SHA-256"); }
                catch (NoSuchAlgorithmException e) { throw new IOException(e); }
                try (InputStream in = zip.getInputStream(entry); FileOutputStream out = new FileOutputStream(library)) {
                    byte[] elf = new byte[64]; int used = 0, n;
                    while (used < elf.length && (n = in.read(elf, used, elf.length - used)) > 0) used += n;
                    if (used != 64 || elf[0] != 127 || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F'
                            || elf[4] != 2 || elf[5] != 1 || elf[6] != 1 || elf[16] != 3 || elf[17] != 0 || (elf[18] & 255) != 183 || elf[19] != 0)
                        throw new IOException("Driver must be an ARM64 little-endian shared library");
                    out.write(elf); hash.update(elf);
                    copy(in, out, MAX_LIBRARY - elf.length, hash);
                    out.getFD().sync();
                }
                String digest = hex(hash.digest());
                Files.write(new File(staging, "meta.json").toPath(), metaBytes.toByteArray());
                File target = new File(root, digest + "-" + meta.library);
                File result = new File(target, meta.library);
                if (target.exists()) {
                    MessageDigest existing;
                    try { existing = MessageDigest.getInstance("SHA-256"); }
                    catch (NoSuchAlgorithmException e) { throw new IOException(e); }
                    try (InputStream in = new FileInputStream(result)) { copy(in, new OutputStream() { public void write(int b) {} public void write(byte[] b, int o, int n) {} }, MAX_LIBRARY, existing); }
                    if (!digest.equals(hex(existing.digest()))) throw new IOException("Stored driver integrity check failed");
                } else {
                    try { Files.move(staging.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE); }
                    catch (AtomicMoveNotSupportedException e) { Files.move(staging.toPath(), target.toPath()); }
                }
                return new Result(result, meta.name == null ? "Imported driver" : meta.name, digest);
            }
        } finally {
            archive.delete();
            if (staging != null && staging.exists()) clean(staging);
        }
    }
}
