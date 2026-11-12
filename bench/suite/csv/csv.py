# CSV report: a million rows of sales read with the language's CSV reader, some fields quoted with
# commas and doubled quotes inside, and summed by region and product and by month: the rows, the
# quantity and the revenue, in the order of the file. Calx uses std.csv, Rust the csv crate, Go
# encoding/csv and Python csv; C has none, so it reads the fields by hand.
import csv
import sys


def main():
    groups = {}
    months = [0.0] * 13
    with open(sys.argv[1], newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        next(reader)
        for row in reader:
            quantity = int(row[3])
            revenue = quantity * float(row[4])
            key = f"{row[1]}|{row[2]}"
            g = groups.get(key)
            if g is None:
                groups[key] = [1, quantity, revenue]
            else:
                g[0] += 1
                g[1] += quantity
                g[2] += revenue
            months[int(row[0][5:7])] += revenue
    for key in sorted(groups):
        rows, quantity, revenue = groups[key]
        print(f"{key}|{rows}|{quantity}|{revenue:.2f}")
    for m in range(1, 13):
        print(f"month {m} {months[m]:.2f}")


main()
