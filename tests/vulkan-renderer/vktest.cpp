// Compares the Vulkan VDP renderer against the software renderer on synthetic scenes.
//
// Each scene configures VDP1/VDP2 through the bus on a fresh Saturn running a spinning IPL (no BIOS needed), runs a few
// frames with each renderer and diffs the delivered frames. The first frame is skipped: it's the frame in which the
// scene turns the display on.
//
// Usage: vktest [name filter]
// Environment variables:
//   SCALE=<n>          run the Vulkan renderer at an internal resolution of n and compare against the software
//                      renderer's output upscaled with nearest neighbor filtering
//   LAYER_MASK=<mask>  enable only the given VDP2 layers (debug render option bits)
//   SAVE=1             dump mismatching frames as PPM images
//   YMIR_VULKAN_DEVICE=<name substring>, YMIR_VULKAN_VALIDATION=1  (see the renderer)

#include <ymir/hw/vdp/renderer/vdp_renderer.hpp>
#include <ymir/sys/saturn.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace ymir;

struct Frame {
    std::vector<uint32> px;
    uint32 w = 0, h = 0;
};

struct Bus {
    Saturn &s;
    void W16(uint32 a, uint16 v) {
        s.mainBus.Write<uint16>(a, v);
    }
    void Reg2(uint32 off, uint16 v) {
        W16(0x5F80000 + off, v);
    }
    void Reg1(uint32 off, uint16 v) {
        W16(0x5D00000 + off, v);
    }
    void VRAM2(uint32 a, uint16 v) {
        W16(0x5E00000 + a, v);
    }
    void CRAM(uint32 a, uint16 v) {
        W16(0x5F00000 + a, v);
    }
    void VRAM1(uint32 a, uint16 v) {
        W16(0x5C00000 + a, v);
    }
};

struct Cmd {
    uint16 ctrl = 0, link = 0, pmod = 0, colr = 0, srca = 0, size = 0;
    sint16 x[4]{}, y[4]{};
    uint16 grda = 0;
};

static void WriteCmd(Bus &b, uint32 index, const Cmd &c) {
    const uint32 a = index * 0x20;
    b.VRAM1(a + 0x00, c.ctrl);
    b.VRAM1(a + 0x02, c.link);
    b.VRAM1(a + 0x04, c.pmod);
    b.VRAM1(a + 0x06, c.colr);
    b.VRAM1(a + 0x08, c.srca);
    b.VRAM1(a + 0x0A, c.size);
    for (int i = 0; i < 4; ++i) {
        b.VRAM1(a + 0x0C + i * 4, (uint16)c.x[i]);
        b.VRAM1(a + 0x0E + i * 4, (uint16)c.y[i]);
    }
    b.VRAM1(a + 0x1C, c.grda);
}

using Scene = std::function<void(Bus &, int frame)>;

enum class Kind { SW, VK };

