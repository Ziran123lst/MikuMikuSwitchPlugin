#pragma once
#include "lib.hpp"
#include "Config.hpp"
#include "logger.hpp"
#include "ModLoader.hpp"
#include "overlays/Hints.hpp"

#include "nc/diva_nc.hpp"

#define FIX(addr) ((addr) - 0x100)

#define ADDR_MOD_LIMIT_1      FIX(0x000C4E50)
#define ADDR_MOD_LIMIT_2      FIX(0x000C8350)

#define ADDR_INLINE_MOD_LIMIT_1 FIX(0x000C4D2C)
#define ADDR_INLINE_MOD_LIMIT_2 FIX(0x000C6C90)
#define ADDR_INLINE_MOD_LIMIT_3 FIX(0x000C716C)
#define ADDR_INLINE_MOD_LIMIT_4 FIX(0x000C73C8)

#define ADDR_EFF_LIMIT            FIX(0x0017B710)
#define ADDR_LYRIC_LIMIT          FIX(0x004C3B18)

#define ADDR_COS_LIMIT_1 FIX(0x000C6F0C)
#define ADDR_COS_LIMIT_2 FIX(0x0076D9C4)
#define ADDR_COS_LIMIT_3 FIX(0x00519CA0)
#define ADDR_COS_LIMIT_3_CSEL_FIX FIX(0x00519CB0)

// Challenge time patches
#define ADDR_CHALLENGE_TIME_1     FIX(0x0018C224)
#define ADDR_CHALLENGE_TIME_2     FIX(0x0018C244)
#define ADDR_CHALLENGE_TIME_3     FIX(0x0018BCB8)

// Optional Gameplay Patches from IPSwitch
#define ADDR_WATERMARK_1          0x00AC9E1A
#define ADDR_WATERMARK_2          0x00AB7545

#define ADDR_HAND_SCALING         0x00185EC0

#define ADDR_LYRICS_DISABLE_1     0x00AD358C
#define ADDR_LYRICS_DISABLE_2     0x00AE87DF

#define ADDR_FORCE_JP_1           0x0021C964
#define ADDR_FORCE_JP_2           0x0021C974

#define ADDR_UNLOCK_PATCH         FIX(0x0CA400)

#define OFFSET_SATURATION_1       FIX(0x5D1B64)
#define OFFSET_SATURATION_2       FIX(0x5D29B0)

HOOK_DEFINE_TRAMPOLINE(TaskAdvLogo) {
    static int Callback(void* this_ptr) {
        if (Config::skipAdv) {
            return 1;
        }
        return Orig(this_ptr);
    }
};

HOOK_DEFINE_INLINE(ClearMovieSurfaceFrontInlineHook) {
    static void Callback(exl::hook::InlineCtx* ctx) {
        uintptr_t range_ptr = ctx->X[0];
        if (range_ptr == 0) return;

        char** str_start_ptr = reinterpret_cast<char**>(range_ptr);
        char** str_end_ptr   = str_start_ptr + 1;

        char* str_start = *str_start_ptr;
        char* str_end   = *str_end_ptr;

        if (str_start && str_end > str_start) {
            size_t len = str_end - str_start;
            if (len == 5 && std::memcmp(str_start, "FRONT", 5) == 0) {
                *str_end_ptr = str_start;
                str_start[0] = '\0';
            }
        }
    }
};

