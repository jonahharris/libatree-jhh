//! Loads the same DEFS/EXPRS/EVENTS files bench_file reads, inserts them into
//! the Rust `a-tree` crate, and reports insert throughput, search latency and
//! total matches so the two implementations can be compared on identical data.
//!
//!     cargo run --release -- DEFS EXPRS EVENTS [--repeat R]
use a_tree::{ATree, AttributeDefinition};
use std::fs;
use std::time::Instant;

fn lines(path: &str) -> Vec<String> {
    fs::read_to_string(path)
        .unwrap_or_else(|e| panic!("cannot read {path}: {e}"))
        .lines()
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .map(|l| l.to_string())
        .collect()
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut repeat = 1usize;
    let mut files: Vec<&str> = Vec::new();
    let mut i = 1;
    while i < args.len() {
        if args[i] == "--repeat" && i + 1 < args.len() {
            repeat = args[i + 1].parse().unwrap();
            i += 2;
        } else {
            files.push(&args[i]);
            i += 1;
        }
    }
    if files.len() != 3 {
        eprintln!("usage: atree_rust_compare DEFS EXPRS EVENTS [--repeat R]");
        std::process::exit(2);
    }

    // schema
    let mut types = std::collections::HashMap::new();
    let defs: Vec<AttributeDefinition> = lines(files[0])
        .iter()
        .map(|l| {
            let (name, ty) = l.split_once('\t').expect("name<TAB>type");
            let ty = ty.trim();
            types.insert(name.to_string(), ty.to_string());
            match ty {
                "bool" => AttributeDefinition::boolean(name),
                "int" => AttributeDefinition::integer(name),
                "float" => AttributeDefinition::float(name),
                "string" => AttributeDefinition::string(name),
                "int_list" => AttributeDefinition::integer_list(name),
                "string_list" => AttributeDefinition::string_list(name),
                other => panic!("unknown type {other}"),
            }
        })
        .collect();
    let mut atree = ATree::<u64>::new(&defs).expect("ATree::new");

    // expressions
    let exprs = lines(files[1]);
    let mut failures = 0usize;
    let t0 = Instant::now();
    for l in &exprs {
        let (id, text) = l.split_once('\t').expect("id<TAB>expression");
        let id: u64 = id.parse().unwrap();
        if let Err(e) = atree.insert(&id, text) {
            if failures < 3 {
                eprintln!("insert {id} failed: {e}");
            }
            failures += 1;
        }
    }
    let insert_secs = t0.elapsed().as_secs_f64();

    // events (built once, outside the timed region, like bench_file)
    let event_lines = lines(files[2]);
    let mut events = Vec::with_capacity(event_lines.len());
    for l in &event_lines {
        let mut b = atree.make_event();
        for item in l.split(';') {
            let (name, value) = item.split_once('=').expect("attr=value");
            let name = name.trim();
            let value = value.trim();
            match types[name].as_str() {
                "bool" => b.with_boolean(name, value == "true").unwrap(),
                "int" => b.with_integer(name, value.parse().unwrap()).unwrap(),
                "string" => b.with_string(name, value.trim_matches('"')).unwrap(),
                "int_list" => {
                    let v: Vec<i64> = value
                        .trim_matches(|c| c == '[' || c == ']')
                        .split(',')
                        .filter(|s| !s.trim().is_empty())
                        .map(|s| s.trim().parse().unwrap())
                        .collect();
                    b.with_integer_list(name, &v).unwrap()
                }
                "string_list" => {
                    let v: Vec<&str> = value
                        .trim_matches(|c| c == '[' || c == ']')
                        .split(',')
                        .map(|s| s.trim().trim_matches('"'))
                        .filter(|s| !s.is_empty())
                        .collect();
                    b.with_string_list(name, &v).unwrap()
                }
                other => panic!("unsupported event type {other}"),
            }
        }
        events.push(b.build().unwrap());
    }

    // search
    let mut lat: Vec<u64> = Vec::with_capacity(events.len() * repeat);
    let mut total_matches = 0u64;
    for _ in 0..repeat {
        for ev in &events {
            let t = Instant::now();
            let report = atree.search(ev).expect("search");
            let ns = t.elapsed().as_nanos() as u64;
            total_matches += report.matches().len() as u64;
            lat.push(ns);
        }
    }
    lat.sort_unstable();
    let n = lat.len();
    let mean = lat.iter().sum::<u64>() as f64 / n as f64;
    println!("a-tree (Rust crate) benchmark: {} expressions, {} events (x{})", exprs.len(), events.len(), repeat);
    println!(
        "  insert: {:.0} expressions/s, {} failures",
        exprs.len() as f64 / insert_secs,
        failures
    );
    println!(
        "  search: p50 {:.1} us, p99 {:.1} us, mean {:.1} us; per event avg {:.1} matches; total matches {}",
        lat[n / 2] as f64 / 1000.0,
        lat[(n * 99 / 100).min(n - 1)] as f64 / 1000.0,
        mean / 1000.0,
        total_matches as f64 / n as f64,
        total_matches
    );
}
