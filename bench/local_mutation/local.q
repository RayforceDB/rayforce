N:500;
syms:N#`a`b`c`d`e`f`g`h`i`j;
mkP:{([]ts:`long$();sym:`symbol$();qty:`long$())};
runL:{[steps] P:mkP[]; q:N#0j; k:0j; do[steps;P,:([]ts:N#k;sym:syms;qty:q);q[til 10]:10#k+1;k+:1];(count P;sum q)};
bench:{[steps] t:.z.p;r:runL steps;ms:("f"$"j"$(.z.p-t))%1000000;show "BENCH|local|",(string steps),"|",(string ms),"|",(string first r),"|",string last r;};
runL 10;
do[5;bench 1000;bench 2000];
exit 0;
