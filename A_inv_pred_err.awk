# invalidate-rate prediction error from gsdec cols 43-47.
#   pred_full(43)=invrate_sum*dts (whole cohort, what the rule uses)
#   pred_surv(44)=Σ rate*dts over SURVIVING segs ; actual(45)=Σ real invalidations on them
#   nseg(46)/nsurv(47) = cohort size / survivors
# Apples-to-apples error = pred_surv vs actual. full vs actual shows churn inflation.
BEGIN { FS=" " }
NR==1 { next }
{
  pf=$43+0; ps=$44+0; ac=$45+0; nseg=$46+0; nsurv=$47+0;
  if (nseg>0) {
    nt++; Spf+=pf; Sps+=ps; Sac+=ac; Snseg+=nseg; Snsurv+=nsurv;
    if (ac>0) {
      m++; e=(ps-ac)/ac; se+=e; ae+=(e<0?-e:e);
      ef=(pf-ac)/ac; sef+=ef;
      if (ps>ac) over++;
    } else {
      zero++;
    }
  }
}
END {
  if (nt>0) {
    printf "%-9s | vt=%d  surv_frac=%.1f%%  act=0_ticks=%d\n", tr, nt, 100.0*Snsurv/Snseg, zero+0;
    printf "          SURV  Σpred=%.0f Σact=%.0f  ratio=%.3f (%+.1f%%)  | per-tick(m=%d): signedRE=%+.1f%% absRE=%.1f%% over=%.0f%%\n",
           Sps, Sac, Sps/Sac, 100.0*(Sps/Sac-1.0), m, 100.0*se/m, 100.0*ae/m, 100.0*over/m;
    printf "          FULL  Σpred=%.0f Σact=%.0f  ratio=%.3f (%+.1f%%)  | per-tick signedRE=%+.1f%%\n",
           Spf, Sac, Spf/Sac, 100.0*(Spf/Sac-1.0), 100.0*sef/m;
  } else printf "%-9s | no valid invpred ticks\n", tr;
}
