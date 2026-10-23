test_vmaddfp_1:
  #_ REGISTER_IN v4 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  # 1.0, 1.5, 1.1, 1.9
  vmaddfp v3, v4, v4, v4
  blr
  # The binary32 value 0x3f8ccccd is slightly above 1.1. Fused x*x+x
  # rounds to 0x4013d70b, matching the Xenia Canary instruction fixture.
  #_ REGISTER_OUT v3 [40000000, 40700000, 4013d70b, 40b051eb]
  #_ REGISTER_OUT v4 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  # 2.0, 3.75, 2.31, 5.51
  # 40b051eb is actually 5.50999975, not 5.51?
  # 40b051ec is 5.51

test_vmaddfp_2:
  #_ REGISTER_IN v4 [3f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_IN v5 [3f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_IN v6 [3f800000, 3f800000, 3f800000, 3f800000]
  # 1.0, 1.5, 1.1, 1.9
  vmaddfp v3, v4, v5, v6
  blr
  #_ REGISTER_OUT v3 [40000000, 40000000, 40000000, 40000000]
  #_ REGISTER_OUT v4 [3f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_OUT v5 [3f800000, 3f800000, 3f800000, 3f800000]
  #_ REGISTER_OUT v6 [3f800000, 3f800000, 3f800000, 3f800000]
  
test_vmaddfp_3:
  # v4 = 5, 5, 1, 1
  # v5 = 5, 5, 1, 1
  # v6 = 1, 1, 1, 1
  #_ REGISTER_IN v4 [40a00000, 40a00000, 3f800000, 3f800000]
  #_ REGISTER_IN v5 [40a00000, 40a00000, 3f800000, 3f800000]
  #_ REGISTER_IN v6 [3f800000, 3f800000, 3f800000, 3f800000]
  # 1.0, 1.5, 1.1, 1.9
  vmaddfp v3, v4, v5, v6
  blr
  # v3 = 26.0, 26.0, 2.0, 2.0
  #_ REGISTER_OUT v3 [41d00000, 41d00000, 40000000, 40000000]
  #_ REGISTER_OUT v4 [40a00000, 40a00000, 3f800000, 3f800000]
  #_ REGISTER_OUT v5 [40a00000, 40a00000, 3f800000, 3f800000]
  #_ REGISTER_OUT v6 [3f800000, 3f800000, 3f800000, 3f800000]

test_vmaddfp_fused_rounding:
  # Separate multiply and add rounds this to zero. The fused Xbox operation
  # retains the exact product residual, 2^-46.
  #_ REGISTER_IN v4 [3f800001, 3f800001, 3f800001, 3f800001]
  #_ REGISTER_IN v5 [3f800001, 3f800001, 3f800001, 3f800001]
  #_ REGISTER_IN v6 [bf800002, bf800002, bf800002, bf800002]
  vmaddfp v3, v4, v5, v6
  blr
  #_ REGISTER_OUT v3 [28800000, 28800000, 28800000, 28800000]
  #_ REGISTER_OUT v4 [3f800001, 3f800001, 3f800001, 3f800001]
  #_ REGISTER_OUT v5 [3f800001, 3f800001, 3f800001, 3f800001]
  #_ REGISTER_OUT v6 [bf800002, bf800002, bf800002, bf800002]