static std::vector<Frame> Run(Kind kind, const Scene &scene, int numFrames, vdp::config::Enhancements enh, bool &ok) {
    auto sat = std::make_unique<Saturn>();
    sat->configuration.swRenderer.threadedVDP1 = false;
    sat->configuration.swRenderer.threadedVDP2 = false;
    sat->configuration.swRenderer.threadedDeinterlacer = false;

    // IPL: initial PC = 0x10, SP = 0x06004000; at 0x10: bra . ; nop
    static std::array<uint8, sys::kIPLSize> ipl{};
    ipl.fill(0);
    const uint8 prog[] = {0, 0, 0, 0x10, 0x06, 0x00, 0x40, 0x00};
    std::memcpy(ipl.data(), prog, sizeof(prog));
    ipl[0x10] = 0xAF;
    ipl[0x11] = 0xFE;
    ipl[0x12] = 0x00;
    ipl[0x13] = 0x09;
    sat->LoadIPL(ipl);

    std::vector<Frame> frames;
    auto cb = [&](uint32 *fb, uint32 w, uint32 h) {
        Frame f;
        f.w = w;
        f.h = h;
        f.px.assign(fb, fb + w * h);
        frames.push_back(std::move(f));
    };
    struct Ctx {
        decltype(cb) *fn;
    } ctx{&cb};
    auto trampoline = [](uint32 *fb, uint32 w, uint32 h, void *c) { (*static_cast<Ctx *>(c)->fn)(fb, w, h); };

    if (kind == Kind::VK) {
        const char *scaleEnv = std::getenv("SCALE");
        auto r = sat->VDP.UseVulkanRenderer(scaleEnv ? std::atoi(scaleEnv) : 1);
        if (!r) {
            std::printf("Vulkan renderer failed: %s\n", r.Error().message.c_str());
            ok = false;
            return {};
        }
        sat->VDP.SetVulkanFrameReadyCallback({&ctx, trampoline});
    } else {
        sat->VDP.SetSoftwareRenderCallback({&ctx, trampoline});
    }
    sat->VDP.SetEnhancements(enh);
    if (const char *lm = std::getenv("LAYER_MASK")) {
        const int mask = std::atoi(lm);
        for (int i = 0; i < 6; ++i) {
            sat->VDP.vdp2DebugRenderOptions.enabledLayers[i] = (mask >> i) & 1;
        }
        sat->VDP.GetRenderer().UpdateEnabledLayers();
    }
    sat->Reset(true);

    Bus b{*sat};
    for (int i = 0; i < numFrames; ++i) {
        scene(b, i);
        sat->RunFrame();
    }
    ok = true;
    return frames;
}

static int g_failures = 0;

static void SavePPM(const std::string &fn, const Frame &f) {
    FILE *fp = std::fopen(fn.c_str(), "wb");
    std::fprintf(fp, "P6 %u %u 255\n", f.w, f.h);
    for (uint32 p : f.px) {
        uint8 rgb[3] = {uint8(p), uint8(p >> 8), uint8(p >> 16)};
        std::fwrite(rgb, 1, 3, fp);
    }
    std::fclose(fp);
}

static bool Compare(const std::string &name, const std::vector<Frame> &a, const std::vector<Frame> &b,
                    size_t firstFrame) {
    const size_t n = std::min(a.size(), b.size());
    if (n <= firstFrame) {
        std::printf("[%s] not enough frames (sw=%zu vk=%zu)\n", name.c_str(), a.size(), b.size());
        ++g_failures;
        return false;
    }
    const bool save = std::getenv("SAVE") != nullptr;
    bool okAll = true;
    for (size_t i = firstFrame; i < n; ++i) {
        const Frame &fa = a[i];
        const Frame &fb = b[i];
        if (fa.w != fb.w || fa.h != fb.h) {
            std::printf("[%s] frame %zu size mismatch sw=%ux%u vk=%ux%u\n", name.c_str(), i, fa.w, fa.h, fb.w, fb.h);
            okAll = false;
            continue;
        }
        size_t diffs = 0;
        int firstX = -1, firstY = -1;
        uint32 va = 0, vb = 0;
        for (uint32 y = 0; y < fa.h; ++y) {
            for (uint32 x = 0; x < fa.w; ++x) {
                const uint32 pa = fa.px[y * fa.w + x] & 0xFFFFFF;
                const uint32 pb = fb.px[y * fb.w + x] & 0xFFFFFF;
                if (pa != pb) {
                    if (diffs == 0) {
                        firstX = x, firstY = y, va = pa, vb = pb;
                    }
                    ++diffs;
                }
            }
        }
        if (diffs) {
            okAll = false;
            std::printf("[%s] frame %zu: %zu/%u px differ (%.2f%%); first at (%d,%d) sw=%06X vk=%06X\n", name.c_str(),
                        i, diffs, fa.w * fa.h, 100.0 * diffs / (fa.w * fa.h), firstX, firstY, va, vb);
            if (save) {
                SavePPM(name + "_sw" + std::to_string(i) + ".ppm", fa);
                SavePPM(name + "_vk" + std::to_string(i) + ".ppm", fb);
            }
        }
    }
    bool uniform = true;
    for (size_t i = firstFrame; i < n && uniform; ++i) {
        for (uint32 p : a[i].px) {
            if ((p & 0xFFFFFF) != (a[i].px[0] & 0xFFFFFF)) {
                uniform = false;
                break;
            }
        }
    }
    std::printf("[%s] %s (%zu frames compared%s)\n", name.c_str(), okAll ? "OK" : "MISMATCH", n - firstFrame,
                uniform ? ", WARNING: image is uniform" : "");
    if (!okAll) {
        ++g_failures;
    }
    return okAll;
}

