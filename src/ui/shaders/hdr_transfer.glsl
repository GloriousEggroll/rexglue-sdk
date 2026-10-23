// sRGB/BT.709 SDR to BT.2020 SMPTE ST 2084 output, absolute nits.
vec3 srgb_to_linear(vec3 v) {
  return mix(pow((v + 0.055) / 1.055, vec3(2.4)), v / 12.92, lessThanEqual(v, vec3(0.04045)));
}
vec3 encode_pq(vec3 nits) {
  vec3 p = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(2610.0 / 16384.0));
  return pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) /
             (1.0 + (2392.0 / 128.0) * p), vec3(2523.0 / 32.0));
}
vec3 convert_to_hdr(vec3 srgb, float paper_white, float peak, float highlights) {
  vec3 linear = srgb_to_linear(clamp(srgb, 0.0, 1.0));
  float y = dot(linear, vec3(0.2126, 0.7152, 0.0722));
  float knee = smoothstep(0.6, 1.0, y);
  float luminance = paper_white * y +
      (peak - paper_white) * knee * knee * highlights;
  linear *= y > 0.000001 ? luminance / y : paper_white;
  vec3 bt2020 = vec3(dot(linear, vec3(0.6274040, 0.3292820, 0.0433136)),
                     dot(linear, vec3(0.0690970, 0.9195400, 0.0113612)),
                     dot(linear, vec3(0.0163916, 0.0880132, 0.8955950)));
  float brightest = max(bt2020.r, max(bt2020.g, bt2020.b));
  if (brightest > peak) bt2020 *= peak / brightest;
  return encode_pq(bt2020);
}
