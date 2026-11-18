#include "Crystal.hpp"

#include "Config.hpp"

#include <algorithm>
#include <cmath>

#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

namespace Hyprdrive::Crystal {

    namespace {
        constexpr float SHIFT = 18.F; // logical px a shard moves what is behind it, at most
        constexpr int   BLUR_SIZE = 8; // the frost under the shards, so text over them reads
        constexpr float RIM = 44.F, THICK = 56.F, BEND = 22.F; // logical px: True Glass's glass-card rim, slab, cap

        SP<CShader>              g_shader;
        bool                     g_shaderFailed = false;
        GLint                    g_locMode = -1, g_locRes = -1, g_locBox = -1, g_locRound = -1, g_locScale = -1, g_locShift = -1, g_locCell = -1, g_locAlpha = -1, g_locMilk = -1, g_locVary = -1, g_locEdge = -1, g_locRimPx = -1, g_locCapPx = -1, g_locThickPx = -1, g_locRimMax = -1, g_locDepth = -1, g_locForm = -1, g_locBevel = -1, g_locTone = -1, g_locLightBase = -1;
        SP<Render::IFramebuffer> g_copy;
        // True Glass's frost: the copy at half size, blurred ping-pong between these two
        SP<Render::IFramebuffer> g_half[2];
        GLint                    g_locFrostTex = -1, g_locFrostMix = -1, g_locDir = -1, g_locBlurR = -1;
        // the frost the next mode-1 draw mixes in (unset: none)
        SP<Render::ITexture>     g_frostTex;
        float                    g_frostMix = 0.F;
        Vector2D                 g_blurDir;
        float                    g_blurR = 0.F;
        constexpr float          FROST_BLUR = 96.F; // logical px, glass.lua's frost.blur

        constexpr const char* VERT = R"#(#version 300 es
uniform mat3 proj;
in vec2 pos;
in vec2 texcoord;
out vec2 v_texcoord;
void main() {
    vec3 p = proj * vec3(pos, 1.0);
    gl_Position = vec4(p.xy, 0.0, p.z);
    v_texcoord = texcoord;
}
)#";

