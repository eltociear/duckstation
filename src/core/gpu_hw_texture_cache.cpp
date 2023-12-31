// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "gpu_hw_texture_cache.h"
#include "gpu.h"

#include "util/gpu_device.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/small_string.h"

Log_SetChannel(GPUTextureCache);

ALWAYS_INLINE static void ListAppend(GPUTextureCache::SourceList* list, GPUTextureCache::Source* item,
                                     GPUTextureCache::SourceListNode* item_node)
{
  item_node->ref = item;
  item_node->list = list;
  item_node->next = nullptr;
  if (list->tail)
  {
    item_node->prev = list->tail;
    list->tail->next = item_node;
    list->tail = item_node;
  }
  else
  {
    item_node->prev = nullptr;
    list->head = item_node;
    list->tail = item_node;
  }
}

ALWAYS_INLINE static void ListUnlink(const GPUTextureCache::SourceListNode& node)
{
  if (node.prev)
    node.prev->next = node.next;
  else
    node.list->head = node.next;
  if (node.next)
    node.next->prev = node.prev;
  else
    node.list->tail = node.prev;
}

template<typename F>
ALWAYS_INLINE static void ListIterate(const GPUTextureCache::SourceList& list, const F& f)
{
  for (const GPUTextureCache::Source::ListNode* n = list.head; n; n = n->next)
    f(n->ref);
}

ALWAYS_INLINE static TinyString SourceToString(const GPUTextureCache::Source* src)
{
  static constexpr const std::array<const char*, 4> texture_modes = {
    {"Palette4Bit", "Palette8Bit", "Direct16Bit", "Reserved_Direct16Bit"}};

  TinyString ret;
  if (src->key.mode < GPUTextureMode::Direct16Bit)
  {
    ret.format("{} Page[{}] CLUT@[{},{}]", texture_modes[static_cast<u8>(src->key.mode)], src->key.page,
               src->key.palette.GetXBase(), src->key.palette.GetYBase());
  }
  else
  {
    ret.format("{} Page[{}]", texture_modes[static_cast<u8>(src->key.mode)], src->key.page);
  }
  return ret;
}

ALWAYS_INLINE static u32 PageStartX(u32 pn)
{
  return (pn % GPUTextureCache::VRAM_PAGES_WIDE) * GPUTextureCache::VRAM_PAGE_WIDTH;
}

ALWAYS_INLINE static u32 PageStartY(u32 pn)
{
  return (pn / GPUTextureCache::VRAM_PAGES_WIDE) * GPUTextureCache::VRAM_PAGE_HEIGHT;
}

ALWAYS_INLINE_RELEASE static const u16* VRAMPagePointer(u32 pn)
{
  const u32 start_y = PageStartY(pn);
  const u32 start_x = PageStartX(pn);
  return &g_vram[start_y * VRAM_WIDTH + start_x];
}

