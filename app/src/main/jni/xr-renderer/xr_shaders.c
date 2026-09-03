#include "xr_shaders.h"

const char* const VERTEX_SRC =
    "#version 300 es\n"
    "in vec4 a_position;\n"
    "in vec4 a_texcoord;\n"
    "out vec2 v_plain;\n"
    "void main() {\n"
    "    gl_Position = a_position;\n"
    "    v_plain = a_texcoord.xy;\n"
    "}\n";

// Gather warp. Each output pixel samples the color frame shifted by a
// disparity derived from the depth map. u_disparity is signed per eye and
// zero in mono, which makes this exactly the old passthrough. The transform
// matrix is applied after the shift since the shift is defined in frame
// space, not in the video driver's transformed space.
const char* const FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform sampler2D u_depth;\n"
    "uniform sampler2D u_offsets;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform float u_disparity;\n"
    "uniform float u_showDepth;\n"
    "uniform float u_barTest;\n"
    "uniform vec3 u_tint;\n"
    "uniform float u_occlusion;\n"
    "uniform float u_eyeIndex;\n"
    "uniform float u_convergence;\n"
    "uniform float u_dispTexels;\n"
    "uniform float u_lowResWidth;\n"
    "uniform float u_frameWidth;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    float d = texture(u_depth, v_plain).a;\n"
    "    if (u_showDepth > 0.5) {\n"
    "        fragColor = vec4(d, d, d, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    vec2 tc = v_plain;\n"
    "    if (u_occlusion > 0.5) {\n"
    // The offset map already picked the right surface. All that is left is
    // the exact position on it, which the low resolution search only knew to
    // within a texel, and that quantization stair steps along a diagonal
    // silhouette. Two Newton steps against the full resolution depth settle
    // it to well under a pixel.
    "        int reach = int(ceil(abs(u_dispTexels)\n"
    "                        * max(u_convergence, 1.0 - u_convergence))) + 2;\n"
    "        vec2 enc = texture(u_offsets, v_plain).rg;\n"
    "        float off = (u_eyeIndex < 0.5 ? enc.r : enc.g) - 0.5;\n"
    "        tc.x = v_plain.x + off * 2.0 * float(reach) / u_lowResWidth;\n"
    "        float h = 1.0 / u_frameWidth;\n"
    "        for (int i = 0; i < 2; i++) {\n"
    "            float d0 = texture(u_depth, vec2(tc.x, v_plain.y)).a;\n"
    "            float dm = texture(u_depth, vec2(tc.x - h, v_plain.y)).a;\n"
    "            float dp = texture(u_depth, vec2(tc.x + h, v_plain.y)).a;\n"
    "            float e = (tc.x - v_plain.x) + u_disparity * (d0 - u_convergence);\n"
    "            float slope = 1.0 + u_disparity * (dp - dm) / (2.0 * h);\n"
    "            if (abs(slope) < 0.25) {\n"
    "                slope = 0.25;\n"
    "            }\n"
    "            tc.x -= clamp(e / slope, -4.0 * h, 4.0 * h);\n"
    "        }\n"
    "    }\n"
    "    else {\n"
    "        tc.x -= u_disparity * (d - u_convergence);\n"
    "    }\n"
    "    if (u_barTest > 0.5) {\n"
    "        float b = 1.0 - step(0.004, abs(tc.x - 0.5));\n"
    "        fragColor = vec4(b, b, b, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    fragColor = texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy);\n"
    "    fragColor.rgb *= u_tint;\n"
    "}\n";

