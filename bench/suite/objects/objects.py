# Many small objects: 20 rounds of 200,000 records, each with an id, a name made with formatting
# and a list of 1 to 8 numbers, three allocations a record. Each round's records are summed, one
# in 16 kept for the rest of the run, and the others freed.
import sys


class Record:
    __slots__ = ("id", "name", "values")

    def __init__(self, id, name, values):
        self.id = id
        self.name = name
        self.values = values


def main():
    rounds = int(sys.argv[1])
    per = int(sys.argv[2])
    kept = []
    total = 0
    for round in range(rounds):
        batch = []
        for i in range(per):
            id = round * per + i
            batch.append(Record(id, f"item-{id}", [id * k for k in range(1, 2 + id % 8)]))
        for r in batch:
            total += sum(r.values) + len(r.name)
        kept.extend(r for r in batch if r.id % 16 == 0)
    names = sum(len(r.name) for r in kept)
    values = sum(sum(r.values) for r in kept)
    print(f"total {total}\nkept {len(kept)} {names} {values}")


main()
