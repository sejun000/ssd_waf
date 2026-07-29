# gsdec analysis: at RAISE ticks with dC(=Δcomp_cum col17)>0, compare
#   gud (col7) vs compact_avg (col23)   [same tick]
#   gud (col7) vs raw_compact (col49)   [NEXT tick]
# Run once per file with -v tr=<name>.
BEGIN { FS=" " }
NR==1 { next }                      # skip header
{
  ts=$1; gud=$7+0; dec=$11; comp=$17+0; cavg=$23+0; craw=$49+0;
  nrows++;
  if (dec=="RAISE") nraise++;

  # resolve a pending qualifying row: this row is its t+1 -> use this row's raw_compact
  if (pend) {
    nr++; sgr+=pend_gud; scr+=craw; sdr+=(pend_gud-craw);
    if (pend_gud>craw) ngtr++;
    if (craw>0) ncrpos++;
    pend=0;
  }
  if (have_prev) {
    dC = comp - prev_comp;
    if (dC>0) ndc++;
    if (dec=="RAISE" && dC>0) {
      n++; sg+=gud; sca+=cavg; sdiff+=(gud-cavg);
      if (gud>cavg) ngt++;
      if (n==1 || gud<gmin) gmin=gud;
      if (n==1 || gud>gmax) gmax=gud;
      pend=1; pend_gud=gud;
    }
  }
  prev_comp=comp; have_prev=1;
}
END {
  printf "%-9s rows=%d RAISE=%d dC>0=%d | ", tr, nrows, nraise, ndc;
  if (n>0) {
    printf "qual(RAISE&dC>0) n=%d  gud=%.5f cavg=%.5f  Δ(g-ca)=%+.5f  g>ca=%.0f%%  [gud %.4f..%.4f]",
           n, sg/n, sca/n, sdiff/n, 100.0*ngt/n, gmin, gmax;
    if (nr>0)
      printf "  ||  next-tick nraw=%d craw=%.5f Δ(g-craw)=%+.5f  g>craw=%.0f%%  craw>0=%.0f%%",
             nr, scr/nr, sdr/nr, 100.0*ngtr/nr, 100.0*ncrpos/nr;
    printf "\n";
  } else {
    printf "no qualifying rows\n";
  }
}