// Joint bilateral upsample of the depth map. The model output is 256x256
// against a 4K frame, so one depth texel covers a 15x8 block and every depth
// boundary reaches the warp as a 15 pixel ramp. That ramp is the halo: it
// shears whatever colour happens to sit under it.
//
// Each output pixel weights the 5x5 low resolution depth neighbourhood by how
// closely each neighbour's colour matches the colour here, so the depth edge
// snaps to the colour edge instead of straddling it. Measured on a captured
// frame this takes the edge from 15 px to 5 px, which is the resolution limit
// of a 256x256 source rather than of this filter.
//
// The guide rides in the rgb of the depth texture, so it is by construction
// the same frame the depth was inferred from. u_sigmaR trades edge snapping
// against depth detail invented out of colour texture: grass and carpet will
// speckle if it is set too tight.
const char* const UPSAMPLE_FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform sampler2D u_depth;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform float u_sigmaR;\n"
    "uniform float u_sharp;\n"
    "out vec4 fragColor;\n"
    "const float N = 256.0;\n"
    "const float SIGMA_S = 1.5;\n"
    "const float FLAT = 0.05;\n"
    "void main() {\n"
    "    vec3 hi = texture(u_texture, (u_texmatrix * vec4(v_plain, 0.0, 1.0)).xy).rgb;\n"
    "    vec2 lp = v_plain * N - 0.5;\n"
    "    ivec2 base = ivec2(floor(lp));\n"
    "    float num = 0.0;\n"
    "    float den = 0.0;\n"
    "    float dlo = 1.0;\n"
    "    float dhi = 0.0;\n"
    "    for (int dy = -2; dy <= 2; dy++) {\n"
    "        for (int dx = -2; dx <= 2; dx++) {\n"
    "            ivec2 q = clamp(base + ivec2(dx, dy), ivec2(0), ivec2(int(N) - 1));\n"
    "            vec4 s = texelFetch(u_depth, q, 0);\n"
    "            vec2 off = vec2(q) - lp;\n"
    "            float ws = exp(-dot(off, off) / (2.0 * SIGMA_S * SIGMA_S));\n"
    "            vec3 cd = hi - s.rgb;\n"
    "            float wr = exp(-dot(cd, cd) / (2.0 * u_sigmaR * u_sigmaR));\n"
    "            float w = ws * wr;\n"
    "            num += w * s.a;\n"
    "            den += w;\n"
    "            dlo = min(dlo, s.a);\n"
    "            dhi = max(dhi, s.a);\n"
    "        }\n"
    "    }\n"
    "    float d = num / max(den, 1e-6);\n"
    // A soft depth ramp across a silhouette spreads the disocclusion over the
    // width of the ramp, and that band is the smear. Pushing each texel to
    // whichever side of the local range it is nearer turns the ramp back into
    // a step, using the min and max of taps already read. Flat neighbourhoods
    // are left alone, so only boundaries move.
    "    float span = dhi - dlo;\n"
    "    if (u_sharp > 0.0 && span >= FLAT) {\n"
    "        float u = clamp((d - dlo) / max(span, 1e-6), 0.0, 1.0);\n"
    "        float snapped = dlo + span / (1.0 + exp(-24.0 * (u - 0.5)));\n"
    "        d = mix(d, snapped, u_sharp);\n"
    "    }\n"
    "    fragColor = vec4(d);\n"
    "}\n";

// Inverts the warp properly, once per frame for both eyes, at the same
// quarter resolution as the depth map.
//
// A source pixel at offset t from this one lands here with error
//     e(t) = t + disp * (d(here + t) - convergence)
// so every zero crossing of e is a source that genuinely lands on this pixel.
// Sampling depth at the destination, which is what the warp did before, is
// only right where depth is flat; at a depth step it is wrong by most of the
// disparity range, which is 57 px at 4K, and that is the smearing. More than
// one crossing means two surfaces compete for this pixel, and the nearest one
// wins, which is what occlusion means.
//
// The whole search span is only about nine texels at this resolution, so the
// exhaustive version is affordable. Both eyes share the depth reads.
const char* const OFFSET_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform sampler2D u_depth;\n"
    "uniform float u_dispTexels;\n"
    "uniform float u_convergence;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    ivec2 sz = textureSize(u_depth, 0);\n"
    "    int x = int(gl_FragCoord.x);\n"
    "    int y = int(gl_FragCoord.y);\n"
    "    int reach = int(ceil(abs(u_dispTexels)\n"
    "                    * max(u_convergence, 1.0 - u_convergence))) + 2;\n"
    "    vec2 result = vec2(0.0);\n"
    "    for (int eye = 0; eye < 2; eye++) {\n"
    "        float disp = (eye == 0) ? u_dispTexels : -u_dispTexels;\n"
    "        float here = texelFetch(u_depth, ivec2(x, y), 0).a;\n"
    "        float bestD = -1.0;\n"
    "        float bestOff = -disp * (here - u_convergence);\n"
    "        float pd = texelFetch(u_depth,\n"
    "                ivec2(clamp(x - reach, 0, sz.x - 1), y), 0).a;\n"
    "        float pe = float(-reach) + disp * (pd - u_convergence);\n"
    "        for (int t = -reach + 1; t <= reach; t++) {\n"
    "            float cd = texelFetch(u_depth,\n"
    "                    ivec2(clamp(x + t, 0, sz.x - 1), y), 0).a;\n"
    "            float ce = float(t) + disp * (cd - u_convergence);\n"
    "            float span = ce - pe;\n"
    "            if (pe * ce <= 0.0 && abs(span) > 1e-6) {\n"
    "                float f = clamp(-pe / span, 0.0, 1.0);\n"
    "                float rd = pd + f * (cd - pd);\n"
    "                if (rd > bestD) {\n"
    "                    bestD = rd;\n"
    "                    bestOff = float(t - 1) + f;\n"
    "                }\n"
    "            }\n"
    "            pd = cd;\n"
    "            pe = ce;\n"
    "        }\n"
    "        result[eye] = bestOff;\n"
    "    }\n"
    "    fragColor = vec4(result / (2.0 * float(reach)) + 0.5, 0.0, 1.0);\n"
    "}\n";

