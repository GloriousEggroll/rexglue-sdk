test_vnmsubfp_signed_zero:
  #_ REGISTER_IN v4 [3f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_IN v5 [3f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_IN v6 [3f800000, 3f800000, 3f800000, 3f800000]
  vnmsubfp v3, v4, v5, v6
  blr
  #_ REGISTER_OUT v3 [80000000, 80000000, 80000000, 80000000]

test_vnmsubfp_fused_rounding:
  # Separate multiply and subtract rounds this to zero. The fused Xbox
  # operation retains and then negates the exact product residual, 2^-46.
  #_ REGISTER_IN v4 [3f800001, 3f800001, 3f800001, 3f800001]
  #_ REGISTER_IN v5 [3f800001, 3f800001, 3f800001, 3f800001]
  #_ REGISTER_IN v6 [3f800002, 3f800002, 3f800002, 3f800002]
  vnmsubfp v3, v4, v5, v6
  blr
  #_ REGISTER_OUT v3 [a8800000, a8800000, a8800000, a8800000]
