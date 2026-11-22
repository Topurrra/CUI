#version 450
/* cui_render_vulkan.c's fragment shader: cui_render_gl.c's, branching on the vertex's
 * mode. Keep the two in step. tools/compile_vk_shaders.py compiles it into
 * generated/cui_render_vulkan_spirv.h. */
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_local;
layout(location = 2) flat in vec2 v_half;
layout(location = 3) flat in vec4 v_radii;
layout(location = 4) flat in vec2 v_params;
layout(location = 5) in vec4 v_color;
layout(location = 6) flat in vec4 v_bcolor;
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 frag;
float sd_rrect(vec2 p, vec2 b, vec4 r) {
  float rr = p.x < 0.0 ? (p.y < 0.0 ? r.x : r.w) : (p.y < 0.0 ? r.y : r.z);
  rr = min(rr, min(b.x, b.y));
  vec2 q = abs(p) - b + rr;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rr;
}
float erf_(float x) {
  float s = sign(x); x = abs(x);
  float t = 1.0 / (1.0 + 0.3275911 * x);
  float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);
  return s * y;
}
float bayer8(vec2 p) {
  ivec2 q = ivec2(mod(floor(p), 8.0));
  int a = q.x ^ q.y, y = q.y;
  int m = ((a & 1) << 5) | ((y & 1) << 4) | ((a & 2) << 2) | ((y & 2) << 1) | ((a & 4) >> 1) | ((y & 4) >> 2);
  return (float(m) + 0.5) / 64.0;
}
void main() {
  int mode = int(v_params.y + 0.5);
  vec4 c = v_color;
  if (mode == 4) { frag = vec4(c.rgb * c.a, c.a); return; }
  if (mode == 0) { float a = c.a * texture(u_tex, v_uv).r; frag = vec4(c.rgb * a, a); return; }
  float d = sd_rrect(v_local, v_half, v_radii);
  if (mode == 1) {
    float outer = clamp(0.5 - d, 0.0, 1.0);
    float bw = v_params.x;
    float inner = bw > 0.0 ? clamp(0.5 - (d + bw), 0.0, 1.0) : outer;
    float fa = c.a * inner, ba = v_bcolor.a * (outer - inner);
    frag = vec4(c.rgb * fa + v_bcolor.rgb * ba, fa + ba);
  } else if (mode == 2) {
    float s = max(v_params.x, 0.01);
    float a = c.a * 0.5 * (1.0 - erf_(d / (s * 1.41421356)));
    frag = vec4(c.rgb * a, a);
  } else if (mode == 5) {
    float t = bayer8(v_uv);
    float k = t >= v_params.x && t < c.a ? v_bcolor.a * clamp(0.5 - d, 0.0, 1.0) : 0.0;
    frag = vec4(v_bcolor.rgb * k, k);
  } else {
    float cov = clamp(0.5 - d, 0.0, 1.0);
    vec4 t = texture(u_tex, v_uv);
    float k = c.a * cov;
    frag = vec4(t.rgb * t.a * c.rgb * k, t.a * k);
  }
}
