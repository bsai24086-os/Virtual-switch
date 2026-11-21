#!/bin/bash
# Runs the MAC table benchmark and writes bench_results.csv.
#   ./bench.sh            full run (about 1 minute)
#   ./bench.sh quick      short run, to check that everything works
# Then: python3 plot.py   (draws bench_results.png)
set -u
cd "$(dirname "$0")"

OPS=500000; REPS=3
[ "${1:-}" = "quick" ] && { OPS=50000; REPS=1; }

make bench_global bench_striped bench_rwlock >/dev/null || exit 1

OUT=bench_results.csv
echo "mode,threads,read_pct,mops" > $OUT
for pct in 95 50; do
  for t in 1 2 4 8 16; do
    for mode in global striped rwlock; do
      sum=0
      for r in $(seq $REPS); do
        line=$(./bench_$mode $t $OPS $pct) || { echo "bench_$mode failed"; exit 1; }
        v=${line##*,}
        sum=$(echo "$sum + $v" | bc -l)
      done
      avg=$(echo "scale=3; $sum / $REPS" | bc -l)
      echo "$mode,$t,$pct,$avg" | tee -a $OUT
    done
  done
done
