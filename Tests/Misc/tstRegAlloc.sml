fun tstReg (f, r1, r2, r3, r4) =
let
  val t1 = r1 + r2
  val t2 = r3 + r4
  val t3 = t1 + t2
  val t4 = t1 + t3
  val t5 = t2 + t3
  val t6 = t4 + t5
in
  f t1;
  f t2;
  f t3;
  f t4;
  f t5;
  f t6
end
