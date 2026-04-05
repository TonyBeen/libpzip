set -euo pipefail

# ===== config =====
INPUT="/home/eular/VSCode/music"
OUTDIR="/tmp/pzip_abc"
RUNS=12          # 总轮次（含 warmup）
WARMUP=2         # 前2次丢弃
BIN="./build/pzip-zip"

# ===== prep =====
mkdir -p "$OUTDIR"
rm -f "$OUTDIR"/*.zip "$OUTDIR"/*_times.txt "$OUTDIR"/*_eval.txt

run_case() {
  local name="$1"
  local cmd="$2"
  local times="$OUTDIR/${name}_times.txt"

  for i in $(seq 1 "$RUNS"); do
    # 每次输出不同文件，避免覆盖影响
    local zip="$OUTDIR/${name}_${i}.zip"
    /usr/bin/time -f "%e" bash -lc "$cmd \"$zip\" \"$INPUT\" -r" 1>/dev/null 2>>"$times"
  done

  tail -n +"$((WARMUP+1))" "$times" > "$OUTDIR/${name}_eval.txt"
}

# ===== run A/B/C =====
# A: lz4
run_case "lz4"  "$BIN --codec lz4"

# B: zstd (当前CLI未暴露级别时，用默认级别)
run_case "zstd" "$BIN --codec zstd"

# C: zlib level 6
run_case "zlib6" "$BIN --codec zlib -l 6"

# ===== stats helpers =====
calc_stats() {
  local f="$1"
  sort -n "$f" | awk '
    {x[NR]=$1}
    END{
      n=NR
      if(n==0){print "n=0 median=NA p95=NA"; exit}
      med=(n%2?x[(n+1)/2]:(x[n/2]+x[n/2+1])/2)
      p=int((95*n+99)/100); if(p<1)p=1; if(p>n)p=n
      printf("n=%d median=%.6f p95=%.6f\n", n, med, x[p])
    }'
}

get_median() {
  local f="$1"
  sort -n "$f" | awk '{x[NR]=$1} END{print (NR%2?x[(NR+1)/2]:(x[NR/2]+x[NR/2+1])/2)}'
}

size_of() {
  local f="$1"
  stat -c %s "$f"
}

# ===== print stats =====
echo "[lz4 stats]";   calc_stats "$OUTDIR/lz4_eval.txt"
echo "[zstd stats]";  calc_stats "$OUTDIR/zstd_eval.txt"
echo "[zlib6 stats]"; calc_stats "$OUTDIR/zlib6_eval.txt"

ML=$(get_median "$OUTDIR/lz4_eval.txt")
MS=$(get_median "$OUTDIR/zstd_eval.txt")
MZ=$(get_median "$OUTDIR/zlib6_eval.txt")

SL=$(size_of "$OUTDIR/lz4_${RUNS}.zip")
SS=$(size_of "$OUTDIR/zstd_${RUNS}.zip")
SZ=$(size_of "$OUTDIR/zlib6_${RUNS}.zip")

echo
echo "[speedup vs zlib6]"
awk -v l="$ML" -v s="$MS" -v z="$MZ" 'BEGIN{
  printf("lz4   speedup=%.2fx  improvement=%.2f%%\n", z/l, (z-l)/z*100)
  printf("zstd  speedup=%.2fx  improvement=%.2f%%\n", z/s, (z-s)/z*100)
}'

echo
echo "[size ratio vs zlib6]"
awk -v l="$SL" -v s="$SS" -v z="$SZ" 'BEGIN{
  printf("lz4   size=%d  ratio=%.3f  delta=%.2f%%\n", l, l/z, (l-z)/z*100)
  printf("zstd  size=%d  ratio=%.3f  delta=%.2f%%\n", s, s/z, (s-z)/z*100)
  printf("zlib6 size=%d  ratio=1.000  delta=0.00%%\n", z)
}'