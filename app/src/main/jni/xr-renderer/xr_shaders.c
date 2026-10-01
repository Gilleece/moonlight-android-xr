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

// The picture grade, the same sums as pictureGradeApply in xr_grade.c, shared
// by the warp and the frame colour sample so the glow and the room's light
// follow the graded picture. u_grade is the offset, the contrast gain, the
// exponent (one over the gamma) and the saturation. Each shader only calls it
// behind u_gradeOn, a branch on a uniform, so a picture left as streamed
// costs nothing. The video's conversion to RGB can land a little under black
// or over white, which the screen shows as black or white, so that is what
// gets graded: lifted, it would come up darker than black does.
#define GRADE_GLSL \
    "uniform float u_gradeOn;\n" \
    "uniform vec4 u_grade;\n" \
    "vec3 grade(vec3 c) {\n" \
    "    c = clamp(c, 0.0, 1.0);\n" \
    "    c = clamp((c - 0.5) * u_grade.y + 0.5 + u_grade.x, 0.0, 1.0);\n" \
    "    c = pow(c, vec3(u_grade.z));\n" \
    "    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n" \
    "    return clamp(mix(vec3(l), c, u_grade.w), 0.0, 1.0);\n" \
    "}\n"

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
    // How far inside the frame the shifted sample is held, in frame uv. Half a
    // texel as shipped; -1 is a whole frame outside, which no shift reaches.
    "uniform float u_srcInset;\n"
    // Width of the band at the left and right edges over which the shift
    // falls to nothing, in frame uv. 0 is no band.
    "uniform float u_edgeFade;\n"
    "uniform float u_depthCubic;\n"
    GRADE_GLSL
    "out vec4 fragColor;\n"
    // The depth here is a quarter of the frame wide, so a texel of it spans
    // four pixels, and a bilinear read joins texels with straight segments.
    // Across a depth step the shift then changes inside a single texel, and a
    // straight silhouette comes out as a staircase with four pixel treads. A
    // cubic B spline is smooth across texel boundaries. It is done as four
    // bilinear fetches placed between texel pairs, the way the glow does it,
    // so it costs three more than the plain read.
    //
    // Every read of the depth in a pixel is on that pixel's row, so the
    // vertical half of the spline is worked out once in main, into depthRow:
    // the two fetch rows in uv and the share of the first.
    "vec2 depthSize;\n"
    "float depthTexelX;\n"
    "vec3 depthRow;\n"
    // The spline along one axis at t texels in: where its two fetches go, in
    // texels, and how much of the first to take. The glow's four weights,
    // multiplied through by six and folded, which is the same numbers for
    // about half the arithmetic.
    "vec3 splineAxis(float t) {\n"
    "    float base = floor(t);\n"
    "    float f = t - base;\n"
    "    float f2 = f * f;\n"
    "    float g0 = 5.0 + f * (-3.0 + f * (-3.0 + 2.0 * f));\n"
    "    float w1 = 4.0 + f2 * (-6.0 + 3.0 * f);\n"
    "    return vec3(base - 0.5 + w1 / g0, base + 1.5 + f2 * f / (6.0 - g0), g0 / 6.0);\n"
    "}\n"
    "float depthAt(float x) {\n"
    "    if (u_depthCubic < 0.5) {\n"
    "        return texture(u_depth, vec2(x, v_plain.y)).a;\n"
    "    }\n"
    "    vec3 sx = splineAxis(x * depthSize.x - 0.5);\n"
    "    vec2 hx = sx.xy * depthTexelX;\n"
    "    float a00 = texture(u_depth, vec2(hx.x, depthRow.x)).a;\n"
    "    float a10 = texture(u_depth, vec2(hx.y, depthRow.x)).a;\n"
    "    float a01 = texture(u_depth, vec2(hx.x, depthRow.y)).a;\n"
    "    float a11 = texture(u_depth, vec2(hx.y, depthRow.y)).a;\n"
    "    return mix(mix(a11, a01, sx.z), mix(a10, a00, sx.z), depthRow.z);\n"
    "}\n"
    "void main() {\n"
    "    if (u_depthCubic > 0.5) {\n"
    "        depthSize = vec2(textureSize(u_depth, 0));\n"
    "        depthTexelX = 1.0 / depthSize.x;\n"
    "        vec3 sy = splineAxis(v_plain.y * depthSize.y - 0.5);\n"
    "        depthRow = vec3(sy.xy / depthSize.y, sy.z);\n"
    "    }\n"
    "    if (u_showDepth > 0.5) {\n"
    "        float dd = depthAt(v_plain.x);\n"
    "        fragColor = vec4(dd, dd, dd, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    vec2 tc = v_plain;\n"
    // A pixel near a side edge whose shift points outward has nothing in the
    // frame to read, and the clamp below hands it the edge column, so that
    // column gets smeared across the width of the shift. Letting the shift die
    // away over the last few pixels means nothing asks to leave the frame, and
    // the picture's edge stays where it is in both eyes. Only across, since
    // the shift is horizontal.
    "    float edgeW = smoothstep(0.0, max(u_edgeFade, 1e-6),\n"
    "                             min(v_plain.x, 1.0 - v_plain.x));\n"
    "    float disp = edgeW * u_disparity;\n"
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
    "        tc.x = v_plain.x + edgeW * off * 2.0 * float(reach) / u_lowResWidth;\n"
    "        float h = 1.0 / u_frameWidth;\n"
    "        for (int i = 0; i < 2; i++) {\n"
    // Where the sample lands comes from d0 alone, so only it takes the cubic
    // read. The pair either side only scales the step.
    "            float d0 = depthAt(tc.x);\n"
    "            float dm = texture(u_depth, vec2(tc.x - h, v_plain.y)).a;\n"
    "            float dp = texture(u_depth, vec2(tc.x + h, v_plain.y)).a;\n"
    "            float e = (tc.x - v_plain.x) + disp * (d0 - u_convergence);\n"
    "            float slope = 1.0 + disp * (dp - dm) / (2.0 * h);\n"
    "            if (abs(slope) < 0.25) {\n"
    "                slope = 0.25;\n"
    "            }\n"
    "            tc.x -= clamp(e / slope, -4.0 * h, 4.0 * h);\n"
    "        }\n"
    "    }\n"
    "    else {\n"
    "        tc.x -= disp * (depthAt(v_plain.x) - u_convergence);\n"
    "    }\n"
    "    if (u_barTest > 0.5) {\n"
    "        float b = 1.0 - step(0.004, abs(tc.x - 0.5));\n"
    "        fragColor = vec4(b, b, b, 1.0);\n"
    "        return;\n"
    "    }\n"
    // Past 0 or 1 the read is left to however the driver treats an external
    // texture off its edge, and the bilinear tap at the edge texel can reach
    // round to the far side of the picture. Held half a texel inside, the
    // sample stays on the picture whatever the shift did.
    "    tc.x = clamp(tc.x, u_srcInset, 1.0 - u_srcInset);\n"
    "    fragColor = texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy);\n"
    // On the colour alone, after the shift, so where things sit in each eye
    // is the same graded or not
    "    if (u_gradeOn > 0.5) {\n"
    "        fragColor.rgb = grade(fragColor.rgb);\n"
    "    }\n"
    "    fragColor.rgb *= u_tint;\n"
    "}\n";

