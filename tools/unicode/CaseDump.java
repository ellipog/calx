import java.io.*;
import java.text.BreakIterator;
import java.util.Locale;

/**
 * What Java does to each code point when it changes case with Locale.ROOT, for gen_case.py.
 *
 * Each line is a code point, what toLowerCase and toUpperCase give for the string of it alone,
 * its word class, and whether it's cased, all in hex but the class. The class is how Java's
 * word BreakIterator treats it, found by asking the iterator about it between letters, digits
 * and itself, since Java lowercases a capital sigma by the words around it. Cased is whether a
 * sigma in a word with it is final or not by it, which is what Java decides that by. Code points whose line would be "same same O 0" are left
 * out. Run with Java 25: java CaseDump.java cases.txt
 */
public class CaseDump {
    static final String KANJI = "\u65E5", HIRAGANA = "\u3042", KATAKANA = "\u30A2", CR = "\r";
    static final String SIGMA = "\u03A3", ALEF = "\u05D0";
    static final char FINAL_SIGMA = '\u03C2';
    static final BreakIterator words = BreakIterator.getWordInstance(Locale.ROOT);

    static boolean boundary(String s, int at) {
        words.setText(s);
        return words.isBoundary(at);
    }

    static String hex(String s) {
        StringBuilder b = new StringBuilder();
        s.codePoints().forEach(c -> b.append(b.length() > 0 ? "," : "").append(Integer.toHexString(c)));
        return b.toString();
    }

    static String wordClass(String c) {
        int n = c.length();
        String sig = "" + bit(boundary("A" + c, 1)) + bit(boundary(c + "A", n)) + bit(boundary("A" + c + "A", 1)) +
                bit(boundary("A" + c + "A", 1 + n)) + bit(boundary("1" + c + "1", 1)) + bit(boundary("1" + c + "1", 1 + n)) +
                bit(boundary(c + c, n)) + bit(boundary(" " + c, 1)) + bit(boundary("A" + c + c + "A", 1)) +
                bit(boundary(KANJI + c, 1)) + bit(boundary(c + KANJI, n));
        switch (sig) {
            // Format characters: no boundary on either side. Past U+FFFF, Java puts one after
            // them inside a word, which gen_case.py's model does for every such code point.
            case "00000000000": case "00010100000": return "XF";
            case "00000001011": case "00010101011": {
                // A letter or a digit: only a letter carries a word on after an apostrophe.
                boolean letter = !boundary("A'" + c, 2);
                boolean digit = !boundary("1," + c, 2);
                if (letter == digit) throw new IllegalStateException(Integer.toHexString(c.codePointAt(0)));
                return letter ? "L" : "D";
            }
            case "01000000001": case "01000010001": case "01010110001": return "X";  // marks
            case "01011111011": return "DANDA";
            case "11000011111": return "MWN";  // between letters, or between digits
            case "11001111111": case "11011111111": return "MW";  // between letters
            case "11110011111": return "MN";  // between digits
            case "11110111111": return "POST";  // after digits
            case "11111100111": return "WS";
            case "11111101100": return "K";
            case "11111101111": {
                if (!boundary(c + HIRAGANA, n)) return "H";
                if (!boundary(c + KATAKANA, n)) return "KA";
                throw new IllegalStateException(Integer.toHexString(c.codePointAt(0)));
            }
            case "11111110111": return c.equals(CR) ? "CR" : "LS";
            case "11111111111": case "11111011111": return "O";  // the second is before digits, which no sigma sees
            default: throw new IllegalStateException(Integer.toHexString(c.codePointAt(0)) + " " + sig);
        }
    }

    static char bit(boolean b) { return b ? '1' : '0'; }

    /** Whether the sigma after an A at the start of `s` lowercases to the final form. */
    static boolean finalFirst(String s) {
        return s.toLowerCase(Locale.ROOT).charAt(1) == FINAL_SIGMA;
    }

    /** Whether the sigma at the end of `s` lowercases to the final form. */
    static boolean finalLast(String s) {
        String l = s.toLowerCase(Locale.ROOT);
        return l.charAt(l.length() - 1) == FINAL_SIGMA;
    }

    /**
     * Whether `c` is cased, found by putting it in a word with a capital sigma: a sigma with a
     * cased letter before it is final, and one with a cased letter after it isn't. The word is
     * made of letters that aren't cased, an alef, around what `c` can be in a word between.
     * Classes that never share a word with a sigma, which is a letter, aren't asked.
     */
    static boolean cased(String c, String cls) {
        switch (cls) {
            case "L": case "X": case "XF": case "DANDA": return !finalFirst("A" + SIGMA + c);
            case "D": return finalLast(ALEF + c + SIGMA);
            case "MW": case "MWN": return finalLast(ALEF + c + ALEF + SIGMA);
            case "MN": return finalLast("1" + c + "1" + SIGMA);
            case "POST": return !finalFirst("A" + SIGMA + "1" + c);
            default: return false;
        }
    }

    public static void main(String[] args) throws IOException {
        try (PrintWriter out = new PrintWriter(new BufferedWriter(new FileWriter(args[0])))) {
            out.println("# " + System.getProperty("java.vendor") + " " + System.getProperty("java.version"));
            for (int cp = 0; cp <= 0x10FFFF; cp++) {
                if (cp >= 0xD800 && cp <= 0xDFFF) continue;
                String c = new String(Character.toChars(cp));
                String lo = c.toLowerCase(Locale.ROOT), up = c.toUpperCase(Locale.ROOT);
                String cls = wordClass(c);
                boolean cased = cased(c, cls);
                if (lo.equals(c) && up.equals(c) && cls.equals("O") && !cased) continue;
                out.println(Integer.toHexString(cp) + " " + (lo.equals(c) ? "same" : hex(lo)) + " " + (up.equals(c) ? "same" : hex(up)) +
                        " " + cls + " " + (cased ? 1 : 0));
            }
        }
    }
}