// ---------------------------------------------------------------------------------------------------------------------
// Scene building blocks

// VDP2 setup: display on, 320x224, VRAM cycle patterns granting NBG0-3 access, single-color back screen
static void SetupVDP2Basic(Bus &b, uint16 tvmd = 0x8000) {
    b.Reg2(0x000, tvmd);
    b.Reg2(0x00E, 0x0000);
    for (uint32 off = 0x010; off <= 0x01C; off += 4) {
        b.Reg2(off, 0x0123);
        b.Reg2(off + 2, 0x4567);
    }
    b.Reg2(0x0AC, 0x0003);
    b.Reg2(0x0AE, 0xFFFF);
    b.VRAM2(0x7FFFE, 0x8000 | (8 << 10) | (4 << 5) | 2);
}

// Fills a 4bpp character set and CRAM with random data
static void FillCharsAndPalettes(Bus &b, uint32 charBase, uint32 numChars, uint32 seed) {
    std::mt19937 rng(seed);
    for (uint32 c = 0; c < numChars; ++c) {
        for (uint32 i = 0; i < 32; i += 2) {
            b.VRAM2(charBase + c * 32 + i, (uint16)rng());
        }
    }
    for (uint32 i = 0; i < 1024; ++i) {
        b.CRAM(i * 2, (uint16)(rng() & 0xFFFF));
    }
}

// NBGn with 1-word pattern names, 1x1 cells, 1x1 planes, all maps at mapAddr
static void SetupNBG(Bus &b, int n, uint32 mapAddr, uint32 charAddr, uint32 seed) {
    std::mt19937 rng(seed);
    const uint32 charNumBase = charAddr / 0x20;
    for (uint32 i = 0; i < 64 * 64; ++i) {
        const uint16 pal = rng() & 0xF;
        const uint16 ch = (charNumBase + (rng() % 256)) & 0x3FF;
        const uint16 flips = rng() & 0x0C00;
        b.VRAM2(mapAddr + i * 2, (pal << 12) | flips | ch);
    }
    b.Reg2(0x030 + n * 2, 0x8000 | ((charNumBase >> 10) & 0x7));
    const uint16 mapNum = mapAddr / 0x2000;
    b.Reg2(0x040 + n * 4, mapNum | (mapNum << 8));
    b.Reg2(0x042 + n * 4, mapNum | (mapNum << 8));
}

// Common VDP1 setup: 16bpp framebuffer, automatic swap and draw, full screen erase
static void SetupVDP1(Bus &b, uint16 eraseRight = ((320 / 8) << 9) | 223) {
    b.Reg1(0x00, 0x0000);
    b.Reg1(0x02, 0x0000);
    b.Reg1(0x06, 0x0000);
    b.Reg1(0x08, 0x0000);
    b.Reg1(0x0A, eraseRight);
    b.Reg1(0x04, 0x0002);
}

// Sprite layer setup: mixed palette/RGB, priority 5 for all sprite registers
static void SetupSpriteLayer(Bus &b) {
    b.Reg2(0x0E0, 0x0020);
    for (uint32 r = 0x0F0; r <= 0x0F6; r += 2) {
        b.Reg2(r, 0x0505);
    }
}