// Joint bilateral upsample of the depth map. The model output is a few
// hundred texels across against a 4K frame, so one depth texel covers a block
// of up to 15x8 pixels and every depth boundary reaches the warp as a ramp that
// wide. That ramp is the halo: it shears whatever colour happens to sit under
// it.
//
// Each output pixel weights the 5x5 low resolution depth neighbourhood by how
// closely each neighbour's colour matches the colour here, so the depth edge
// snaps to the colour edge instead of straddling it. Measured on a captured
// frame from a 256 square map this took the edge from 15 px to 5 px, which is
// the resolution limit of the source rather than of this filter.
//
// u_depthSize is the map's width and height, which is the session's.
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
    "uniform vec2 u_depthSize;\n"
    "out vec4 fragColor;\n"
    "const float SIGMA_S = 1.5;\n"
    "const float FLAT = 0.05;\n"
    "void main() {\n"
    "    vec3 hi = texture(u_texture, (u_texmatrix * vec4(v_plain, 0.0, 1.0)).xy).rgb;\n"
    "    vec2 lp = v_plain * u_depthSize - 0.5;\n"
    "    ivec2 base = ivec2(floor(lp));\n"
    "    float num = 0.0;\n"
    "    float den = 0.0;\n"
    "    float dlo = 1.0;\n"
    "    float dhi = 0.0;\n"
    "    for (int dy = -2; dy <= 2; dy++) {\n"
    "        for (int dx = -2; dx <= 2; dx++) {\n"
    "            ivec2 q = clamp(base + ivec2(dx, dy), ivec2(0), ivec2(u_depthSize) - 1);\n"
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

