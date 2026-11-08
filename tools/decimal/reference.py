"""The reference side of tools/decimal/check.calx: CPython's decimal module.

Each line of input is `op|prec|rounding|a|b`, and each line of output is what Python gives, or
`error` when it raises; for `powi` and `pow`, what it gives correctly rounded. The context's exponents go as
far as libmpdec's allow, as std.decimal's do.
"""
import decimal
import struct
import sys
from decimal import Decimal as D

ROUNDINGS = {
    "HalfEven": decimal.ROUND_HALF_EVEN,
    "HalfUp": decimal.ROUND_HALF_UP,
    "HalfDown": decimal.ROUND_HALF_DOWN,
    "Up": decimal.ROUND_UP,
    "Down": decimal.ROUND_DOWN,
    "Ceiling": decimal.ROUND_CEILING,
    "Floor": decimal.ROUND_FLOOR,
    "ZeroFiveUp": decimal.ROUND_05UP,
}


def run(op, a, b):
    if op == "add":
        return str(D(a) + D(b))
    if op == "sub":
        return str(D(a) - D(b))
    if op == "mul":
        return str(D(a) * D(b))
    if op == "div":
        return str(D(a) / D(b))
    if op == "rem":
        return str(D(a) % D(b))
    if op == "neg":
        return str(-D(a))
    if op == "abs":
        return str(abs(D(a)))
    if op == "sqrt":
        return str(D(a).sqrt())
    if op == "exp":
        return str(D(a).exp())
    if op == "ln":
        return str(D(a).ln())
    if op == "log10":
        return str(D(a).log10())
    if op == "powi":
        # Python's integer powers are only almost always correctly rounded: libmpdec multiplies at
        # a few more digits and rounds twice. std.decimal's are correctly rounded, so the
        # reference is the power at 3000 digits, exact for every result that short, rounded once.
        ctx = decimal.getcontext()
        with decimal.localcontext() as wide:
            wide.prec = 3000
            r = D(a) ** int(b)
        return str(ctx.plus(r))
    if op == "pow":
        # Almost always correctly rounded too, and not for exact results in the directed roundings:
        # 4 ** 1.5 rounded down is 7.999...9. The reference is the power at 50 more digits, rounded
        # once.
        ctx = decimal.getcontext()
        with decimal.localcontext() as wide:
            wide.prec = ctx.prec + 50
            wide.rounding = decimal.ROUND_HALF_EVEN
            r = D(a) ** D(b)
        return str(ctx.plus(r))
    if op == "quantize":
        return str(D(a).quantize(D(b)))
    if op == "round":
        return str(round(D(a), int(b)))
    if op == "integral":
        return str(D(a).to_integral_value())
    if op == "normalize":
        return str(D(a).normalize())
    if op == "cmp":
        x, y = D(a), D(b)
        return str((x > y) - (x < y)) + " " + str(x == y).lower()
    if op == "str":
        return str(D(a))
    if op == "plain":
        return format(D(a), "f")
    if op == "float":
        return "%016x" % struct.unpack("<Q", struct.pack("<d", float(D(a))))[0]
    if op == "int":
        return str(int(D(a)))
    if op == "fromfloat":
        return str(D(struct.unpack("<d", struct.pack("<Q", int(a, 16)))[0]))
    raise ValueError(op)


def main():
    out = []
    for line in sys.stdin:
        op, prec, rounding, a, b = line.rstrip("\n").split("|")
        ctx = decimal.Context(prec=int(prec), rounding=ROUNDINGS[rounding], Emax=decimal.MAX_EMAX,
                              Emin=decimal.MIN_EMIN)
        decimal.setcontext(ctx)
        try:
            out.append(run(op, a, b))
        except (decimal.DecimalException, ValueError, OverflowError):
            out.append("error")
    sys.stdout.write("\n".join(out) + "\n")


main()