// TODO: Vectorize these.
ALWAYS_INLINE_RELEASE static void DecodeTexture4(const u16* page, const u16* palette, u32* dest, u32 dest_stride)
{
  for (u32 y = 0; y < TEXTURE_PAGE_HEIGHT; y++)
  {
    const u16* page_ptr = page;
    u32* dest_ptr = dest;

    for (u32 x = 0; x < TEXTURE_PAGE_WIDTH / 4; x++)
    {
      const u32 pp = *(page_ptr++);
      *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[pp & 0x0F]);
      *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[(pp >> 4) & 0x0F]);
      *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[(pp >> 8) & 0x0F]);
      *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[pp >> 12]);
    }

    page += VRAM_WIDTH;
    dest = reinterpret_cast<u32*>(reinterpret_cast<u8*>(dest) + dest_stride);
  }
}
ALWAYS_INLINE_RELEASE static void DecodeTexture8(const u16* page, const u16* palette, u32* dest, u32 dest_stride)
{
  for (u32 y = 0; y < TEXTURE_PAGE_HEIGHT; y++)
  {
    const u16* page_ptr = page;
    u32* dest_ptr = dest;

    for (u32 x = 0; x < TEXTURE_PAGE_WIDTH / 2; x++)
    {
      const u32 pp = *(page_ptr++);
      *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[pp & 0xFF]);
      *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(palette[pp >> 8]);
    }

    page += VRAM_WIDTH;
    dest = reinterpret_cast<u32*>(reinterpret_cast<u8*>(dest) + dest_stride);
  }
}
ALWAYS_INLINE_RELEASE static void DecodeTexture16(const u16* page, u32* dest, u32 dest_stride)
{
  for (u32 y = 0; y < TEXTURE_PAGE_HEIGHT; y++)
  {
    const u16* page_ptr = page;
    u32* dest_ptr = dest;

    for (u32 x = 0; x < TEXTURE_PAGE_WIDTH; x++)
      *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(*(page_ptr++));

    page += VRAM_WIDTH;
    dest = reinterpret_cast<u32*>(reinterpret_cast<u8*>(dest) + dest_stride);
  }
}

ALWAYS_INLINE_RELEASE static void DecodeTexture(u8 page, GPUTexturePaletteReg palette, GPUTextureMode mode, u32* dest,
                                                u32 dest_stride)
{
  const u16* page_ptr = VRAMPagePointer(page);
  switch (mode)
  {
    case GPUTextureMode::Palette4Bit:
      DecodeTexture4(page_ptr, &g_vram[palette.GetYBase() * VRAM_WIDTH + palette.GetXBase()], dest, dest_stride);
      break;
    case GPUTextureMode::Palette8Bit:
      DecodeTexture8(page_ptr, &g_vram[palette.GetYBase() * VRAM_WIDTH + palette.GetXBase()], dest, dest_stride);
      break;
    case GPUTextureMode::Direct16Bit:
    case GPUTextureMode::Reserved_Direct16Bit:
      DecodeTexture16(page_ptr, dest, dest_stride);
      break;

      DefaultCaseIsUnreachable()
  }
}

static void DecodeTexture(u8 page, GPUTexturePaletteReg palette, GPUTextureMode mode, GPUTexture* texture)
{
  alignas(16) static u32 s_temp_buffer[TEXTURE_PAGE_WIDTH * TEXTURE_PAGE_HEIGHT];

  u32* tex_map;
  u32 tex_stride;
  const bool mapped =
    texture->Map(reinterpret_cast<void**>(&tex_map), &tex_stride, 0, 0, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT);
  if (!mapped)
  {
    tex_map = s_temp_buffer;
    tex_stride = sizeof(u32) * TEXTURE_PAGE_WIDTH;
  }

  DecodeTexture(page, palette, mode, tex_map, tex_stride);

  if (mapped)
    texture->Unmap();
  else
    texture->Update(0, 0, TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT, tex_map, tex_stride);
}

ALWAYS_INLINE u32 WidthForMode(GPUTextureMode mode)
{
  return TEXTURE_PAGE_WIDTH >> ((mode < GPUTextureMode::Direct16Bit) ? (2 - static_cast<u8>(mode)) : 0);
}

GPUTextureCache::GPUTextureCache()
{
}

GPUTextureCache::~GPUTextureCache()
{
  Clear();
}

const GPUTextureCache::Source* GPUTextureCache::LookupSource(SourceKey key)
{
  for (const SourceListNode* n = m_page_sources[key.page].head; n; n = n->next)
  {
    // TODO: Move to front
    if (n->ref->key == key)
      return n->ref;
  }

  return CreateSource(key);
}