static uint32 BeginCommandList(Bus &b) {
    uint32 idx = 0;
    Cmd c{};
    c.ctrl = 0x0009; // system clip
    c.x[2] = 319;
    c.y[2] = 223;
    WriteCmd(b, idx++, c);
    c = {};
    c.ctrl = 0x000A; // local coordinates
    WriteCmd(b, idx++, c);
    return idx;
}

static void EndCommandList(Bus &b, uint32 idx) {
    Cmd c{};
    c.ctrl = 0x8000;
    WriteCmd(b, idx, c);
}

// Random VDP1 content over NBG0: polygons, polylines, lines, distorted/normal/scaled sprites and user clipping in
// every color calculation mode. onlyMode/onlyKind restrict the modes and command kinds.
static Scene MakeVDP1Scene(uint32 seed, bool random, int onlyMode = -1, int onlyKind = -1, bool nbg = true) {
    return [=](Bus &b, int f) {
        if (f == 0) {
            SetupVDP2Basic(b);
            FillCharsAndPalettes(b, 0x10000, 512, seed);
            SetupNBG(b, 0, 0x00000, 0x10000, seed + 1);
            b.Reg2(0x020, nbg ? 0x0001 : 0x0000);
            b.Reg2(0x0F8, 0x0003);
            b.Reg2(0x078, 0x0001);
            b.Reg2(0x07C, 0x0001);
            SetupSpriteLayer(b);
            b.Reg2(0x0EC, 0x0040); // sprite color calculation
            b.Reg2(0x100, 0x0808);
            SetupVDP1(b);

            std::mt19937 rng(seed + 7);
            for (uint32 i = 0; i < 64; ++i) {
                b.VRAM1(0x70000 + i * 2, (uint16)rng()); // gouraud tables
            }
            for (uint32 i = 0; i < 0x8000; i += 2) {
                b.VRAM1(0x40000 + i, (uint16)rng()); // textures
            }
        }

        std::mt19937 rng(seed * 31 + f);
        uint32 idx = BeginCommandList(b);
        static const uint16 kModes[] = {
            0x00C0, // replace
            0x00C4, // gouraud
            0x00C3, // half-transparency
            0x00C1, // shadow
            0x00C2, // half-luminance
            0x01C0, // mesh
            0x80C0, // MSB on
            0x00C7, // gouraud + half-transparency
        };
        const int numPolys = random ? 40 : 16;
        for (int p = 0; p < numPolys; ++p) {
            Cmd pc{};
            int kind = random ? (int)(rng() % 7) : p % 7;
            uint16 mode = kModes[random ? rng() % 8 : p % 8];
            if (onlyKind >= 0) {
                kind = onlyKind;
            }
            if (onlyMode >= 0) {
                mode = kModes[onlyMode];
            }
            pc.pmod = mode;
            pc.colr = 0x8000 | (uint16)(rng() & 0x7FFF);
            pc.grda = (uint16)((0x70000 + (rng() % 8) * 8) >> 3);
            const int cx = rng() % 360 - 20, cy = rng() % 260 - 18;
            for (int k = 0; k < 4; ++k) {
                pc.x[k] = cx + (int)(rng() % 90) - 45;
                pc.y[k] = cy + (int)(rng() % 90) - 45;
            }
            switch (kind) {
            case 0: pc.ctrl = 0x0004; break; // polygon
            case 1: pc.ctrl = 0x0005; break; // polyline
            case 2: pc.ctrl = 0x0006; break; // line
            case 3:                          // distorted sprite, 16bpp RGB
                pc.ctrl = 0x0002 | ((rng() & 3) << 4);
                pc.pmod = (mode & ~0x0038) | 0x0028;
                pc.srca = 0x40000 >> 3;
                pc.size = ((16 / 8) << 8) | 16;
                break;
            case 4: // normal sprite, 4bpp bank
                pc.ctrl = 0x0000 | ((rng() & 3) << 4);
                pc.pmod = (mode & ~0x0038) | (rng() & 0x80);
                pc.colr = 0x0100;
                pc.srca = 0x44000 >> 3;
                pc.size = ((32 / 8) << 8) | 24;
                break;
            case 5: // scaled sprite, 8bpp
                pc.ctrl = 0x0001 | ((rng() & 0xF) << 8);
                pc.pmod = (mode & ~0x0038) | 0x0020;
                pc.colr = 0x0000;
                pc.srca = 0x46000 >> 3;
                pc.size = ((24 / 8) << 8) | 20;
                pc.x[1] = 30 + rng() % 60;
                pc.y[1] = 30 + rng() % 60;
                break;
            case 6: { // user clipping on polygon
                Cmd uc{};
                uc.ctrl = 0x0008;
                uc.x[0] = 40;
                uc.y[0] = 30;
                uc.x[2] = 280;
                uc.y[2] = 190;
                WriteCmd(b, idx++, uc);
                pc.ctrl = 0x0004;
                pc.pmod |= 0x0400 | ((rng() & 1) << 9);
                break;
            }
            }
            WriteCmd(b, idx++, pc);
        }
        EndCommandList(b, idx);
    };
}