// Feeds the depth model. The video is far larger than 256x256, so a single
// bilinear tap per output pixel aliases badly and the depth map crawls with
// it. A 4x4 box over each destination pixel is still nothing on this GPU.
const char* const DOWNSCALE_FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform mat4 u_texmatrix;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 sum = vec3(0.0);\n"
    "    for (int y = 0; y < 4; y++) {\n"
    "        for (int x = 0; x < 4; x++) {\n"
    "            vec2 off = (vec2(float(x), float(y)) - 1.5) * (0.25 / 256.0);\n"
    "            vec2 tc = v_plain + off;\n"
    "            sum += texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy).rgb;\n"
    "        }\n"
    "    }\n"
    "    fragColor = vec4(sum * (1.0 / 16.0), 1.0);\n"
    "}\n";

// Feeds the ambilight. Thirty two square is coarse enough to read as a wash
// rather than as a blurred copy of the picture, and the same 4x4 box the depth
// downscale uses is what keeps it steady: one tap per output texel and the
// colours crawl as the sample points cross detail in the frame.
// u_crop is the region of the frame worth sampling, x0 y0 w h, and letterbox
// detection narrows it to the picture inside the black bars. At 0 0 1 1 the
// output is what it was before there was a crop at all.
const char* const AMBI_FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform vec4 u_crop;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec2 base = u_crop.xy + v_plain * u_crop.zw;\n"
    "    vec3 sum = vec3(0.0);\n"
    "    for (int y = 0; y < 4; y++) {\n"
    "        for (int x = 0; x < 4; x++) {\n"
    // Shrunk with the crop, so the box stays inside the content it belongs to
    // instead of reaching back over the bar
    "            vec2 off = (vec2(float(x), float(y)) - 1.5) * (0.25 / 32.0) * u_crop.zw;\n"
    "            vec2 tc = base + off;\n"
    "            sum += texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy).rgb;\n"
    "        }\n"
    "    }\n"
    "    fragColor = vec4(sum * (1.0 / 16.0), 1.0);\n"
    "}\n";

