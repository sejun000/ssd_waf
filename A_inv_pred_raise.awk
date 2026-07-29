# RAISE-conditioned inv prediction error.
# Row m's invpred cols = prediction formed at tick (m-1) vs actual over (m-1, m].
# So "prediction AT a RAISE tick t, actual at next tick t+1" = measurement rows m
# whose PREVIOUS row (m-1 = t) had decision==RAISE.  (optionally also dC[t]>0)
#   col43 pred_full (=invrate_sum[t]*dts, the rule signal), col44 pred_surv,
#   col45 actual, col46 nseg, col47 nsurv, col11 decision, col17 comp_cum.
BEGIN { FS=" " }
NR==1 { next }
{
  comp=$17+0; dec=$11; pf=$43+0; ps=$44+0; ac=$45+0; nseg=$46+0; nsurv=$47+0;

  if (have_prev && nseg>0) {
    # ALL ticks (unconditioned) — for contrast
    A_pf+=pf; A_ps+=ps; A_ac+=ac; A_n++;
    # RAISE-conditioned: previous row (the prediction tick) was RAISE
    if (prev_dec=="RAISE") {
      R_pf+=pf; R_ps+=ps; R_ac+=ac; R_n++;
      if (ac>0){ R_m++; e=(ps-ac)/ac; if(e<0)e=-e; R_are+=e; if(ps>ac)R_over++; }
    }
    # RAISE && dC>0 at the prediction tick (mirrors the gud filter)
    if (prev_dec=="RAISE" && prev_dcpos) {
      RD_pf+=pf; RD_ps+=ps; RD_ac+=ac; RD_n++;
    }
  }
  # stash THIS row as the next iteration's "prediction tick"
  thisdcpos = (have_comp && (comp-prevcomp)>0) ? 1 : 0;
  prev_dec=dec; prev_dcpos=thisdcpos; prevcomp=comp; have_comp=1; have_prev=1;
}
END {
  printf "%-9s\n", tr;
  if (A_n>0)
    printf "   ALL        n=%-5d FULL ratio=%.3f (%+.1f%%)  SURV ratio=%.3f (%+.1f%%)\n",
           A_n, A_pf/A_ac, 100*(A_pf/A_ac-1), A_ps/A_ac, 100*(A_ps/A_ac-1);
  if (R_n>0)
    printf "   RAISE      n=%-5d FULL ratio=%.3f (%+.1f%%)  SURV ratio=%.3f (%+.1f%%)  | tick absRE=%.0f%% over=%.0f%%\n",
           R_n, R_pf/R_ac, 100*(R_pf/R_ac-1), R_ps/R_ac, 100*(R_ps/R_ac-1), 100*R_are/R_m, 100*R_over/R_m;
  if (RD_n>0)
    printf "   RAISE&dC>0 n=%-5d FULL ratio=%.3f (%+.1f%%)  SURV ratio=%.3f (%+.1f%%)\n",
           RD_n, RD_pf/RD_ac, 100*(RD_pf/RD_ac-1), RD_ps/RD_ac, 100*(RD_ps/RD_ac-1);
}
