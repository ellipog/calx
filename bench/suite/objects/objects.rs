// Many small objects: 20 rounds of 200,000 records, each with an id, a name made with formatting
// and a list of 1 to 8 numbers, three allocations a record. Each round's records are summed, one
// in 16 kept for the rest of the run, and the others freed.
struct Record {
    id: i64,
    name: String,
    values: Vec<i64>,
}

fn main() {
    let mut args = std::env::args().skip(1);
    let rounds: i64 = args.next().and_then(|s| s.parse().ok()).unwrap_or(20);
    let per: i64 = args.next().and_then(|s| s.parse().ok()).unwrap_or(200_000);
    let mut kept: Vec<Record> = Vec::new();
    let mut total = 0i64;
    for round in 0..rounds {
        let mut batch: Vec<Record> = Vec::new();
        for i in 0..per {
            let id = round * per + i;
            let values: Vec<i64> = (1..2 + id % 8).map(|k| id * k).collect();
            batch.push(Record { id, name: format!("item-{}", id), values });
        }
        for r in &batch {
            total += r.values.iter().sum::<i64>() + r.name.len() as i64;
        }
        kept.extend(batch.into_iter().filter(|r| r.id % 16 == 0));
    }
    let names: usize = kept.iter().map(|r| r.name.len()).sum();
    let values: i64 = kept.iter().map(|r| r.values.iter().sum::<i64>()).sum();
    println!("total {}\nkept {} {} {}", total, kept.len(), names, values);
}
