import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.HexFormat;

/**
 * What Java does with bytes that may not be UTF-8, for utf8_check.py to compare with Calx.
 *
 * java Utf8Check.java decode: each line of standard input is bytes in hex, and each line out is
 * what new String(bytes, UTF_8) gives, in hex as UTF-8 again.
 *
 * java Utf8Check.java lines: standard input is raw bytes, read as vanilla's console reads them,
 * with a BufferedReader over an InputStreamReader in UTF-8, and each line out is one readLine,
 * in hex as UTF-8.
 */
public class Utf8Check {
    public static void main(String[] args) throws IOException {
        HexFormat hex = HexFormat.of();
        PrintStream out = new PrintStream(new BufferedOutputStream(new FileOutputStream(FileDescriptor.out), 1 << 16), false, StandardCharsets.US_ASCII);
        if (args[0].equals("decode")) {
            BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.US_ASCII));
            String line;
            while ((line = in.readLine()) != null) {
                byte[] bytes = hex.parseHex(line);
                out.println(hex.formatHex(new String(bytes, StandardCharsets.UTF_8).getBytes(StandardCharsets.UTF_8)));
            }
        } else {
            BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
            String line;
            while ((line = in.readLine()) != null) {
                out.println(hex.formatHex(line.getBytes(StandardCharsets.UTF_8)));
            }
        }
        out.flush();
    }
}