template<typename F>
void GPUTextureCache::LoopPages(u32 x, u32 y, u32 width, u32 height, const F& f)
{
  DebugAssert(width > 0 && height > 0);
  DebugAssert((x + width) <= VRAM_WIDTH && (y + height) <= VRAM_HEIGHT);

  const u32 start_x = x / VRAM_PAGE_WIDTH;
  const u32 start_y = y / VRAM_PAGE_HEIGHT;
  const u32 end_x = (x + (width - 1)) / VRAM_PAGE_WIDTH;
  const u32 end_y = (y + (height - 1)) / VRAM_PAGE_HEIGHT;

  u32 page_number = PageIndex(start_x, start_y);
  for (u32 page_y = start_y; page_y <= end_y; page_y++)
  {
    u32 y_page_number = page_number;

    for (u32 page_x = start_x; page_x <= end_x; page_x++)
    {
      f(page_number);
      y_page_number++;
    }

    page_number += VRAM_PAGES_WIDE;
  }
}

void GPUTextureCache::Clear()
{
  for (u32 i = 0; i < NUM_PAGES; i++)
    InvalidatePage(i);

    // should all be null
#ifdef _DEBUG
  for (u32 i = 0; i < NUM_PAGES; i++)
    DebugAssert(!m_page_sources[i].head && !m_page_sources[i].tail);
#endif
}

void GPUTextureCache::InvalidatePage(u32 pn)
{
  DebugAssert(pn < NUM_PAGES);

  SourceList& ps = m_page_sources[pn];
  for (SourceListNode* n = ps.head; n;)
  {
    Source* src = n->ref;
    n = n->next;

    if (Log::GetLogLevel() <= LOGLEVEL_DEV)
      Log_DevFmt("Invalidate source {}", SourceToString(src));

    for (u32 i = 0; i < src->num_page_refs; i++)
      ListUnlink(src->page_refs[i]);

    // TODO: return to hash cache
    g_gpu_device->RecycleTexture(std::move(src->texture));
    delete src;
  }

  ps.head = nullptr;
  ps.tail = nullptr;
}

void GPUTextureCache::InvalidatePages(u32 x, u32 y, u32 width, u32 height)
{
  LoopPages(x, y, width, height, [this](u32 page) { InvalidatePage(page); });
}

const GPUTextureCache::Source* GPUTextureCache::CreateSource(SourceKey key)
{
  // TODO: Hash cache
  std::unique_ptr<GPUTexture> tex = g_gpu_device->FetchTexture(TEXTURE_PAGE_WIDTH, TEXTURE_PAGE_HEIGHT, 1, 1, 1,
                                                               GPUTexture::Type::Texture, GPUTexture::Format::RGBA8);
  if (!tex)
  {
    Log_ErrorPrint("Failed to create texture.");
    return nullptr;
  }

  DecodeTexture(key.page, key.palette, key.mode, tex.get());

  Source* src = new Source();
  src->key = key;
  src->texture = std::move(tex);
  src->num_page_refs = 0;

  std::array<u32, MAX_PAGE_REFS_PER_SOURCE> page_refns;
  const auto add_page_ref = [this, src, &page_refns](u32 pn) {
    // Don't double up references
    for (u32 i = 0; i < src->num_page_refs; i++)
    {
      if (page_refns[i] == pn)
        return;
    }

    const u32 ri = src->num_page_refs++;
    page_refns[ri] = pn;

    // TODO: Textures should be at the front, CLUTs at the back. Moving to front will take care of that.
    ListAppend(&m_page_sources[pn], src, &src->page_refs[ri]);
  };

  LoopPages(PageStartX(key.page), PageStartY(key.page), WidthForMode(key.mode), TEXTURE_PAGE_HEIGHT, add_page_ref);

  if (key.mode < GPUTextureMode::Direct16Bit)
  {
    LoopPages(key.palette.GetXBase(), key.palette.GetYBase(), GPUTexturePaletteReg::GetWidth(key.mode), 1,
              add_page_ref);
  }

  if (Log::GetLogLevel() <= LOGLEVEL_DEV)
    Log_DevFmt("Appended new source {} to {} pages", SourceToString(src), src->num_page_refs);

  return src;
}