        // mode 0: a straight copy. mode 1: the plate, its shards sampled from that
        // copy. Render px throughout; shards are laid out in logical px (uScale).
        constexpr const char* FRAG = R"#(#version 300 es
precision highp float;
in vec2 v_texcoord;
uniform sampler2D tex;
uniform int   uMode;
uniform vec2  uRes;
uniform vec4  uBox;
uniform float uRound;
uniform float uScale;
uniform float uShift;
uniform float uCell;
uniform float uAlpha;
uniform float uMilk;
uniform float uVary;
uniform float uEdge;
uniform float uRimPx;
uniform float uCapPx;
uniform float uThickPx;
uniform float uRimMax;
uniform float uDepth;
uniform float uForm;
uniform float uBevel;
uniform int   uTone;
uniform float uLightBase;
uniform sampler2D frostTex;
uniform float uFrostMix;
uniform vec2  uDir;
uniform float uBlurR;
layout(location = 0) out vec4 fragColor;
vec2 cellPoint(vec2 c) { return fract(sin(vec2(dot(c, vec2(127.1, 311.7)), dot(c, vec2(269.5, 183.3)))) * 43758.5453); }
vec4 crystalFacet(vec2 p) {
    float cell = max(uCell, 8.0);
    vec2 q = p / cell, i = floor(q), f = fract(q), best = vec2(0.0), bestC = vec2(0.0);
    float d1 = 8.0;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        vec2 g = vec2(x, y), r = g + cellPoint(i + g) - f;
        float d = dot(r, r);
        if (d < d1) { d1 = d; best = r; bestC = i + g; }
    }
    float edge = 8.0;
    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {
        vec2 g = vec2(x, y), r = g + cellPoint(i + g) - f, e = r - best;
        if (dot(e, e) > 1e-5) edge = min(edge, dot(0.5 * (best + r), normalize(e)));
    }
    vec3 h = fract(sin(vec3(dot(bestC, vec2(41.3, 289.1)), dot(bestC, vec2(173.9, 61.7)), dot(bestC, vec2(97.2, 157.4)))) * 24634.6345);
    return vec4(h.xy * 2.0 - 1.0, h.z, edge * cell);
}
// True Glass's glass-card material: n_d 1.52 and Abbe 6 as Cauchy indices at 611,
// 549 and 464 nm, Schlick's F0, and its lights (GlassRenderer.cpp)
const vec3  N_RGB       = vec3(1.51013, 1.53912, 1.59930);
const float F0          = 0.04258;
const vec2  SPEC_LIGHT  = vec2(-0.70711, -0.70711);
const vec2  BEVEL_LIGHT = vec2(-0.70711, -0.70711);
float rimHeight(float t) { float r = 1.0 - t; return pow(max(1.0 - r * r * r, 0.0), 1.0 / 3.0); }
float rimSlope(float t) { float r = 1.0 - t; float g = max(1.0 - r * r * r, 1e-4); return r * r * pow(g, -2.0 / 3.0); }
vec2 refractOffsetPx(vec3 N, float n, float z, float capPx) {
    vec3 T = refract(vec3(0.0, 0.0, -1.0), N, 1.0 / n);
    if (T.z > -1e-3) return vec2(0.0);
    vec2 o = T.xy * (z / -T.z);
    float l = length(o);
    return l > 1e-4 ? o * (capPx * tanh(l / max(capPx, 1e-3)) / l) : o;
}
// glassTone: light lifts all of it into the light range, the darker the more; dark smokes it
vec3 tone(vec3 c) {
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    if (uTone == 1) return mix(c, vec3(1.0), min(uLightBase + 0.4 * (1.0 - smoothstep(0.3, 0.85, l)), 0.85));
    if (uTone == 2) return c * (1.0 - 0.58 * (0.35 + 0.65 * smoothstep(0.1, 0.85, l)));
    return c;
}
// The engine's frosted read (sampleBlurred, milk 0): its frost over the sharp picture,
// the colour kept vivid as a blur would grey it, and a little deeper
vec3 frosted(vec2 uv) {
    vec3 c = texture(tex, uv).rgb;
    if (uFrostMix > 0.001) {
        c = mix(c, texture(frostTex, uv).rgb, uFrostMix);
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = clamp(mix(vec3(l), c, 1.0 + 0.35 * uFrostMix), 0.0, 1.0) * (1.0 - 0.10 * uFrostMix);
    }
    return c;
}
void main() {
    if (uMode == 0) { fragColor = texture(tex, v_texcoord); return; }
    // mode 3: the engine's gaussianblur.frag, one direction: at most 8 taps, paired into
    // bilinear fetches, sigma a third of the radius
    if (uMode == 3) {
        float sigma = max(uBlurR / 3.0, 0.001), inv2 = -0.5 / (sigma * sigma);
        int   n     = min(int(ceil(uBlurR)), 8);
        vec4  acc   = clamp(texture(tex, v_texcoord), 0.0, 1.0);
        float wsum  = 1.0;
        for (int i = 1; i <= n; i += 2) {
            float x1 = float(i), x2 = float(i + 1);
            float w1 = exp(x1 * x1 * inv2), w2 = (i + 1 <= n) ? exp(x2 * x2 * inv2) : 0.0, w = w1 + w2;
            if (w < 0.0001) continue;
            float o = (x1 * w1 + x2 * w2) / w;
            acc += (clamp(texture(tex, v_texcoord + uDir * o), 0.0, 1.0) + clamp(texture(tex, v_texcoord - uDir * o), 0.0, 1.0)) * w;
            wsum += 2.0 * w;
        }
        fragColor = acc / wsum;
        return;
    }
    vec2  local = v_texcoord * uRes - uBox.xy;
    vec2  half_ = 0.5 * uBox.zw;
    vec2  d     = abs(local - half_) - (half_ - vec2(uRound));
    float sdf   = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - uRound;
    float mask  = clamp(0.5 - sdf, 0.0, 1.0);
    if (mask <= 0.0) discard;
    vec2  sg = sign(local - half_), qp = max(d, 0.0);
    // the outline's outward normal
    vec2  n  = (qp.x > 0.0 && qp.y > 0.0) ? normalize(qp) * sg : (d.x > d.y ? vec2(sg.x, 0.0) : vec2(0.0, sg.y));
    float inside = -sdf;
    // True Glass's bevel (bevel_size 1.6, strength 0.25, shadow 0.18, light from 315 deg)
    float sizePx = max(1.6 * uScale, 1.0);
    float ring   = (1.0 - smoothstep(-0.35 * sizePx, 0.0, sdf)) * smoothstep(-sizePx, -0.35 * sizePx, sdf);
    float fc     = max(dot(n, BEVEL_LIGHT), 0.0), fc2 = fc * fc;
    float facing = clamp(dot(n, BEVEL_LIGHT) * 0.5 + 0.5, 0.0, 1.0);
    // mode 2: the bevel line alone, lit white or shaded black, over whatever is there
    if (uMode == 2) {
        float hi = ring * fc2 * fc2 * 0.25 * uForm, lo = ring * (1.0 - facing) * 0.18 * uForm;
        fragColor = vec4(vec3(hi), hi + lo) * (mask * uAlpha);
        return;
    }
    vec4  f     = crystalFacet(local / uScale);
    // (eased in over 12 px, as the engine's shards; they form as the pane opens)
    float inner = smoothstep(0.0, 12.0 * uScale, inside) * uForm;
    vec2  s     = f.xy * uShift * inner;

    // True Glass's physical rim (glass.frag): a slab uThickPx thick whose edge rolls
    // over across uRimPx; each colour refracts through it by its own index
    vec3  N = vec3(0.0, 0.0, 1.0);
    float slope = 0.0;
    vec2  oR = vec2(0.0), oG = vec2(0.0), oB = vec2(0.0);
    if (uRimPx > 0.0) {
        // depth: a thicker slab, a wider rim and a little more bend; the lens forms
        // with the open (materialize), so the pane is there at once and its bend grows
        float rim = max(min(uRimPx * (1.0 + 0.9 * uDepth), uRimMax), 1.0);
        float H   = uThickPx * (1.0 + 0.35 * uDepth) * uForm;
        float cap = min(0.5 * min(rim, uRimPx) * (1.0 + 0.25 * uDepth), uCapPx);
        float t   = clamp(inside / rim, 0.0, 1.0);
        float z   = H * rimHeight(t);
        slope     = t < 1.0 ? min(H * rimSlope(t) / rim, 40.0) : 0.0;
        N         = normalize(vec3(n * slope, 1.0));
        oG = refractOffsetPx(N, N_RGB.g, z, cap);
        oR = slope > 0.01 ? refractOffsetPx(N, N_RGB.r, z, cap) : oG;
        oB = slope > 0.01 ? refractOffsetPx(N, N_RGB.b, z, cap) : oG;
        // a thick pane sets what is behind it further off: smaller about its middle,
        // eased in from the rim so the rim's own bend stays continuous
        if (uDepth > 0.001) {
            vec2  o   = (local - half_) * (0.035 * uDepth);
            float lim = 22.0 * uScale, lo = length(o);
            if (lo > 1e-3) o *= lim * tanh(lo / lim) / lo;
            o *= smoothstep(0.0, 1.0, t) * uForm;
            oR += o; oG += o; oB += o;
        }
    }
    vec3  c = vec3(frosted(v_texcoord + (s * 0.94 + oR) / uRes).r, frosted(v_texcoord + (s + oG) / uRes).g,
                   frosted(v_texcoord + (s * 1.06 + oB) / uRes).b);
    // (the engine's shards: 0.4 of their milk unless light glass, as mix(0.4, 1.0, lightGlass);
    // light glass is milky already, so a third there, or the two stack to white)
    float milk  = uTone == 1 ? 0.33 : 0.4;
    float lift  = inner * milk * (uMilk * 0.12 + uVary * 0.20 * f.z + uEdge * 0.25 * (1.0 - smoothstep(0.0, 1.2, f.w)));
    c = tone(mix(c, vec3(1.0), lift));

    if (uRimPx > 0.0) {
        float edgeIn = smoothstep(0.0, 2.0 * uScale, inside);
        // Fresnel (Schlick): the tilted rim mirrors the scene round it (fresnel_strength 0.25)
        float m1 = 1.0 - clamp(N.z, 0.0, 1.0), m2 = m1 * m1;
        float fr = F0 + (1.0 - F0) * m2 * m2 * m1;
        float fw = clamp(fr * 0.25 * 2.2 * uForm, 0.0, 0.85) * edgeIn;
        if (fw > 0.0 && slope > 0.0) {
            vec2 eo  = v_texcoord + N.xy * 20.0 * uScale / uRes, es = vec2(6.0 * uScale) / uRes;
            vec3 env = (frosted(eo + vec2(es.x, es.y)) + frosted(eo + vec2(-es.x, es.y)) +
                        frosted(eo + vec2(es.x, -es.y)) + frosted(eo - es)) * 0.25;
            c = mix(c, tone(env), fw);
        }
        // the light above (specular_strength 0.35, 315 deg at 35 deg): an arc where the rim turns over
        if (N.z < 0.985) {
            float nl   = length(N.xy);
            // (a thicker pane scatters its light: the lit arc softer and a little wider)
            float band = smoothstep(0.955 - 0.18 * uDepth, 0.996, nl) * edgeIn * (1.0 - 0.35 * uDepth);
            float fl   = dot(N.xy, SPEC_LIGHT) / nl, f2 = fl * fl, f6 = f2 * f2 * f2;
            c += vec3(1.0, 0.99, 0.97) * (fl > 0.0 ? f6 : 0.35 * f6) * band * 0.35 * uForm;
        }
        c = mix(c, vec3(1.0), uBevel * ring * fc2 * fc2 * 0.25 * uForm);
        c = mix(c, vec3(0.0), uBevel * ring * (1.0 - facing) * 0.18 * uForm);
    }
    float a = mask * uAlpha;
    fragColor = vec4(c * a, a);
}
)#";

        SP<CShader> shader() {
            if (g_shader || g_shaderFailed)
                return g_shader;
            auto s = makeShared<CShader>();
            if (!s->createProgram(VERT, FRAG, true, true)) {
                g_shaderFailed = true;
                Log::logger->log(Log::ERR, "[hyprdrive] crystal shader failed to build; plates stay plain");
                return nullptr;
            }
            g_locMode  = glGetUniformLocation(s->program(), "uMode");
            g_locRes   = glGetUniformLocation(s->program(), "uRes");
            g_locBox   = glGetUniformLocation(s->program(), "uBox");
            g_locRound = glGetUniformLocation(s->program(), "uRound");
            g_locScale = glGetUniformLocation(s->program(), "uScale");
            g_locShift = glGetUniformLocation(s->program(), "uShift");
            g_locCell  = glGetUniformLocation(s->program(), "uCell");
            g_locAlpha = glGetUniformLocation(s->program(), "uAlpha");
            g_locMilk  = glGetUniformLocation(s->program(), "uMilk");
            g_locVary  = glGetUniformLocation(s->program(), "uVary");
            g_locEdge  = glGetUniformLocation(s->program(), "uEdge");
            g_locRimPx = glGetUniformLocation(s->program(), "uRimPx");
            g_locCapPx   = glGetUniformLocation(s->program(), "uCapPx");
            g_locThickPx = glGetUniformLocation(s->program(), "uThickPx");
            g_locRimMax  = glGetUniformLocation(s->program(), "uRimMax");
            g_locDepth   = glGetUniformLocation(s->program(), "uDepth");
            g_locForm    = glGetUniformLocation(s->program(), "uForm");
            g_locBevel  = glGetUniformLocation(s->program(), "uBevel");
            g_locTone   = glGetUniformLocation(s->program(), "uTone");
            g_locLightBase = glGetUniformLocation(s->program(), "uLightBase");
            g_locFrostTex  = glGetUniformLocation(s->program(), "frostTex");
            g_locFrostMix  = glGetUniformLocation(s->program(), "uFrostMix");
            g_locDir       = glGetUniformLocation(s->program(), "uDir");
            g_locBlurR     = glGetUniformLocation(s->program(), "uBlurR");
            g_shader   = s;
            return g_shader;
        }

        // Hyprland widens the damage round a live-blur element by 2.5 of these
        // (CRenderPass::render); a shard reaching no further reads only fresh pixels.
        float blurRadius() {
            const auto SIZE   = std::clamp<int64_t>(Config::getValue<int>("decoration:blur:size"), 1, 40);
            const auto PASSES = std::clamp<int64_t>(Config::getValue<int>("decoration:blur:passes"), 1, 8);
            return sc<float>(SIZE) * std::pow(2.F, sc<float>(PASSES));
        }

        // a panel Crystal setting (plugin:hyprdrive:crystal_*)
        float setting(const char* key) {
            return Config::getValue<float>(key);
        }

        // Glass's menu frost (Clear 0, Auto 0.5, Frosted 1)
        float frostSetting() {
            return std::clamp(Config::getValue<float>("plugin:hyprdrive:crystal_frost"), 0.F, 1.F);
        }
        // Hyprland's own blur, for a look that asks for it (the backdrop); plates and
        // previews take the engine's frost instead (frostInto)
        int frostPasses(const SLook& look) {
            return std::max(look.blurPasses, 0);
        }

        bool fitFB(SP<Render::IFramebuffer>& fb, const char* name, const Vector2D& size) {
            if (!fb)
                fb = g_pHyprRenderer->createFB(name);
            if (!fb)
                return false;
            if (!fb->isAllocated() || fb->m_size != size) {
                fb->release();
                if (!fb->alloc(sc<int>(size.x), sc<int>(size.y)))
                    return false;
            }
            return true;
        }

        struct SPlate {
            CBox  box;
            float rounding = 0.F, alpha = 1.F;
            SLook look;
            bool  bevelOnly = false; // mode 2: no copy, just the line
        };

        void drawQuad(const SP<CShader>& program, const SP<Render::ITexture>& tex, const CBox& full, const CRegion& clip, int mode, const SPlate& p, float scale,
                      float shift, float bend, const Vector2D& res) {
            auto transform = tex->m_transform;
            if (g_pHyprRenderer->monitorTransformEnabled())
                transform = Math::composeTransform(Math::wlTransformToHyprutils(Math::invertTransform(g_pHyprRenderer->m_renderData.pMonitor->m_transform)), transform);
            const auto MATRIX = g_pHyprRenderer->projectBoxToTarget(full, transform);

            glActiveTexture(GL_TEXTURE0);
            tex->bind();
            tex->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            tex->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            tex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            tex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);

            const auto S = Render::GL::g_pHyprOpenGL->useShader(program);
            S->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, MATRIX.getMatrix());
            S->setUniformInt(SHADER_TEX, 0);
            glUniform1i(g_locMode, mode);
            glUniform2f(g_locRes, sc<float>(res.x), sc<float>(res.y));
            glUniform4f(g_locBox, sc<float>(p.box.x), sc<float>(p.box.y), sc<float>(p.box.w), sc<float>(p.box.h));
            glUniform1f(g_locRound, std::min(p.rounding, sc<float>(std::min(p.box.w, p.box.h)) * 0.5F));
            glUniform1f(g_locScale, scale);
            glUniform1f(g_locShift, shift);
            // Physical glass materializes, as the engine's: the pane is there almost at once
            // (2.5x its fade) while its lens and light grow with the fade itself
            const float FORM = std::clamp(p.alpha, 0.F, 1.F);
            glUniform1f(g_locForm, p.look.rim ? FORM : 1.F);
            glUniform1f(g_locAlpha, p.look.rim ? std::min(1.F, FORM * 2.5F) : p.alpha);
            glUniform1f(g_locCell, setting("plugin:hyprdrive:crystal_cell"));
            glUniform1f(g_locMilk, p.look.milk * setting("plugin:hyprdrive:crystal_milk"));
            glUniform1f(g_locVary, p.look.vary * setting("plugin:hyprdrive:crystal_milk"));
            glUniform1f(g_locEdge, p.look.edge * setting("plugin:hyprdrive:crystal_edge"));
            // (the rim at most 30 % of the plate's short side: at the engine's half, the
            // dock was all rim, one gentle slope with no edge to see bend)
            glUniform1f(g_locRimPx, p.look.rim ? RIM * scale : 0.F);
            glUniform1f(g_locRimMax, sc<float>(std::min(p.box.w, p.box.h)) * 0.3F);
            // (the engine caps the bend at half the rim; here also within the damage Hyprland widens)
            glUniform1f(g_locCapPx, bend);
            glUniform1f(g_locThickPx, THICK * scale);
            glUniform1f(g_locDepth, std::clamp(Config::getValue<float>("plugin:hyprdrive:crystal_depth"), 0.F, 1.F));
            glUniform1f(g_locBevel, p.look.bevel ? 1.F : 0.F);
            glUniform1i(g_locTone, p.look.tone ? Config::getValue<int>("plugin:hyprdrive:crystal_tone") : 0);
            // (the engine's lightBase for a frosted card: 0.25, plus 0.3 of its frost)
            glUniform1f(g_locLightBase, 0.25F + 0.3F * frostSetting());
            glUniform2f(g_locDir, sc<float>(g_blurDir.x), sc<float>(g_blurDir.y));
            glUniform1f(g_locBlurR, g_blurR);
            const bool FROSTED = mode == 1 && g_frostTex && g_frostTex->ok() && g_frostMix > 0.F;
            glUniform1f(g_locFrostMix, FROSTED ? g_frostMix : 0.F);
            glUniform1i(g_locFrostTex, 1);
            glActiveTexture(GL_TEXTURE1);
            if (FROSTED) {
                g_frostTex->bind();
                g_frostTex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                g_frostTex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            } else
                tex->bind();
            glActiveTexture(GL_TEXTURE0);

            glBindVertexArray(S->getUniformLocation(SHADER_SHADER_VAO));
            if (clip.empty()) {
                // (a half-size buffer: the monitor's scissor rects would not fit it)
                Render::GL::g_pHyprOpenGL->scissor(nullptr);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            } else
                clip.forEachRect([](const auto& RECT) {
                    Render::GL::g_pHyprOpenGL->scissor(&RECT, g_pHyprRenderer->m_renderData.transformDamage);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                });
            Render::GL::g_pHyprOpenGL->scissor(nullptr);
            glBindVertexArray(0);
            tex->unbind();
        }

        class CCrystalPassElement : public IPassElement {
          public:
            explicit CCrystalPassElement(const SPlate& p) : m_plate(p) {}

            std::vector<UP<IPassElement>> draw() override {
                const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
                const auto CUR     = g_pHyprRenderer->m_renderData.currentFB;
                const auto PROGRAM = shader();
                if (!MONITOR || !CUR || !PROGRAM)
                    return {};
                const auto CURTEX = CUR->getTexture();
                if (!CURTEX || !CURTEX->ok())
                    return {};
                if (m_plate.bevelOnly) {
                    const Vector2D RES = MONITOR->m_pixelSize;
                    CRegion        clip{g_pHyprRenderer->m_renderData.damage};
                    clip.intersect(m_plate.box);
                    if (!clip.empty() && RES.x >= 2 && RES.y >= 2)
                        drawQuad(PROGRAM, CURTEX, CBox{0, 0, RES.x, RES.y}, clip, 2, m_plate, std::max(0.25F, sc<float>(MONITOR->m_scale)), 0.F, 0.F, RES);
                    return {};
                }

                const Vector2D RES   = MONITOR->m_pixelSize;
                const float    SCALE = std::max(0.25F, sc<float>(MONITOR->m_scale));
                if (RES.x < 2 || RES.y < 2)
                    return {};
                // (a shard's tilt is at most sqrt 2 long)
                // (shard and rim together stay within the damage Hyprland widens: see blurRadius)
                const float REACH   = blurRadius() * 1.4F;
                const float BENDPX  = std::min(BEND * SCALE * 1.25F, REACH * 0.35F); // (1.25: the cap at full depth)
                const float SHIFTPX = std::min(SHIFT * SCALE * m_plate.look.shift * setting("plugin:hyprdrive:crystal_shift"), REACH - BENDPX - 22.F * SCALE) / 1.4143F;
                const CBox  FULL{0, 0, RES.x, RES.y};
                // (the rim's reflection reads up to 26 logical px beyond it)
                // (the engine's frost reaches 1.5 of its radius: two gaussian passes at half size)
                const bool  ENGINEFROST = m_plate.look.blurPasses < 0 && frostSetting() > 0.F;
                const float FROSTREACH  = ENGINEFROST ? 1.5F * FROST_BLUR * SCALE : 0.F;
                const CBox  READ = m_plate.box.copy().expand(SHIFTPX * 1.4143F + std::max(BENDPX + 22.F * SCALE, 26.F * SCALE) + FROSTREACH + 2.F).intersection(FULL);

                CRegion clip{g_pHyprRenderer->m_renderData.damage};
                clip.intersect(m_plate.box);
                if (clip.empty() || READ.empty())
                    return {};

                if (!g_copy)
                    g_copy = g_pHyprRenderer->createFB("hyprdrive-crystal");
                if (!g_copy)
                    return {};
                if (!g_copy->isAllocated() || g_copy->m_size != RES) {
                    g_copy->release();
                    if (!g_copy->alloc(sc<int>(RES.x), sc<int>(RES.y)))
                        return {};
                }

                // Frosted first, at this plate's own blur (decoration:blur is off here)
                SP<Render::ITexture> source;
                const int PASSES = frostPasses(m_plate.look);
                if (PASSES > 0) {
                    const auto PREVSIZE   = Config::getValue<int>("decoration:blur:size");
                    const auto PREVPASSES = Config::getValue<int>("decoration:blur:passes");
                    auto       restore    = Hyprutils::Utils::CScopeGuard([PREVSIZE, PREVPASSES] {
                        Config::setValue("decoration:blur:size", PREVSIZE);
                        Config::setValue("decoration:blur:passes", PREVPASSES);
                    });
                    Config::setValue("decoration:blur:size", BLUR_SIZE);
                    Config::setValue("decoration:blur:passes", PASSES);
                    CRegion blurDamage{READ};
                    source = g_pHyprRenderer->blurMainFramebuffer(1.F, &blurDamage);
                }
                if (!source || !source->ok())
                    source = CURTEX;

                Render::GL::g_pHyprOpenGL->blend(false);
                {
                    const auto GUARD = g_pHyprRenderer->bindTempFB(g_copy);
                    drawQuad(PROGRAM, source, FULL, CRegion{READ}, 0, m_plate, SCALE, SHIFTPX, BENDPX, RES);
                }
                const auto COPYTEX = g_copy->getTexture();
                if (!COPYTEX || !COPYTEX->ok()) {
                    Render::GL::g_pHyprOpenGL->blend(true);
                    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    return {};
                }

                // True Glass's frost (Frost.cpp): the copy at half size, then two gaussian
                // iterations at half its radius, each one pass across and one down
                g_frostTex.reset();
                g_frostMix = 0.F;
                const Vector2D HALF{std::max(1.0, std::floor(RES.x / 2.0)), std::max(1.0, std::floor(RES.y / 2.0))};
                if (ENGINEFROST && fitFB(g_half[0], "hyprdrive-crystal-frost-a", HALF) && fitFB(g_half[1], "hyprdrive-crystal-frost-b", HALF)) {
                    const auto pass = [&](const SP<Render::ITexture>& from, int into, int mode) {
                        const auto GUARD = g_pHyprRenderer->bindTempFB(g_half[into]);
                        Render::GL::g_pHyprOpenGL->setViewport(0, 0, sc<int>(HALF.x), sc<int>(HALF.y));
                        drawQuad(PROGRAM, from, FULL, CRegion{}, mode, m_plate, SCALE, 0.F, 0.F, RES);
                    };
                    g_blurR = std::max(1.F, FROST_BLUR * SCALE * 0.5F);
                    pass(COPYTEX, 0, 0);
                    for (int i = 0; i < 2; i++) {
                        g_blurDir = {1.0 / HALF.x, 0.0};
                        pass(g_half[0]->getTexture(), 1, 3);
                        g_blurDir = {0.0, 1.0 / HALF.y};
                        pass(g_half[1]->getTexture(), 0, 3);
                    }
                    g_frostTex = g_half[0]->getTexture();
                    g_frostMix = frostSetting();
                }

                Render::GL::g_pHyprOpenGL->blend(true);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // Hyprland's premultiplied default
                drawQuad(PROGRAM, COPYTEX, FULL, clip, 1, m_plate, SCALE, SHIFTPX, BENDPX, RES);
                g_frostTex.reset();
                return {};
            }

            ePassElementType type() override {
                return EK_CUSTOM;
            }
            // (for the damage Hyprland widens round it, not for a blur; the line reads nothing)
            bool needsLiveBlur() override {
                return !m_plate.bevelOnly;
            }
            bool needsPrecomputeBlur() override {
                return false;
            }
            std::optional<CBox> boundingBox() override {
                const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
                if (!MONITOR)
                    return std::nullopt;
                return m_plate.box.copy().scale(1.F / std::max(0.25F, sc<float>(MONITOR->m_scale))).round();
            }
            CRegion opaqueRegion() override {
                return {};
            }
            const char* passName() override {
                return PASS_NAME;
            }

          private:
            SPlate m_plate;
        };
    }

    bool enabled() {
        static auto& VAL = Config::valueRef<Hyprlang::INT>("plugin:hyprdrive:crystal");
        return *VAL != 0;
    }

    UP<IPassElement> element(const CBox& box, int rounding, float alpha, const SLook& look) {
        if (!std::isfinite(box.x) || !std::isfinite(box.y) || box.w < 1 || box.h < 1 || alpha <= 0.F)
            return nullptr;
        return makeUnique<CCrystalPassElement>(SPlate{.box = box, .rounding = sc<float>(rounding), .alpha = alpha, .look = look});
    }

    void bevel(const CBox& box, int rounding, float alpha) {
        if (!enabled() || !std::isfinite(box.x) || !std::isfinite(box.y) || box.w < 1 || box.h < 1 || alpha <= 0.F)
            return;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CCrystalPassElement>(SPlate{.box = box, .rounding = sc<float>(rounding), .alpha = alpha, .bevelOnly = true}));
    }

    void queue(const CBox& box, int rounding, float alpha, const SLook& look) {
        if (auto el = element(box, rounding, alpha, look))
            g_pHyprRenderer->m_renderPass.add(std::move(el));
    }

    // (True Glass keys its cards' own background down to 3 %: fill_alpha in glass.lua)
    float backdropDim() {
        return std::clamp(setting("plugin:hyprdrive:crystal_backdrop_dim"), 0.F, 0.9F);
    }

    float tintAlpha(float a) {
        return enabled() ? a * std::clamp(setting("plugin:hyprdrive:crystal_tint"), 0.F, 1.F) : a;
    }

    void shutdown() {
        if (Render::GL::g_pHyprOpenGL && (g_shader || g_copy || g_half[0] || g_half[1])) {
            Render::GL::g_pHyprOpenGL->makeEGLCurrent();
            g_shader.reset();
            if (g_copy)
                g_copy->release();
            g_copy.reset();
            for (auto& fb : g_half) {
                if (fb)
                    fb->release();
                fb.reset();
            }
            g_frostTex.reset();
        }
        g_shaderFailed = false;
    }
}
