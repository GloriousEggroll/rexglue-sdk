#version 450
// Native HDR10 output. Input is the completed SDR scene after scaling and UI.
// Color encoding follows sRGB, BT.2020 D65 primaries, and SMPTE ST 2084.
layout(set = 0, binding = 0) uniform texture2D scene;
layout(set = 0, binding = 1) uniform sampler scene_sampler;
layout(push_constant) uniform Constants {
  layout(offset = 16) float paper_white;
  float peak;
  float highlights;
  float padding;
} settings;
layout(location = 0) out vec4 color;

#extension GL_GOOGLE_include_directive : require
#include "hdr_transfer.glsl"
void main() {
  vec3 srgb = texelFetch(sampler2D(scene, scene_sampler), ivec2(gl_FragCoord.xy), 0).rgb;
  color = vec4(convert_to_hdr(srgb, settings.paper_white, settings.peak, settings.highlights), 1.0);
}
