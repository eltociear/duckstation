// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_types.h"
#include "util/gpu_texture.h"

#include <array>
#include <cstring>
#include <memory>

class GPUTextureCache
{
public:
  static constexpr u32 VRAM_PAGE_WIDTH = 64;
  static constexpr u32 VRAM_PAGE_HEIGHT = 256;
  static constexpr u32 VRAM_PAGES_WIDE = VRAM_WIDTH / VRAM_PAGE_WIDTH;
  static constexpr u32 VRAM_PAGES_HIGH = VRAM_HEIGHT / VRAM_PAGE_HEIGHT;
  static constexpr u32 NUM_PAGES = VRAM_PAGES_WIDE * VRAM_PAGES_HIGH;

  /// 4 pages in C16 mode, 2+4 pages in P8 mode, 1+1 pages in P4 mode.
  static constexpr u32 MAX_PAGE_REFS_PER_SOURCE = 6;

  static constexpr u32 PageIndex(u32 px, u32 py) { return ((py * VRAM_PAGES_WIDE) + px); }

  static constexpr u32 VRAMCoordinateToPage(u32 x, u32 y)
  {
    return PageIndex(x / VRAM_PAGE_WIDTH, y / VRAM_PAGE_HEIGHT);
  }

  struct SourceKey
  {
    u8 page;
    GPUTextureMode mode;
    GPUTexturePaletteReg palette;

    SourceKey() = default;
    ALWAYS_INLINE constexpr SourceKey(u8 page_, GPUTexturePaletteReg palette_, GPUTextureMode mode_)
      : page(page_), palette(palette_), mode(mode_)
    {
    }
    ALWAYS_INLINE constexpr SourceKey(const SourceKey& k) : page(k.page), mode(k.mode), palette(k.palette) {}

    ALWAYS_INLINE SourceKey& operator=(const SourceKey& k)
    {
      page = k.page;
      mode = k.mode;
      palette.bits = k.palette.bits;
      return *this;
    }

    ALWAYS_INLINE bool operator==(const SourceKey& k) const {
      return (std::memcmp(&k, this, sizeof(SourceKey)) == 0);
    }
    ALWAYS_INLINE bool operator!=(const SourceKey& k) const { return (std::memcmp(&k, this, sizeof(SourceKey)) != 0); }
  };
  static_assert(sizeof(SourceKey) == 4);

  struct Source;
  struct SourceList;
  struct SourceListNode;

  struct SourceList
  {
    SourceListNode* head;
    SourceListNode* tail;
  };

  struct SourceListNode
  {
    // why inside itself? because we have 3 lists
    Source* ref;
    SourceList* list;
    SourceListNode* prev;
    SourceListNode* next;
  };

  struct Source
  {
    SourceKey key;
    u32 num_page_refs;
    std::unique_ptr<GPUTexture> texture;

    std::array<SourceListNode, MAX_PAGE_REFS_PER_SOURCE> page_refs;
  };

public:
  GPUTextureCache();
  ~GPUTextureCache();

  const Source* LookupSource(SourceKey key);

  void Clear();
  void InvalidatePages(u32 x, u32 y, u32 width, u32 height);
  void InvalidatePage(u32 pn);

private:
  template<typename F>
  void LoopPages(u32 x, u32 y, u32 width, u32 height, const F& f);

  const Source* CreateSource(SourceKey key);

  std::array<SourceList, NUM_PAGES> m_page_sources = {};
};