struct Test {
    std::string name;
    Scene scene;
    int frames = 4;
    vdp::config::Enhancements enh{};
    size_t firstFrame = 1;
};

static std::vector<Test> MakeTests() {
    std::vector<Test> tests;

    tests.push_back({"backscreen", [](Bus &b, int f) {
                         if (f == 0) {
                             SetupVDP2Basic(b);
                         }
                     }});

    tests.push_back({"nbg_tiles", [](Bus &b, int f) {
                         if (f == 0) {
                             SetupVDP2Basic(b);
                             FillCharsAndPalettes(b, 0x10000, 512, 1);
                             SetupNBG(b, 0, 0x00000, 0x10000, 2);
                             SetupNBG(b, 1, 0x02000, 0x10000, 3);
                             SetupNBG(b, 2, 0x04000, 0x10000, 4);
                             SetupNBG(b, 3, 0x06000, 0x10000, 5);
                             b.Reg2(0x020, 0x000F);
                             b.Reg2(0x028, 0x0000);
                             b.Reg2(0x02A, 0x0000);
                             b.Reg2(0x0F8, 0x0405);
                             b.Reg2(0x0FA, 0x0203);
                             b.Reg2(0x078, 0x0001);
                             b.Reg2(0x07C, 0x0001);
                             b.Reg2(0x088, 0x0001);
                             b.Reg2(0x08C, 0x0001);
                         }
                         b.Reg2(0x070, f * 3);
                         b.Reg2(0x074, f * 2);
                         b.Reg2(0x080, 100 - f);
                         b.Reg2(0x090, f * 5);
                         b.Reg2(0x094, f);
                         b.Reg2(0x098, 7 * f);
                     }});

    tests.push_back({"nbg_zoom_cc", [](Bus &b, int f) {
                         if (f == 0) {
                             SetupVDP2Basic(b);
                             FillCharsAndPalettes(b, 0x10000, 512, 11);
                             SetupNBG(b, 0, 0x00000, 0x10000, 12);
                             SetupNBG(b, 1, 0x02000, 0x10000, 13);
                             b.Reg2(0x020, 0x0003);
                             b.Reg2(0x0F8, 0x0305);
                             b.Reg2(0x078, 0x0000);
                             b.Reg2(0x07A, 0xC000);
                             b.Reg2(0x07C, 0x0001);
                             b.Reg2(0x07E, 0x4000);
                             b.Reg2(0x088, 0x0001);
                             b.Reg2(0x08C, 0x0001);
                             b.Reg2(0x0EC, 0x0001);
                             b.Reg2(0x108, 0x000C);
                             b.Reg2(0x110, 0x0001);
                             b.Reg2(0x114, 0x0040);
                             b.Reg2(0x116, 0x01E0);
                             b.Reg2(0x118, 0x0010);
                         }
                         b.Reg2(0x070, f * 5);
                     }});

    tests.push_back({"vdp1_polys", MakeVDP1Scene(100, false), 5});
    for (int m = 0; m < 8; ++m) {
        for (int k = 0; k < 7; ++k) {
            tests.push_back({"iso_m" + std::to_string(m) + "_k" + std::to_string(k),
                             MakeVDP1Scene(500 + m * 7 + k, true, m, k, false), 4});
        }
    }
    for (int i = 0; i < 6; ++i) {
        tests.push_back({"vdp1_random" + std::to_string(i), MakeVDP1Scene(200 + i * 17, true), 5});
    }
    {
        Test t{"vdp1_meshes_transparent", MakeVDP1Scene(300, true), 5};
        t.enh.transparentMeshes = true;
        tests.push_back(t);
    }

    // Single user-clipped polygon cases
    struct ClipCase {
        const char *name;
        sint16 x[4], y[4];
        bool outside;
    };
    static const ClipCase kClipCases[] = {
        {"in_in", {100, 150, 150, 100}, {60, 60, 120, 120}, false},
        {"in_out", {100, 150, 150, 100}, {60, 60, 120, 120}, true},
        {"straddle_in", {10, 150, 150, 10}, {10, 10, 120, 120}, false},
        {"straddle_out", {10, 150, 150, 10}, {10, 10, 120, 120}, true},
        {"out_in", {200, 260, 230, 230}, {0, 0, 25, 25}, false},
        {"out_out", {200, 260, 230, 230}, {0, 0, 25, 25}, true},
        {"tri_in", {70, 90, 90, 70}, {190, 160, 190, 190}, false},
        {"tri_out", {70, 90, 90, 70}, {190, 160, 190, 190}, true},
    };
    for (const ClipCase &cc : kClipCases) {
        tests.push_back({std::string("clip_") + cc.name, [cc](Bus &b, int f) {
                             if (f == 0) {
                                 SetupVDP2Basic(b);
                                 b.Reg2(0x020, 0x0000);
                                 SetupSpriteLayer(b);
                                 SetupVDP1(b);
                             }
                             uint32 idx = BeginCommandList(b);
                             Cmd c{};
                             c.ctrl = 0x0008;
                             c.x[0] = 40;
                             c.y[0] = 30;
                             c.x[2] = 280;
                             c.y[2] = 190;
                             WriteCmd(b, idx++, c);
                             c = {};
                             c.ctrl = 0x0004;
                             c.pmod = 0x04C0 | (cc.outside ? 0x0200 : 0);
                             c.colr = 0xFC1F;
                             for (int k = 0; k < 4; ++k) {
                                 c.x[k] = cc.x[k];
                                 c.y[k] = cc.y[k];
                             }
                             WriteCmd(b, idx++, c);
                             EndCommandList(b, idx);
                         }});
    }

    // Degenerate textures: zero-sized and wrapping around the end of VDP1 VRAM (crashed Daytona USA/Panzer Dragoon)
    tests.push_back({"vdp1_degenerate_textures", [](Bus &b, int f) {
                         if (f == 0) {
                             SetupVDP2Basic(b);
                             b.Reg2(0x020, 0x0000);
                             SetupSpriteLayer(b);
                             SetupVDP1(b);
                             std::mt19937 rng(77);
                             for (uint32 i = 0x1000; i < 0x80000; i += 2) {
                                 b.VRAM1(i, (uint16)rng());
                             }
                         }
                         uint32 idx = BeginCommandList(b);
                         static const uint16 kSizes[] = {0x0000, 0x0008, 0x0100, 0x3FFF, 0x0820};
                         static const uint16 kSrcAddrs[] = {0x0200, 0xFFFF, 0xFFF8, 0xFF00, 0x8000};
                         for (int i = 0; i < 5; ++i) {
                             for (int k = 0; k < 3; ++k) {
                                 Cmd c{};
                                 c.ctrl = k; // normal, scaled, distorted
                                 c.pmod = 0x00E8;
                                 c.srca = kSrcAddrs[i];
                                 c.size = kSizes[i];
                                 c.x[0] = 20 + i * 60;
                                 c.y[0] = 20 + k * 70;
                                 c.x[1] = c.x[0] + 40;
                                 c.y[1] = c.y[0];
                                 c.x[2] = c.x[0] + 40;
                                 c.y[2] = c.y[0] + 50;
                                 c.x[3] = c.x[0];
                                 c.y[3] = c.y[0] + 50;
                                 if (k == 1) {
                                     c.x[1] = 40;
                                     c.y[1] = 50;
                                 }
                                 WriteCmd(b, idx++, c);
                             }
                         }
                         EndCommandList(b, idx);
                     }});

    // Hi-res + double-density interlace, with and without deinterlacing
    {
        auto base = MakeVDP1Scene(400, true);
        auto scene = [base](Bus &b, int f) {
            base(b, f);
            if (f == 0) {
                SetupVDP2Basic(b, 0x80C2);
                b.Reg1(0x0A, ((639 / 8) << 9) | 223);
            }
        };
        tests.push_back({"hires_interlace", scene, 6});
        Test t{"hires_interlace_deint", scene, 6};
        t.enh.deinterlace = true;
        tests.push_back(t);
    }

    // RBG0 rotation: characters in bank A0, pattern names in A1, parameter table in B1
    tests.push_back({"rbg0", [](Bus &b, int f) {
                         if (f == 0) {
                             SetupVDP2Basic(b);
                             FillCharsAndPalettes(b, 0x00000, 256, 55);
                             std::mt19937 rng(56);
                             for (uint32 i = 0; i < 64 * 64; ++i) {
                                 b.VRAM2(0x20000 + i * 2, ((rng() & 0xF) << 12) | (rng() % 256));
                             }
                             b.Reg2(0x020, 0x0010);
                             b.Reg2(0x02A, 0x0000);
                             b.Reg2(0x038, 0x8000);
                             b.Reg2(0x03A, 0x0000);
                             for (uint32 r = 0x050; r <= 0x05E; r += 2) {
                                 b.Reg2(r, 0x1010);
                             }
                             b.Reg2(0x00E, 0x0300 | 3 | (2 << 2));
                             b.Reg2(0x0B0, 0x0000);
                             b.Reg2(0x0B2, 0x0000);
                             b.Reg2(0x0B4, 0x0000);
                             b.Reg2(0x0B6, 0x0000);
                             b.Reg2(0x0BC, (0x60000 >> 17) & 7);
                             b.Reg2(0x0BE, (0x60000 >> 1) & 0xFFFE);
                             b.Reg2(0x0FC, 0x0006);
                         }
                         const double ang = 0.3 + f * 0.2;
                         auto fx = [](double v) { return (uint32)(sint32)(v * 65536.0); };
                         auto w32 = [&](uint32 off, uint32 v) {
                             b.VRAM2(0x60000 + off, v >> 16);
                             b.VRAM2(0x60000 + off + 2, v & 0xFFFF);
                         };
                         w32(0x00, 0);
                         w32(0x04, 0);
                         w32(0x08, 0);
                         w32(0x0C, 0);
                         w32(0x10, fx(1));
                         w32(0x14, fx(1));
                         w32(0x18, 0);
                         w32(0x1C, fx(std::cos(ang)) & ~0x3F);
                         w32(0x20, fx(-std::sin(ang)) & ~0x3F);
                         w32(0x24, fx(0.25) & ~0x3F);
                         w32(0x28, fx(std::sin(ang)) & ~0x3F);
                         w32(0x2C, fx(std::cos(ang)) & ~0x3F);
                         w32(0x30, fx(0.5) & ~0x3F);
                         w32(0x34, (160u << 16) | 112u);
                         w32(0x38, 40u << 16);
                         w32(0x3C, (160u << 16) | 112u);
                         w32(0x40, 0);
                         w32(0x44, fx(f * 4.0) & ~0x3F);
                         w32(0x48, fx(f * 2.0) & ~0x3F);
                         w32(0x4C, fx(1.25));
                         w32(0x50, fx(0.75));
                         w32(0x54, 0);
                         w32(0x58, 0);
                         w32(0x5C, 0);
                     }});

    // Random VDP2 registers, VRAM and CRAM plus random VDP1 content. "fuzz" rewrites the registers every frame
    // (exercising register change timing); "steady" sets them once and compares frames after they settle.
    for (int steady = 0; steady < 2; ++steady) {
        for (int t = 0; t < (steady ? 40 : 12); ++t) {
            const uint32 seed = 1000 + t;
            auto vdp1 = MakeVDP1Scene(seed, true);
            Test test{(steady ? "steady" : "fuzz") + std::to_string(t),
                      [seed, vdp1, steady](Bus &b, int f) {
                          vdp1(b, f);
                          std::mt19937 rng(seed);
                          if (f == 0) {
                              for (uint32 a = 0; a < 0x80000; a += 2) {
                                  b.VRAM2(a, (uint16)rng());
                              }
                              for (uint32 a = 0; a < 0x1000; a += 2) {
                                  b.CRAM(a, (uint16)rng());
                              }
                          }
                          if (steady && f != 0) {
                              return;
                          }
                          std::mt19937 rng2(seed * 7 + (steady ? 0 : f));
                          for (uint32 off = 0x002; off < 0x120; off += 2) {
                              if (off == 0x004 || off == 0x006 || off == 0x008 || off == 0x00A) {
                                  continue;
                              }
                              uint16 v = (uint16)rng2();
                              if ((rng2() & 3) == 0) {
                                  v = 0;
                              }
                              b.Reg2(off, v);
                          }
                          b.Reg2(0x000, 0x8000 | (rng2() & 0x13));
                      },
                      steady ? 5 : 4};
            test.firstFrame = steady ? 3 : 1;
            tests.push_back(test);
        }
    }
    return tests;
}

