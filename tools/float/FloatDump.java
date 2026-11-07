// Writes doubles and floats with what Java 25's Double.toString and Float.toString print for them,
// one "d <bits> <text>" or "f <bits> <text>" line each, for the Calx printer to be checked against.
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class FloatDump {
    static PrintWriter out;
    static long count;

    static void d(double v) {
        if (Double.isNaN(v)) return;
        out.print("d ");
        out.print(Double.doubleToRawLongBits(v));
        out.print(' ');
        out.println(Double.toString(v));
        count++;
    }

    static void f(float v) {
        if (Float.isNaN(v)) return;
        out.print("f ");
        out.print(Float.floatToRawIntBits(v) & 0xFFFFFFFFL);
        out.print(' ');
        out.println(Float.toString(v));
        count++;
    }

    public static void main(String[] args) throws IOException {
        out = new PrintWriter(new BufferedWriter(new OutputStreamWriter(new FileOutputStream(args[0]), StandardCharsets.UTF_8), 1 << 20));
        Random r = new Random(20261107);
        // Random bits.
        for (int i = 0; i < 3_000_000; i++) d(Double.longBitsToDouble(r.nextLong()));
        for (int i = 0; i < 3_000_000; i++) f(Float.intBitsToFloat(r.nextInt()));
        // Every power of two, and its neighbours.
        for (long e = 0; e < 2047; e++) {
            long b = e << 52;
            d(Double.longBitsToDouble(b));
            d(Double.longBitsToDouble(b + 1));
            if (b > 0) d(Double.longBitsToDouble(b - 1));
        }
        for (int e = 0; e < 255; e++) {
            int b = e << 23;
            f(Float.intBitsToFloat(b));
            f(Float.intBitsToFloat(b + 1));
            if (b > 0) f(Float.intBitsToFloat(b - 1));
        }
        // The smallest subnormals, every one, and random ones.
        for (long c = 1; c <= 300_000; c++) d(Double.longBitsToDouble(c));
        for (int c = 1; c <= 300_000; c++) f(Float.intBitsToFloat(c));
        for (int i = 0; i < 300_000; i++) d(Double.longBitsToDouble(r.nextLong() & ((1L << 52) - 1)));
        for (int i = 0; i < 300_000; i++) f(Float.intBitsToFloat(r.nextInt() & ((1 << 23) - 1)));
        // Decimals as a program writes them: integers and short fractions.
        for (int i = 0; i < 1_000_000; i++) {
            long n = r.nextLong() % 10_000_000_000L;
            int k = r.nextInt(23);
            double v = n / Math.pow(10, k);
            d(v);
            f((float) v);
        }
        for (int i = 0; i <= 100_000; i++) {
            d(i);
            d(i / 100.0);
            f(i);
            f(i / 100.0f);
        }
        // Around each power of ten, where the layout and the digit count change.
        for (int k = -325; k <= 309; k++) {
            double p = Double.parseDouble("1e" + k);
            double v = p;
            for (int j = 0; j < 20; j++) v = Math.nextDown(v);
            for (int j = 0; j < 40; j++) {
                d(v);
                v = Math.nextUp(v);
            }
        }
        for (int k = -46; k <= 39; k++) {
            float p = Float.parseFloat("1e" + k);
            float v = p;
            for (int j = 0; j < 20; j++) v = Math.nextDown(v);
            for (int j = 0; j < 40; j++) {
                f(v);
                v = Math.nextUp(v);
            }
        }
        double[] ds = {0.0, -0.0, Double.MIN_VALUE, Double.MIN_NORMAL, Double.MAX_VALUE, 0.1, 0.2, 0.3, 1e23, 2e23, 9.999999999999999e22,
                       Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY, 1e-3, 1e7, 9999999.999999998, 0.001, 0.0009999999999999998};
        for (double v : ds) d(v);
        float[] fs = {0f, -0f, Float.MIN_VALUE, Float.MIN_NORMAL, Float.MAX_VALUE, 0.1f, 1e10f, 1e7f, 1e-3f, Float.POSITIVE_INFINITY};
        for (float v : fs) f(v);
        out.close();
        System.err.println(count + " values");
    }
}
