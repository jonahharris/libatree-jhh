#!/usr/bin/env python3
"""Converts the Rust a-tree crate's benches/data/search.json into the three
plain-text files bench_file reads (defs, exprs, events).

    python3 bench/convert_rust_search_json.py reference/a-tree/benches/data/search.json bench/data/rust_search
"""
import json
import sys


TYPES = {
    "boolean": "bool",
    "integer": "int",
    "float": "float",
    "string": "string",
    "integer_list": "int_list",
    "string_list": "string_list",
}


def fmt_value(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, str):
        return '"' + v.replace('"', "") + '"'
    if isinstance(v, list):
        return "[" + ", ".join(fmt_value(x) for x in v) + "]"
    raise ValueError(type(v))


def main(src, prefix):
    with open(src) as f:
        data = json.load(f)
    with open(prefix + ".defs", "w") as f:
        f.write("# name\ttype\n")
        for name, kind in sorted(data["attributes"].items()):
            f.write(f"{name}\t{TYPES[kind]}\n")
    with open(prefix + ".exprs", "w") as f:
        f.write("# id\texpression\n")
        for e in data["expressions"]:
            f.write(f"{e['id']}\t{e['expression']}\n")
    with open(prefix + ".events", "w") as f:
        f.write("# attr=value;attr=value\n")
        for ev in data["events"]:
            f.write(";".join(f"{k}={fmt_value(v)}" for k, v in sorted(ev.items())) + "\n")
    print(f"{len(data['attributes'])} attributes, {len(data['expressions'])} expressions, "
          f"{len(data['events'])} events -> {prefix}.{{defs,exprs,events}}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