// Feeds the depth model. The video is far larger than the map, so a single
// bilinear tap per output pixel aliases badly and the depth map crawls with
// it. A 4x4 box over each destination pixel is still nothing on this GPU.
// u_texel is one destination texel on each axis, so the box covers exactly
// that at any map size.
const char* const DOWNSCALE_FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform vec2 u_texel;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 sum = vec3(0.0);\n"
    "    for (int y = 0; y < 4; y++) {\n"
    "        for (int x = 0; x < 4; x++) {\n"
    "            vec2 off = (vec2(float(x), float(y)) - 1.5) * (0.25 * u_texel);\n"
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
// The picture grade goes on each tap rather than on their sum, so the sample
// is the average of what the screen shows. The letterbox detector draws with
// it off, so a lifted black bar is still found as a bar.
const char* const AMBI_FRAGMENT_SRC =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision highp float;\n"
    "in vec2 v_plain;\n"
    "uniform samplerExternalOES u_texture;\n"
    "uniform mat4 u_texmatrix;\n"
    "uniform vec4 u_crop;\n"
    GRADE_GLSL
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
    "            vec3 c = texture(u_texture, (u_texmatrix * vec4(tc, 0.0, 1.0)).xy).rgb;\n"
    "            if (u_gradeOn > 0.5) {\n"
    "                c = grade(c);\n"
    "            }\n"
    "            sum += c;\n"
    "        }\n"
    "    }\n"
    "    fragColor = vec4(sum * (1.0 / 16.0), 1.0);\n"
    "}\n";