// The glow itself. The quad is larger than the screen, so the middle of it
// covers the picture and only the border is ever seen. Sampling the colour
// texture over that middle and letting the clamp carry the edge texels outward
// is what spreads the frame's colours into the space around it.
const char* const GLOW_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_intensity;\n"
    "out vec4 fragColor;\n"
    // 1.7 is GLOW_SCALE and 32.0 is AMBI_SAMPLE_TEX, both kept in step by hand
    "const float scale = 1.7;\n"
    "const float size = 32.0;\n"
    "void main() {\n"
    "    vec2 uv = v_plain;\n"
    "    vec2 fuv = (uv - 0.5) * scale + 0.5;\n"
    // A cubic B spline over the colour texture, done as four bilinear fetches.
    // Plain bilinear puts a crease at every texel boundary, and magnified this
    // far those creases are the lines that showed across the glow. This kernel
    // approximates rather than interpolates, so it smooths the texel to texel
    // steps on the way as well.
    "    vec2 tc = fuv * size - 0.5;\n"
    "    vec2 base = floor(tc);\n"
    "    vec2 f = tc - base;\n"
    "    vec2 f2 = f * f;\n"
    "    vec2 f3 = f2 * f;\n"
    "    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;\n"
    "    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;\n"
    "    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;\n"
    "    vec2 w3 = f3 / 6.0;\n"
    // Each pair of taps folds into one bilinear fetch placed between them, so
    // sixteen texel reads come out of four
    "    vec2 g0 = w0 + w1;\n"
    "    vec2 g1 = w2 + w3;\n"
    "    vec2 h0 = (base - 0.5 + w1 / g0) / size;\n"
    "    vec2 h1 = (base + 1.5 + w3 / g1) / size;\n"
    "    vec3 c00 = texture(u_texture, vec2(h0.x, h0.y)).rgb;\n"
    "    vec3 c10 = texture(u_texture, vec2(h1.x, h0.y)).rgb;\n"
    "    vec3 c01 = texture(u_texture, vec2(h0.x, h1.y)).rgb;\n"
    "    vec3 c11 = texture(u_texture, vec2(h1.x, h1.y)).rgb;\n"
    "    vec3 color = mix(mix(c11, c01, g0.x), mix(c10, c00, g0.x), g0.y);\n"
    // Distance out into the border, 0 at the screen edge and 1 at the rim
    "    vec2 d = max(abs(uv - 0.5) - 0.5 / scale, 0.0) / (0.5 - 0.5 / scale);\n"
    "    float t = min(length(d), 1.0);\n"
    // Flat at both ends, so neither the start of the fade nor the rim draws a
    // line of its own. Squared to keep about the strength the plain curve had.
    "    float s = t * t * (3.0 - 2.0 * t);\n"
    "    float fall = (1.0 - s) * (1.0 - s);\n"
    "    float a = fall * u_intensity;\n"
    // Premultiplied, which is what the runtime composites the panel art as
    "    fragColor = vec4(color * a, a);\n"
    "}\n";

// The 3d room. Both the colouring of the generated room and the light the
// picture throws are worked out per vertex: the geometry is a few hundred
// vertices, and the only thing that changes frame to frame is how much of the
// screen's light lands on each of them. All the fragment side does is pick
// between that vertex colour and the atlas a baked room is textured with, which
// is what keeps a full screen projection layer affordable.
const char* const ROOM_VERTEX_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec3 a_position;\n"
    "in vec3 a_color;\n"
    "in float a_spill;\n"
    "in vec2 a_uv;\n"
    "uniform mat4 u_viewproj;\n"
    "uniform sampler2D u_ambi;\n"
    "uniform float u_spillGain;\n"
    "out vec3 v_color;\n"
    "out vec3 v_wash;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    // Five taps over the frame's colour texture, centre and the middle of each
    // edge. A wash of light on a wall carries no more detail than that.
    "    vec3 lit = texture(u_ambi, vec2(0.5, 0.5)).rgb;\n"
    "    lit += texture(u_ambi, vec2(0.15, 0.5)).rgb;\n"
    "    lit += texture(u_ambi, vec2(0.85, 0.5)).rgb;\n"
    "    lit += texture(u_ambi, vec2(0.5, 0.15)).rgb;\n"
    "    lit += texture(u_ambi, vec2(0.5, 0.85)).rgb;\n"
    "    vec3 wash = lit * 0.2;\n"
    "    v_color = a_color;\n"
    "    v_wash = wash * (a_spill * u_spillGain);\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = u_viewproj * vec4(a_position, 1.0);\n"
    "}\n";

const char* const ROOM_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 v_color;\n"
    "in vec3 v_wash;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_room;\n"
    "uniform float u_texMix;\n"
    "uniform float u_dim;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    // A generated room is at mix 0 and a textured one at 1. The sample happens
    // either way, so a white 1x1 stands in while nothing else is loaded.
    "    vec3 base = mix(v_color, texture(u_room, v_uv).rgb * u_dim, u_texMix);\n"
    "    fragColor = vec4(base + v_wash, 1.0);\n"
    "}\n";
