test_vrsqrtefp128_normal:
  #_ REGISTER_IN v4 [3f800000, 40800000, 41100000, 41800000]
  vrsqrtefp128 v5, v4
  blr
  #_ REGISTER_OUT v5 [3f7ff400, 3efff400, 3eaaa500, 3e7ff400]

test_vrsqrtefp128_fractional:
  #_ REGISTER_IN v4 [3f000000, 3e800000, 3e000000, 3d800000]
  vrsqrtefp128 v5, v4
  blr
  #_ REGISTER_OUT v5 [3fb4fd00, 3ffff400, 4034fd00, 407ff400]

test_vrsqrtefp128_special:
  # +0, -0, +infinity, -infinity.
  #_ REGISTER_IN v4 [00000000, 80000000, 7f800000, ff800000]
  vrsqrtefp128 v5, v4
  blr
  #_ REGISTER_OUT v5 [7f800000, ff800000, 00000000, 7fc00000]

test_vrsqrtefp128_negative_and_nan:
  # Negative normal, positive quiet NaN, negative signaling NaN, denormal.
  #_ REGISTER_IN v4 [bf800000, 7fc12345, ff812345, 00000001]
  vrsqrtefp128 v5, v4
  blr
  #_ REGISTER_OUT v5 [7fc00000, 7fc12345, ffc12345, 7f800000]
