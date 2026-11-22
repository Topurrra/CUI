#version 450
/* cui_render_vulkan.c's vertex shader: cui_render_gl.c's, for Vulkan (clip space y down).
 * tools/compile_vk_shaders.py compiles it into generated/cui_render_vulkan_spirv.h. */
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec2 a_local;
layout(location = 3) in vec2 a_half;
layout(location = 4) in vec4 a_radii;
layout(location = 5) in vec2 a_params;
layout(location = 6) in vec4 a_color;
layout(location = 7) in vec4 a_bcolor;
layout(push_constant) uniform Globals { vec2 u_viewport; };
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_local;
layout(location = 2) flat out vec2 v_half;
layout(location = 3) flat out vec4 v_radii;
layout(location = 4) flat out vec2 v_params;
layout(location = 5) out vec4 v_color;
layout(location = 6) flat out vec4 v_bcolor;
void main() {
  v_uv = a_uv; v_local = a_local; v_half = a_half; v_radii = a_radii;
  v_params = a_params; v_color = a_color; v_bcolor = a_bcolor;
  gl_Position = vec4(a_pos.x / u_viewport.x * 2.0 - 1.0, a_pos.y / u_viewport.y * 2.0 - 1.0, 0.0, 1.0);
}
