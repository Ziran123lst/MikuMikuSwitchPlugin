#include "MemoryHooks.hpp"
#include "lib.hpp"
#include "logger.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace MemoryHooks {

    using AllocFn        = void*  (*)(void* allocator, size_t size);
    using AllocAlignFn   = void*  (*)(void* allocator, size_t size, size_t align);
    using GetSizeOfFn    = size_t (*)(void* allocator, const void* ptr);
    using SetAllocModeFn = void   (*)(void* allocator, int mode);
    using SegaFreeFn     = void   (*)(void* ptr);

    static AllocFn        s_StdAllocFn        = nullptr;
    static AllocAlignFn   s_StdAllocAlignFn   = nullptr;
    static GetSizeOfFn    s_StdGetSizeOfFn    = nullptr;
    static SetAllocModeFn s_SetGpuAllocModeFn = nullptr;
    static SegaFreeFn     s_SegaFreeFn        = nullptr;

    inline uintptr_t ResolveSymbol(const char* sym) {
        for (int i = static_cast<int>(exl::util::ModuleIndex::Start);
             i < static_cast<int>(exl::util::ModuleIndex::End); i++) {
            auto index = static_cast<exl::util::ModuleIndex>(i);
            if (exl::util::HasModule(index)) {
                Elf_Sym* symbol = exl::reloc::GetSymbol(index, sym);
                if (symbol != nullptr) {
                    return exl::util::GetModuleInfo(index).m_Total.m_Start + symbol->st_value;
                }
            }
        }
        return 0;
    }

    inline void EnsureSdkAllocators() {
        if (!s_StdAllocFn) {
            s_StdAllocFn = reinterpret_cast<AllocFn>(
                ResolveSymbol("_ZN2nn3mem17StandardAllocator8AllocateEm")
            );
        }
        if (!s_StdAllocAlignFn) {
            s_StdAllocAlignFn = reinterpret_cast<AllocAlignFn>(
                ResolveSymbol("_ZN2nn3mem17StandardAllocator8AllocateEmm")
            );
        }
        if (!s_StdGetSizeOfFn) {
            s_StdGetSizeOfFn = reinterpret_cast<GetSizeOfFn>(
                ResolveSymbol("_ZNK2nn3mem17StandardAllocator9GetSizeOfEPKv")
            );
        }
        if (!s_SegaFreeFn) {
            uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
            s_SegaFreeFn = reinterpret_cast<SegaFreeFn>(base + 0x0004bdf0);
        }
    }

    // =========================================================================
    // FAST CALLER + PARENT CALLER TRACER (ARM64 FP WALK)
    // =========================================================================
    struct FrameRecord {
        FrameRecord* next;
        uintptr_t lr;
    };

    inline void GetCallers(uintptr_t& caller, uintptr_t& parentCaller) {
        uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
        caller = 0;
        parentCaller = 0;

        auto* fp = reinterpret_cast<FrameRecord*>(__builtin_frame_address(0));
        if (fp && ((reinterpret_cast<uintptr_t>(fp) & 7) == 0)) {
            if (fp->lr >= base) caller = fp->lr - base;
            if (fp->next && ((reinterpret_cast<uintptr_t>(fp->next) & 7) == 0)) {
                if (fp->next->lr >= base) parentCaller = fp->next->lr - base;
            }
        }
    }

    // =========================================================================
    // DETAILED MEMORY ANALYZER
    // =========================================================================
    static int s_SmallAllocSpamCounter = 0;

    inline void TraceMemoryOperation(const char* op, void* ptr, size_t size, uintptr_t caller, uintptr_t parent, const char* tag = nullptr) {
        if (size >= 1024 * 1024) {
            // WriteLog("\n[!!! FATAL ALLOC !!!] [%s] +%.2f MB (%zu bytes)\n"
            //          "  -> Ptr: %p\n"
            //          "  -> Direct Caller : main + 0x%lx\n"
            //          "  -> Parent Caller : main + 0x%lx\n"
            //          "  -> Tag/Source    : %s\n\n",
            //          op, (double)size / (1024.0 * 1024.0), size, ptr, caller, parent, tag ? tag : "N/A");
            return;
        }

        if (size >= 64 * 1024) {
            // WriteLog("[HEAVY ALLOC] [%s] +%.2f KB | Caller: +0x%lx | Parent: +0x%lx | Ptr: %p | %s\n",
            //          op, (double)size / 1024.0, caller, parent, ptr, tag ? tag : "");
            return;
        }

        if (size >= 1024) {
            // WriteLog("[MID ALLOC] [%s] +%.2f KB | Caller: +0x%lx | Ptr: %p | %s\n",
            //          op, (double)size / 1024.0, caller, ptr, tag ? tag : "");
            return;
        }
    }

    // =========================================================================
    // SAFE CROSS-POOL RESCUE ALLOCATOR
    // =========================================================================
    inline void* SafeRescueSegaPool(void* ptr, size_t size, size_t align = 8) {
        if (ptr != nullptr || size == 0) return ptr;

        EnsureSdkAllocators();
        uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
        void* poolLarge = reinterpret_cast<void*>(base + 0x4d0eb000); // 900 MB
        void* poolSmall = reinterpret_cast<void*>(base + 0x4d0eb048); // 320 MB

        void* fallbackPool = (size > 0x20000) ? poolSmall : poolLarge;

        if (align > 8 && s_StdAllocAlignFn) {
            ptr = s_StdAllocAlignFn(fallbackPool, size, align);
        } else if (s_StdAllocFn) {
            ptr = s_StdAllocFn(fallbackPool, size);
        }

        if (ptr != nullptr) {
            uintptr_t c, p;
            GetCallers(c, p);
        }

        return ptr;
    }

    // =========================================================================
    // 1. RESOURCE HEAP DISPATCH HOOK
    // =========================================================================
    HOOK_DEFINE_TRAMPOLINE(EngineHeapDispatchHook) {
        static void* Callback(uint32_t category, size_t size, const char* tag) {
            uintptr_t caller, parent;
            GetCallers(caller, parent);

            if (tag != nullptr) {
                if (strstr(tag, "spr_ps4_cmn.bin") ||
                    strstr(tag, "aet_ps4_cmn.bin") ||
                    strstr(tag, "spr_gam_loadsc.bin"))
                {
                    return nullptr;
                }
            }

            void* ptr = Orig(category, size, tag);
            if (ptr == nullptr && size > 0) {
                ptr = SafeRescueSegaPool(ptr, size, 8);
            }

            TraceMemoryOperation("EngineDispatch", ptr, size, caller, parent, tag);
            return ptr;
        }
    };

    // =========================================================================
    // 2. SEGA CORE ALLOCATORS (MALLOC, CALLOC, MEMALIGN, REALLOC, FREE)
    // =========================================================================

    // MALLOC (0x0004bd80)
    HOOK_DEFINE_TRAMPOLINE(SegaAllocRouterHook) {
        static void* Callback(size_t size) {
            uintptr_t caller, parent;
            GetCallers(caller, parent);

            void* ptr = Orig(size);
            if (!ptr && size > 0) {
                ptr = SafeRescueSegaPool(ptr, size, 8);
            }

            TraceMemoryOperation("SegaAlloc", ptr, size, caller, parent);
            return ptr;
        }
    };

    // VTABLE ALLOC (0x0004c650)
    HOOK_DEFINE_TRAMPOLINE(SegaVTableAllocHook) {
        static void* Callback(uintptr_t heap_interface, size_t size, size_t p2, uintptr_t p3) {
            uintptr_t caller, parent;
            GetCallers(caller, parent);

            void* ptr = Orig(heap_interface, size, p2, p3);
            if (!ptr && size > 0) {
                ptr = SafeRescueSegaPool(ptr, size, 8);
            }

            TraceMemoryOperation("VTableAlloc", ptr, size, caller, parent);
            return ptr;
        }
    };

    // MEMALIGN (0x0004c2f0)
    HOOK_DEFINE_TRAMPOLINE(SegaMemalignHook) {
        static void* Callback(size_t size, size_t alignment) {
            uintptr_t caller, parent;
            GetCallers(caller, parent);

            void* ptr = Orig(size, alignment);
            if (!ptr && size > 0) {
                ptr = SafeRescueSegaPool(ptr, size, alignment);
            }

            TraceMemoryOperation("Memalign", ptr, size, caller, parent);
            return ptr;
        }
    };

    // CALLOC (0x0004be40)
    HOOK_DEFINE_TRAMPOLINE(SegaCallocRouterHook) {
        static void* Callback(size_t count, size_t size) {
            uintptr_t caller, parent;
            GetCallers(caller, parent);

            size_t total = count * size;
            void* ptr = Orig(count, size);
            if (ptr == nullptr && total > 0) {
                ptr = SafeRescueSegaPool(ptr, total, 8);
                if (ptr) std::memset(ptr, 0, total);
            }

            TraceMemoryOperation("Calloc", ptr, total, caller, parent);
            return ptr;
        }
    };

    // REALLOC (0x0004bec0)
    HOOK_DEFINE_TRAMPOLINE(SegaReallocRouterHook) {
        static void* Callback(uintptr_t old_ptr, size_t new_size) {
            uintptr_t caller, parent;
            GetCallers(caller, parent);

            void* ptr = Orig(old_ptr, new_size);
            if (ptr == nullptr && new_size > 0) {
                ptr = SafeRescueSegaPool(ptr, new_size, 8);
                if (ptr && old_ptr) {
                    EnsureSdkAllocators();
                    uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
                    void* poolLarge = reinterpret_cast<void*>(base + 0x4d0eb000);
                    void* poolSmall = reinterpret_cast<void*>(base + 0x4d0eb048);

                    uintptr_t largeStart = base + 0x14ceb000;
                    uintptr_t largeEnd   = largeStart + 0x38400000;
                    void* srcPool = (old_ptr >= largeStart && old_ptr < largeEnd) ? poolLarge : poolSmall;

                    size_t copySize = new_size;
                    if (s_StdGetSizeOfFn) {
                        size_t oldSize = s_StdGetSizeOfFn(srcPool, reinterpret_cast<const void*>(old_ptr));
                        if (oldSize > 0) copySize = std::min(oldSize, new_size);
                    }

                    std::memcpy(ptr, reinterpret_cast<void*>(old_ptr), copySize);

                    if (s_SegaFreeFn) {
                        s_SegaFreeFn(reinterpret_cast<void*>(old_ptr));
                    }
                }
            }

            TraceMemoryOperation("Realloc", ptr, new_size, caller, parent);
            return ptr;
        }
    };

    // FREE (0x0004bdf0) - TRACK LARGE DEALLOCATIONS
    HOOK_DEFINE_TRAMPOLINE(SegaFreeHook) {
        static void Callback(void* ptr) {
            if (ptr != nullptr) {
                EnsureSdkAllocators();
                uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
                uintptr_t ptrVal = reinterpret_cast<uintptr_t>(ptr);

                uintptr_t largeStart = base + 0x14ceb000;
                uintptr_t largeEnd   = largeStart + 0x38400000;
                void* srcPool = (ptrVal >= largeStart && ptrVal < largeEnd)
                                    ? reinterpret_cast<void*>(base + 0x4d0eb000)
                                    : reinterpret_cast<void*>(base + 0x4d0eb048);

                if (s_StdGetSizeOfFn) {
                    size_t freedSize = s_StdGetSizeOfFn(srcPool, ptr);
                    if (freedSize >= 1024 * 1024) {
                        uintptr_t caller, parent;
                        GetCallers(caller, parent);
                    }
                }
            }
            Orig(ptr);
        }
    };

    // =========================================================================
    // 3. GPU ALLOCATOR & THUMBNAILS
    // =========================================================================
    HOOK_DEFINE_TRAMPOLINE(NumberLineSetModeHook) {
        static void Callback(void* this_ptr, int mode) {
            Orig(this_ptr, 1);
        }
    };

    HOOK_DEFINE_TRAMPOLINE(NumberLineInitHook) {
        static void Callback(void* this_ptr, void* p1, void* p2, void* p3, void* p4, bool p5) {
            Orig(this_ptr, p1, p2, p3, p4, p5);
            if (s_SetGpuAllocModeFn) {
                s_SetGpuAllocModeFn(this_ptr, 1);
            }
        }
    };

    constexpr uint32_t BASE_SPR_PV_TMB_ID = 4527;

    struct PvSpriteId {
        void* pv;
        uint32_t set;
        uint32_t bg[4];
        uint32_t jk[4];
        uint32_t logo[4];
        uint32_t tmb[4];
    };

    static uintptr_t TreeNext(uintptr_t node, uintptr_t endNode) {
        if (!node || node == endNode) return endNode;
        if (*reinterpret_cast<uintptr_t*>(node + 0x08) != 0) {
            node = *reinterpret_cast<uintptr_t*>(node + 0x08);
            while (*reinterpret_cast<uintptr_t*>(node + 0x00) != 0) {
                node = *reinterpret_cast<uintptr_t*>(node + 0x00);
            }
            return node;
        }
        while (true) {
            uintptr_t parent = *reinterpret_cast<uintptr_t*>(node + 0x10);
            if (!parent || parent == endNode) return endNode;
            if (*reinterpret_cast<uintptr_t*>(parent + 0x08) != node) {
                return parent;
            }
            node = parent;
        }
    }

    HOOK_DEFINE_TRAMPOLINE(BlockThumbnailLoadHook) {
        static void Callback(uint32_t setId) {
            if (setId == BASE_SPR_PV_TMB_ID) return;
            Orig(setId);
        }
    };

    HOOK_DEFINE_TRAMPOLINE(BlockThumbnailFinishHook) {
        static bool Callback(uint32_t setId) {
            if (setId == BASE_SPR_PV_TMB_ID) return false;
            return Orig(setId);
        }
    };

    HOOK_DEFINE_TRAMPOLINE(DisableSongThumbnailsHook) {
        static void Callback(uint64_t task) {
            Orig(task);
            uint64_t mapSize = *reinterpret_cast<uint64_t*>(task + 0x340);
            if (mapSize == 0 || mapSize > 10000) return;

            uintptr_t currNode = *reinterpret_cast<uintptr_t*>(task + 0x330);
            uintptr_t endNode  = task + 0x338;

            while (currNode != endNode && currNode != 0) {
                int pv_id = *reinterpret_cast<int*>(currNode + 0x20);
                if (pv_id > 0) {
                    auto* spriteData = reinterpret_cast<PvSpriteId*>(currNode + 0x28);
                    for (int i = 0; i < 4; i++) {
                        spriteData->tmb[i] = static_cast<uint32_t>(-1);
                    }
                }
                currNode = TreeNext(currNode, endNode);
            }
        }
    };

    // =========================================================================
    // INITIALIZATION
    // =========================================================================
    void Init() {
        EngineHeapDispatchHook::InstallAtOffset(0x0041cbc0);

        SegaAllocRouterHook::InstallAtOffset(0x0004bd80);
        SegaVTableAllocHook::InstallAtOffset(0x0004c650);
        SegaCallocRouterHook::InstallAtOffset(0x0004be40);
        SegaMemalignHook::InstallAtOffset(0x0004c2f0);
        SegaReallocRouterHook::InstallAtOffset(0x0004bec0);
        SegaFreeHook::InstallAtOffset(0x0004bdf0);

        uintptr_t setModeSym = ResolveSymbol("_ZN2nn3mem19NumberLineAllocator17SetAllocationModeENS1_14AllocationModeE");
        if (setModeSym) {
            s_SetGpuAllocModeFn = reinterpret_cast<SetAllocModeFn>(setModeSym);
            NumberLineSetModeHook::InstallAtFuncPtr(reinterpret_cast<decltype(&NumberLineSetModeHook::Callback)>(setModeSym));
        }

        uintptr_t initGpuSym = ResolveSymbol("_ZN2nn3mem19NumberLineAllocator10InitializeEPFPvmS2_ES2_PFvS2_S2_ES2_b");
        if (initGpuSym) {
            NumberLineInitHook::InstallAtFuncPtr(reinterpret_cast<decltype(&NumberLineInitHook::Callback)>(initGpuSym));
        }

        // BlockThumbnailLoadHook::InstallAtOffset(0x00611fa0);
        // BlockThumbnailFinishHook::InstallAtOffset(0x006120a0);
        // DisableSongThumbnailsHook::InstallAtOffset(0x005d26a0);
    }

} // namespace MemoryHooks
