// CSV report: a million rows of sales read with the language's CSV reader, some fields quoted with
// commas and doubled quotes inside, and summed by region and product and by month: the rows, the
// quantity and the revenue, in the order of the file. Calx uses std.csv, Rust the csv crate, Go
// encoding/csv and Python csv; C has none, so it reads the fields by hand.
use std::collections::HashMap;

#[derive(Default)]
struct Group {
    rows: i64,
    quantity: i64,
    revenue: f64,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let path = std::env::args().nth(1).expect("the input's path");
    let mut reader = csv::Reader::from_path(path)?;
    let mut groups: HashMap<String, Group> = HashMap::new();
    let mut months = [0.0f64; 13];
    for result in reader.records() {
        let row = result?;
        let quantity: i64 = row[3].parse()?;
        let revenue = quantity as f64 * row[4].parse::<f64>()?;
        let g = groups.entry(format!("{}|{}", &row[1], &row[2])).or_default();
        g.rows += 1;
        g.quantity += quantity;
        g.revenue += revenue;
        months[row[0][5..7].parse::<usize>()?] += revenue;
    }
    let mut keys: Vec<&String> = groups.keys().collect();
    keys.sort();
    for k in keys {
        let g = &groups[k];
        println!("{}|{}|{}|{:.2}", k, g.rows, g.quantity, g.revenue);
    }
    for (m, revenue) in months.iter().enumerate().skip(1) {
        println!("month {} {:.2}", m, revenue);
    }
    Ok(())
}
