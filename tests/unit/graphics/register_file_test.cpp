#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/register_file.h>

using namespace rex::graphics;

TEST_CASE("GPU register file uses Xenos hardware reset values", "[graphics][registers]") {
  RegisterFile registers;

  CHECK(registers[XE_GPU_REG_VGT_MAX_VTX_INDX] == 0x0000FFFF);
  CHECK(registers[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] == 0x0000FFFF);
  CHECK(registers[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] == 0x20002000);
  CHECK(registers[XE_GPU_REG_RB_STENCILREFMASK_BF] == 0x00FFFF00);
  CHECK(registers[XE_GPU_REG_PA_SU_POINT_SIZE] == 0x00080008);
  CHECK(registers[XE_GPU_REG_PA_SU_POINT_MINMAX] == 0x04000010);
  CHECK(registers[XE_GPU_REG_PA_SU_LINE_CNTL] == 0x00000008);
  CHECK(registers[XE_GPU_REG_PA_SC_LINE_CNTL] == 0x00000400);
  CHECK(registers[XE_GPU_REG_VGT_HOS_REUSE_DEPTH] == 0x0000000E);
  CHECK(registers[XE_GPU_REG_VGT_VERTEX_REUSE_BLOCK_CNTL] == 0x0000000E);
  CHECK(registers[XE_GPU_REG_VGT_OUT_DEALLOC_CNTL] == 0x00000010);
  CHECK(registers[XE_GPU_REG_PA_CL_GB_VERT_CLIP_ADJ] == 0x40000000);
  CHECK(registers[XE_GPU_REG_PA_CL_GB_VERT_DISC_ADJ] == 0x3F800000);
  CHECK(registers[XE_GPU_REG_PA_CL_GB_HORZ_CLIP_ADJ] == 0x40000000);
  CHECK(registers[XE_GPU_REG_PA_CL_GB_HORZ_DISC_ADJ] == 0x3F800000);
  CHECK(registers[XE_GPU_REG_PA_SC_AA_MASK] == 0x0000FFFF);

  CHECK(registers[XE_GPU_REG_RB_COLORCONTROL] == 0);
}