inline void ApplyCustomPatches() {

    // fix custom hairs with "ng=1" not appearing
    exl::patch::CodePatcher(0x3bbf70).Write<uint32_t>(0x52800008);

    // Challenge Time Handling (3 Modes: "enabled", "disabled", "default")
    if (Config::challengeTimeMode == "enabled") {
        exl::patch::CodePatcher(ADDR_CHALLENGE_TIME_1).Write<uint32_t>(0x320003FA);
        exl::patch::CodePatcher(ADDR_CHALLENGE_TIME_2).Write<uint32_t>(0x7100011F);
        exl::patch::CodePatcher(ADDR_CHALLENGE_TIME_3).Write<uint32_t>(0x7100011F);
    } else if (Config::challengeTimeMode == "disabled") {
        exl::patch::CodePatcher(ADDR_CHALLENGE_TIME_1).Write<uint32_t>(0x52800000);
        exl::patch::CodePatcher(ADDR_CHALLENGE_TIME_2).Write<uint32_t>(0x52800000);
        exl::patch::CodePatcher(ADDR_CHALLENGE_TIME_3).Write<uint32_t>(0x52800000);
    }

    // ExPatch (Unlock all difficulty levels)
    if (Config::ExPatch) {
        exl::patch::CodePatcher(ADDR_UNLOCK_PATCH).Write<uint32_t>(0x52800020);
    }

    // TaskAdvLogo mov 1 and ret but hook cuz very early timings
    TaskAdvLogo::InstallAtOffset(0x77ac0);

    // 1. Remove Copyright & PV Watermark
    if (Config::removeWatermarks) {
        exl::patch::CodePatcher(ADDR_WATERMARK_1).Write<uint8_t>(0x00);
        exl::patch::CodePatcher(ADDR_WATERMARK_2).Write<uint8_t>(0x00);
    }

    // 2. Disable Hand Scaling
    if (Config::disableHandScaling) {
        exl::patch::CodePatcher(ADDR_HAND_SCALING).Write<uint32_t>(0x14000258);
    }

    // 3. Disable Lyrics Display
    if (Config::disableLyrics) {
        exl::patch::CodePatcher(ADDR_LYRICS_DISABLE_1).Write<uint8_t>(0x00);
        exl::patch::CodePatcher(ADDR_LYRICS_DISABLE_2).Write<uint8_t>(0x00);
    }

    // 4. Force Japanese Language/Region
    if (Config::forceJapanese) {
        exl::patch::CodePatcher(ADDR_FORCE_JP_1).Write<uint32_t>(0x52800000);
        exl::patch::CodePatcher(ADDR_FORCE_JP_2).Write<uint32_t>(0x52800008);
    }

    // 5. FT UI leftovers
    if (Config::forceFtUI) {
        exl::patch::CodePatcher(0xAE7EA8).Write<uint64_t>(0x00347370646F6D2D);
        exl::patch::CodePatcher(0xACC6DF).Write<uint8_t>(0x00);
    }
    // 6. No movies
    if (Config::noMV) {
        exl::patch::CodePatcher(0x004CA9A0).Write<uint32_t>(0x1400006A);
        exl::patch::CodePatcher(0x004CAB54).Write<uint32_t>(0x14000081);
    }

    // 7. Movie_pv_type to not be FRONT for all vids
    if (Config::backMV) {
        ClearMovieSurfaceFrontInlineHook::InstallAtOffset(0x004CADF8);
    }

    // Core patches: Module limits
    exl::patch::CodePatcher(ADDR_MOD_LIMIT_1 + 8).Write<uint32_t>(0x2A0103E8);

    // Rewrite reader function to bypass module limit and protect against null dereference
    exl::patch::CodePatcher(ADDR_MOD_LIMIT_2 + 0x00).Write<uint32_t>(0xB4000060); // cbz x0, +0xC (jump if ptr is null)
    exl::patch::CodePatcher(ADDR_MOD_LIMIT_2 + 0x04).Write<uint32_t>(0xB9400000); // ldr w0, [x0] (read ID if valid)
    exl::patch::CodePatcher(ADDR_MOD_LIMIT_2 + 0x08).Write<uint32_t>(0xD65F03C0); // ret
    exl::patch::CodePatcher(ADDR_MOD_LIMIT_2 + 0x0C).Write<uint32_t>(0x2A1F03E0); // mov w0, wzr (return 0 if null)
    exl::patch::CodePatcher(ADDR_MOD_LIMIT_2 + 0x10).Write<uint32_t>(0xD65F03C0); // ret

    // Inlined module limit patches
    exl::patch::CodePatcher(ADDR_INLINE_MOD_LIMIT_1).Write<uint32_t>(0xD503201F);
    exl::patch::CodePatcher(ADDR_INLINE_MOD_LIMIT_2).Write<uint32_t>(0x2A0803E1);
    exl::patch::CodePatcher(ADDR_INLINE_MOD_LIMIT_3).Write<uint32_t>(0x2A1B03E9);
    exl::patch::CodePatcher(ADDR_INLINE_MOD_LIMIT_4).Write<uint32_t>(0x2A1A03E9);

    // Costume limit patches
    exl::patch::CodePatcher(ADDR_COS_LIMIT_2).Write<uint32_t>(0x2A0003F8);
    exl::patch::CodePatcher(ADDR_COS_LIMIT_3).Write<uint32_t>(0x2A1F03E9);
    exl::patch::CodePatcher(ADDR_COS_LIMIT_3_CSEL_FIX).Write<uint32_t>(0x2A1403F7);

    // Saturation song patch
    exl::patch::CodePatcher(OFFSET_SATURATION_1).Write<uint32_t>(0x710F951F);
    exl::patch::CodePatcher(OFFSET_SATURATION_2).Write<uint32_t>(0x710F903F);

    // Effect and open lyrics limits
    exl::patch::CodePatcher(ADDR_EFF_LIMIT).Write<uint32_t>(0x7103FC3F);
    exl::patch::CodePatcher(ADDR_LYRIC_LIMIT).Write<uint32_t>(0xF10FA27F);

    // =========================================================
    // TAA (Temporal Anti-Aliasing) restoration - PS4-style smooth
    // =========================================================
    // 1. NOP the per-frame TAA enable clear (bl 0x4A0440)
    exl::patch::CodePatcher(0x00481B40).Write<uint32_t>(0xD503201F);
    // 2. Force TAA quality getter to return enabled (mov w0,#1; ret)
    exl::patch::CodePatcher(0x0020C3D0).Write<uint32_t>(0x52800020);
    exl::patch::CodePatcher(0x0020C3D4).Write<uint32_t>(0xD65F03C0);
    // 3. Keep TAA enable flag: str w1,[x0,#0xC] -> str w8,[x0,#0xC] (w8=1)
    exl::patch::CodePatcher(0x004A044C).Write<uint32_t>(0xB9000808);
    // 4. Force temporal enable in resolve: mov w26,wzr -> mov w26,#1
    exl::patch::CodePatcher(0x004ABAC0).Write<uint32_t>(0x5280003A);
    // 5. Force jitter path: ldr w8,[x0,#0xC] -> mov w8,#1
    exl::patch::CodePatcher(0x004AE050).Write<uint32_t>(0x52800028);
    // 6. 8-phase centered jitter body (replaces ±0.5px 2-value toggle)
    //    phase 0-7 -> -0.5,-0.375,-0.25,-0.125,0,0.125,0.25,0.375 px
    exl::patch::CodePatcher(0x004AE05C).Write<uint32_t>(0x12000908); // and w8,w8,#7
    exl::patch::CodePatcher(0x004AE060).Write<uint32_t>(0x1E220101); // scvtf s1,w8
    exl::patch::CodePatcher(0x004AE064).Write<uint32_t>(0x1E281002); // fmov s2,#0.125
    exl::patch::CodePatcher(0x004AE068).Write<uint32_t>(0x1E220821); // fmul s1,s1,s2
    exl::patch::CodePatcher(0x004AE06C).Write<uint32_t>(0x1E2C1002); // fmov s2,#0.5
    exl::patch::CodePatcher(0x004AE070).Write<uint32_t>(0x1E223821); // fsub s1,s1,s2
    exl::patch::CodePatcher(0x004AE074).Write<uint32_t>(0x1E200820); // fmul s0,s1,s0
    // 7. Unfreeze phase counter: NOP cbz w23 (always update phase)
    exl::patch::CodePatcher(0x004AC3FC).Write<uint32_t>(0xD503201F);
    // 8. Phase always increments: csinc -> add w8,w8,#1 (no reset)
    exl::patch::CodePatcher(0x004AC408).Write<uint32_t>(0x11000508);

}