// The glow's copy of the sample texture: every texel lifted to a steady luma,
// so the glow takes its hue from the picture and its brightness from the glow
// level, and the ring of edge texels rolled off where the picture goes dark.
// Only the ring and the texels just inside it reach the glow's border. An edge
// texel that is dark first looks a little way into the picture, so a thin dark
// border does not take the colour with it. One still dark then takes the
// colour of the lit texels near it along the ring, weighted by how lit they
// are and falling smoothly with distance, at as much brightness as the nearest
// of them carries that far. A lit run then ramps down into a dark one instead
// of stopping at its boundary, and a long dark run still ends in black. Lit
// texels are left as the normalisation made them.
//
// The same sums as xr_glow.c, with its constants handed in as uniforms.
const char* const GLOW_EDGE_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D u_texture;\n"
    // The luma target, floor and top of the knee, then where lit starts and
    // ends, then the reach along the ring in texels and the inward steps
    "uniform vec3 u_luma;\n"
    "uniform vec2 u_lit;\n"
    "uniform float u_reach;\n"
    "uniform int u_steps;\n"
    "out vec4 fragColor;\n"
    // 32 is AMBI_SAMPLE_TEX, kept in step by hand. The ring runs round it
    // anticlockwise from the bottom left corner, 31 texels a side.
    "const int N = 32;\n"
    "const int RING = 4 * (N - 1);\n"
    "vec3 lifted(ivec2 p) {\n"
    "    vec3 c = texelFetch(u_texture, p, 0).rgb;\n"
    "    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "    float s = smoothstep(u_luma.y, u_luma.z, l);\n"
    "    return clamp(c * (mix(l, u_luma.x, s) / max(l, 1e-5)), 0.0, 1.0);\n"
    "}\n"
    "float lit(vec3 c) {\n"
    "    return smoothstep(u_lit.x, u_lit.y, max(c.r, max(c.g, c.b)));\n"
    "}\n"
    "ivec2 ringTexel(int i) {\n"
    "    if (i < N - 1) return ivec2(i, 0);\n"
    "    if (i < 2 * (N - 1)) return ivec2(N - 1, i - (N - 1));\n"
    "    if (i < 3 * (N - 1)) return ivec2(3 * (N - 1) - i, N - 1);\n"
    "    return ivec2(0, RING - i);\n"
    "}\n"
    "int ringIndex(ivec2 p) {\n"
    "    if (p.y == 0) return p.x;\n"
    "    if (p.x == N - 1) return (N - 1) + p.y;\n"
    "    if (p.y == N - 1) return 3 * (N - 1) - p.x;\n"
    "    return RING - p.y;\n"
    "}\n"
    // The texel's colour, or where it is dark the first lit one further in.
    // Whole texels, since a fetch between a dark one and a lit one reads as
    // lit at half the brightness. Corners look in along the diagonal.
    "vec3 edgeColour(int i) {\n"
    "    ivec2 p = ringTexel(i);\n"
    "    ivec2 inward = ivec2(p.x == 0 ? 1 : (p.x == N - 1 ? -1 : 0),\n"
    "                         p.y == 0 ? 1 : (p.y == N - 1 ? -1 : 0));\n"
    "    vec3 c = lifted(p);\n"
    "    for (int s = 1; s <= u_steps; s++) {\n"
    "        vec3 deeper = lifted(p + inward * s);\n"
    "        c = mix(deeper, c, lit(c));\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    if (p.x > 0 && p.y > 0 && p.x < N - 1 && p.y < N - 1) {\n"
    "        fragColor = vec4(lifted(p), 1.0);\n"
    "        return;\n"
    "    }\n"
    "    int j = ringIndex(p);\n"
    "    int r = min(int(u_reach), RING / 2 - 1);\n"
    "    vec3 own = vec3(0.0);\n"
    "    vec3 sum = vec3(0.0);\n"
    "    float weight = 0.0;\n"
    "    float carried = 0.0;\n"
    "    for (int k = -r; k <= r; k++) {\n"
    "        vec3 c = edgeColour((j + k + RING) % RING);\n"
    "        float t = float(k) / u_reach;\n"
    "        float w = (1.0 - t * t) * lit(c);\n"
    "        sum += c * w;\n"
    "        weight += w;\n"
    "        carried = max(carried, w);\n"
    "        if (k == 0) {\n"
    "            own = c;\n"
    "        }\n"
    "    }\n"
    "    vec3 nearby = sum / max(weight, 1e-4);\n"
    "    vec3 c = own + nearby * (carried * (1.0 - lit(own)));\n"
    "    fragColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
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
    // 1 takes the colour through a spline widened by three texels, 0 through
    // the spline alone
    "uniform float u_blur;\n"
    "out vec4 fragColor;\n"
    // 1.7 is GLOW_SCALE and 32.0 is AMBI_SAMPLE_TEX, both kept in step by hand
    "const float scale = 1.7;\n"
    "const float size = 32.0;\n"
    // A cubic B spline over the colour texture, done as four bilinear fetches.
    // Plain bilinear puts a crease at every texel boundary, and magnified this
    // far those creases are the lines that showed across the glow. This kernel
    // approximates rather than interpolates, so it smooths the texel to texel
    // steps on the way as well.
    "vec3 spline(vec2 fuv) {\n"
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
    "    return mix(mix(c11, c01, g0.x), mix(c10, c00, g0.x), g0.y);\n"
    "}\n"
    // The average of nine spline evaluations one sample texel apart, so the
    // picture's own detail stops reaching the rim and only its colour does.
    // The spline averaged over three texels is one kernel six taps wide, and
    // those pair up into three bilinear fetches an axis the way the spline's
    // own four pair into two: nine fetches for what would otherwise be 36,
    // the same numbers to rounding, clamped edges included.
    "vec3 blurredSpline(vec2 fuv) {\n"
    "    vec2 tc = fuv * size - 0.5;\n"
    "    vec2 base = floor(tc);\n"
    "    vec2 f = tc - base;\n"
    "    vec2 f2 = f * f;\n"
    "    vec2 f3 = f2 * f;\n"
    // The spline's weights on base - 1 to base + 2, then the widened
    // kernel's on base - 2 to base + 3
    "    vec2 b0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;\n"
    "    vec2 b1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;\n"
    "    vec2 b2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;\n"
    "    vec2 b3 = f3 / 6.0;\n"
    "    vec2 wb = (b0 + b1) / 3.0;\n"
    "    vec2 wd = (b1 + b2 + b3) / 3.0;\n"
    "    vec2 wf = b3 / 3.0;\n"
    "    vec2 g0 = (2.0 * b0 + b1) / 3.0;\n"
    "    vec2 g1 = (b0 + 2.0 * b1 + 2.0 * b2 + b3) / 3.0;\n"
    "    vec2 g2 = (b2 + 2.0 * b3) / 3.0;\n"
    "    vec2 h0 = (base - 1.5 + wb / g0) / size;\n"
    "    vec2 h1 = (base + 0.5 + wd / g1) / size;\n"
    "    vec2 h2 = (base + 2.5 + wf / g2) / size;\n"
    "    vec3 r0 = texture(u_texture, vec2(h0.x, h0.y)).rgb * g0.x\n"
    "            + texture(u_texture, vec2(h1.x, h0.y)).rgb * g1.x\n"
    "            + texture(u_texture, vec2(h2.x, h0.y)).rgb * g2.x;\n"
    "    vec3 r1 = texture(u_texture, vec2(h0.x, h1.y)).rgb * g0.x\n"
    "            + texture(u_texture, vec2(h1.x, h1.y)).rgb * g1.x\n"
    "            + texture(u_texture, vec2(h2.x, h1.y)).rgb * g2.x;\n"
    "    vec3 r2 = texture(u_texture, vec2(h0.x, h2.y)).rgb * g0.x\n"
    "            + texture(u_texture, vec2(h1.x, h2.y)).rgb * g1.x\n"
    "            + texture(u_texture, vec2(h2.x, h2.y)).rgb * g2.x;\n"
    "    return r0 * g0.y + r1 * g1.y + r2 * g2.y;\n"
    "}\n"
    "void main() {\n"
    "    vec2 uv = v_plain;\n"
    "    vec2 fuv = (uv - 0.5) * scale + 0.5;\n"
    "    vec3 color;\n"
    "    if (u_blur > 0.5) {\n"
    "        color = blurredSpline(fuv);\n"
    "    }\n"
    "    else {\n"
    "        color = spline(fuv);\n"
    "    }\n"
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

// The 3d room. The light the picture throws is worked out per vertex: the only
// thing that changes frame to frame is how much of the screen's light lands on
// each of them. All the fragment side does is pick between the part's vertex
// colour and the atlas it is textured with, which is what keeps a full screen
// projection layer affordable.
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
    // A part painted from its vertex colours is at mix 0 and a textured one at
    // 1. The sample happens either way, so a white 1x1 stands in for a part
    // with no atlas. The brightness goes on either side of the mix, so it
    // turns the whole room down and not only its atlases.
    "    vec3 base = mix(v_color, texture(u_room, v_uv).rgb, u_texMix) * u_dim;\n"
    "    fragColor = vec4(base + v_wash, 1.0);\n"
    "}\n";