int main(int argc, char **argv) {
    const char *only = argc > 1 ? argv[1] : nullptr;
    const char *scaleEnv = std::getenv("SCALE");
    const uint32 scale = scaleEnv ? std::atoi(scaleEnv) : 1;

    int ran = 0;
    for (Test &t : MakeTests()) {
        if (only && t.name.find(only) == std::string::npos) {
            continue;
        }
        ++ran;
        bool ok1 = false, ok2 = false;
        auto sw = Run(Kind::SW, t.scene, t.frames, t.enh, ok1);
        // The Vulkan renderer delivers frames one frame late
        auto vk = Run(Kind::VK, t.scene, t.frames + 1, t.enh, ok2);
        if (!ok2) {
            return 2;
        }
        if (scale > 1) {
            // Compare against the software renderer's output with each pixel replicated into a block
            for (Frame &f : sw) {
                Frame up;
                up.w = f.w * scale;
                up.h = f.h * scale;
                up.px.resize(up.w * up.h);
                for (uint32 y = 0; y < up.h; ++y) {
                    for (uint32 x = 0; x < up.w; ++x) {
                        up.px[y * up.w + x] = f.px[(y / scale) * f.w + x / scale];
                    }
                }
                f = std::move(up);
            }
        }
        Compare(t.name, sw, vk, t.firstFrame);
    }
    std::printf("%d/%d tests failed\n", g_failures, ran);
    return g_failures ? 1 : 0;
}
