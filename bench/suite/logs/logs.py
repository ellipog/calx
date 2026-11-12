# Log analysis: a million lines of a web server's access log taken apart by splitting, no regular
# expressions, and summed: requests by status and by method, requests and bytes by path, with
# query strings dropped and numeric ids folded, and by hour, and the distinct client addresses.
import sys


def main():
    lines = 0
    statuses = {}
    methods = {}
    paths = {}
    hours = [[0, 0] for _ in range(24)]
    ips = set()
    with open(sys.argv[1], encoding="utf-8") as f:
        for line in f:
            lines += 1
            ip, rest = line.split(" ", 1)
            ips.add(ip)
            at = rest.index(":")
            hour = int(rest[at + 1 : at + 3])
            _, after = rest.split('"', 1)
            request, tail = after.split('"', 1)
            method, path, _ = request.split(" ")
            q = path.find("?")
            if q >= 0:
                path = path[:q]
            slash = path.rfind("/")
            if path[slash + 1 :].isdigit() and path[slash + 1 :].isascii():
                path = path[: slash + 1] + "{id}"
            fields = tail.strip().split(" ")
            status = int(fields[0])
            size = int(fields[1])
            statuses[status] = statuses.get(status, 0) + 1
            methods[method] = methods.get(method, 0) + 1
            p = paths.get(path)
            if p is None:
                paths[path] = [1, size]
            else:
                p[0] += 1
                p[1] += size
            hours[hour][0] += 1
            hours[hour][1] += size
    out = [f"lines {lines}", f"ips {len(ips)}"]
    for code in sorted(statuses):
        out.append(f"status {code} {statuses[code]}")
    for name in sorted(methods):
        out.append(f"method {name} {methods[name]}")
    for p, (n, size) in sorted(paths.items(), key=lambda e: (-e[1][0], e[0])):
        out.append(f"path {p} {n} {size}")
    for h in range(24):
        out.append(f"hour {h:02d} {hours[h][0]} {hours[h][1]}")
    print("\n".join(out))


main()
