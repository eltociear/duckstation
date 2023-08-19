// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu.h"
#include "texture_replacements.h"

#include "util/gpu_device.h"

#include "common/dimensional_array.h"
#include "common/heap_array.h"

#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class GPU_SW_Backend;
struct GPUBackendCommand;
struct GPUBackendDrawCommand;

class GPU_HW final : public GPU
{
public:
  enum class BatchRenderMode : u8
  {
    TransparencyDisabled,
    TransparentAndOpaque,
    OnlyOpaque,
    OnlyTransparent
  };

  enum class InterlacedRenderMode : u8
  {
    None,
    InterleavedFields,
    SeparateFields
  };

  GPU_HW();
  ~GPU_HW() override;

  const Threading::Thread* GetSWThread() const override;
  bool IsHardwareRenderer() const override;

  bool Initialize() override;
  void Reset(bool clear_vram) override;
  bool DoState(StateWrapper& sw, GPUTexture** host_texture, bool update_display) override;

  void RestoreGraphicsAPIState() override;

  void UpdateSettings() override;
  void UpdateResolutionScale() override final;
  std::tuple<u32, u32> GetEffectiveDisplayResolution(bool scaled = true) override final;
  std::tuple<u32, u32> GetFullDisplayResolution(bool scaled = true) override final;

private:
  enum : u32
  {
    VRAM_UPDATE_TEXTURE_BUFFER_SIZE = 4 * 1024 * 1024,
    MAX_BATCH_VERTEX_COUNTER_IDS = 65536 - 2,
    MAX_VERTICES_FOR_RECTANGLE = 6 * (((MAX_PRIMITIVE_WIDTH + (TEXTURE_PAGE_WIDTH - 1)) / TEXTURE_PAGE_WIDTH) + 1u) *
                                 (((MAX_PRIMITIVE_HEIGHT + (TEXTURE_PAGE_HEIGHT - 1)) / TEXTURE_PAGE_HEIGHT) + 1u),
    FASTMAD_BUFFER_COUNT = 4,
  };
  static_assert(VRAM_UPDATE_TEXTURE_BUFFER_SIZE >= VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16));

  struct BatchVertex
  {
    float x;
    float y;
    float z;
    float w;
    u32 color;
    u32 texpage;
    u16 u; // 16-bit texcoords are needed for 256 extent rectangles
    u16 v;
    u32 uv_limits;

    ALWAYS_INLINE void Set(float x_, float y_, float z_, float w_, u32 color_, u32 texpage_, u16 packed_texcoord,
                           u32 uv_limits_)
    {
      Set(x_, y_, z_, w_, color_, texpage_, packed_texcoord & 0xFF, (packed_texcoord >> 8), uv_limits_);
    }

    ALWAYS_INLINE void Set(float x_, float y_, float z_, float w_, u32 color_, u32 texpage_, u16 u_, u16 v_,
                           u32 uv_limits_)
    {
      x = x_;
      y = y_;
      z = z_;
      w = w_;
      color = color_;
      texpage = texpage_;
      u = u_;
      v = v_;
      uv_limits = uv_limits_;
    }

    ALWAYS_INLINE static u32 PackUVLimits(u32 min_u, u32 max_u, u32 min_v, u32 max_v)
    {
      return min_u | (min_v << 8) | (max_u << 16) | (max_v << 24);
    }

    ALWAYS_INLINE void SetUVLimits(u32 min_u, u32 max_u, u32 min_v, u32 max_v)
    {
      uv_limits = PackUVLimits(min_u, max_u, min_v, max_v);
    }
  };

  struct BatchConfig
  {
    GPUTextureMode texture_mode = GPUTextureMode::Disabled;
    GPUTransparencyMode transparency_mode = GPUTransparencyMode::Disabled;
    bool dithering = false;
    bool interlacing = false;
    bool set_mask_while_drawing = false;
    bool check_mask_before_draw = false;
    bool use_depth_buffer = false;

    // Returns the render mode for this batch.
    BatchRenderMode GetRenderMode() const
    {
      return transparency_mode == GPUTransparencyMode::Disabled ? BatchRenderMode::TransparencyDisabled :
                                                                  BatchRenderMode::TransparentAndOpaque;
    }
  };

  struct BatchUBOData
  {
    u32 u_texture_window_and[2];
    u32 u_texture_window_or[2];
    float u_src_alpha_factor;
    float u_dst_alpha_factor;
    u32 u_interlaced_displayed_field;
    u32 u_set_mask_while_drawing;
  };

  struct RendererStats
  {
    u32 num_batches;
    u32 num_vram_read_texture_updates;
    u32 num_uniform_buffer_updates;
  };

  bool CreateBuffers();
  void ClearFramebuffer();
  void DestroyBuffers();

  bool CompilePipelines();
  void DestroyPipelines();

  void UpdateVRAMReadTexture();
  void UpdateDepthBufferFromMaskBit();
  void ClearDepthBuffer();
  void SetScissor();
  void MapBatchVertexPointer(u32 required_vertices);
  void UnmapBatchVertexPointer(u32 used_vertices);
  void DrawBatchVertices(BatchRenderMode render_mode, u32 base_vertex, u32 num_vertices);
  void ClearDisplay() override;
  void UpdateDisplay() override;

  u32 CalculateResolutionScale() const;
  GPUDownsampleMode GetDownsampleMode(u32 resolution_scale) const;

  bool IsUsingMultisampling() const;
  bool IsUsingDownsampling() const;

  void SetFullVRAMDirtyRectangle();
  void ClearVRAMDirtyRectangle();
  void IncludeVRAMDirtyRectangle(const Common::Rectangle<u32>& rect);

  ALWAYS_INLINE bool IsFlushed() const { return m_batch_current_vertex_ptr == m_batch_start_vertex_ptr; }
  ALWAYS_INLINE u32 GetBatchVertexSpace() const
  {
    return static_cast<u32>(m_batch_end_vertex_ptr - m_batch_current_vertex_ptr);
  }
  ALWAYS_INLINE u32 GetBatchVertexCount() const
  {
    return static_cast<u32>(m_batch_current_vertex_ptr - m_batch_start_vertex_ptr);
  }
  void EnsureVertexBufferSpace(u32 required_vertices);
  void EnsureVertexBufferSpaceForCurrentCommand();
  void ResetBatchVertexDepth();

  /// Returns the value to be written to the depth buffer for the current operation for mask bit emulation.
  ALWAYS_INLINE float GetCurrentNormalizedVertexDepth() const
  {
    return 1.0f - (static_cast<float>(m_current_depth) / 65535.0f);
  }

  /// Returns the interlaced mode to use when scanning out/displaying.
  InterlacedRenderMode GetInterlacedRenderMode() const;

  /// We need two-pass rendering when using BG-FG blending and texturing, as the transparency can be enabled
  /// on a per-pixel basis, and the opaque pixels shouldn't be blended at all.
  ALWAYS_INLINE bool NeedsTwoPassRendering() const
  {
    // TODO: see if there's a better way we can do this. definitely can with fbfetch.
    return (m_batch.texture_mode != GPUTextureMode::Disabled &&
            (m_batch.transparency_mode == GPUTransparencyMode::BackgroundMinusForeground ||
             (!m_supports_dual_source_blend && m_batch.transparency_mode != GPUTransparencyMode::Disabled)));
  }

  void FillBackendCommandParameters(GPUBackendCommand* cmd) const;
  void FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const;
  void UpdateSoftwareRenderer(bool copy_vram_from_hw);

  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) override;
  void ReadVRAM(u32 x, u32 y, u32 width, u32 height) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) override;
  void DispatchRenderCommand() override;
  void FlushRender() override;
  void DrawRendererStats(bool is_idle_frame) override;

  bool BlitVRAMReplacementTexture(const TextureReplacementTexture* tex, u32 dst_x, u32 dst_y, u32 width, u32 height);

  /// Expands a line into two triangles.
  void DrawLine(float x0, float y0, u32 col0, float x1, float y1, u32 col1, float depth);

  /// Handles quads with flipped texture coordinate directions.
  static void HandleFlippedQuadTextureCoordinates(BatchVertex* vertices);

  /// Computes polygon U/V boundaries.
  static void ComputePolygonUVLimits(BatchVertex* vertices, u32 num_vertices);

  /// Sets the depth test flag for PGXP depth buffering.
  void SetBatchDepthBuffer(bool enabled);
  void CheckForDepthClear(const BatchVertex* vertices, u32 num_vertices);

  /// Returns the number of mipmap levels used for adaptive smoothing.
  u32 GetAdaptiveDownsamplingMipLevels() const;

  void DownsampleFramebuffer(GPUTexture* source, u32 left, u32 top, u32 width, u32 height);
  void DownsampleFramebufferAdaptive(GPUTexture* source, u32 left, u32 top, u32 width, u32 height);
  void DownsampleFramebufferBoxFilter(GPUTexture* source, u32 left, u32 top, u32 width, u32 height);

  std::unique_ptr<GPUTexture> m_vram_texture;
  std::unique_ptr<GPUTexture> m_vram_depth_texture;
  std::unique_ptr<GPUTexture> m_vram_depth_view;
  std::unique_ptr<GPUTexture> m_vram_read_texture;
  std::unique_ptr<GPUTexture> m_vram_readback_texture;
  std::unique_ptr<GPUTexture> m_vram_replacement_texture;
  std::unique_ptr<GPUTexture> m_display_texture;

  std::unique_ptr<GPUFramebuffer> m_vram_framebuffer;
  std::unique_ptr<GPUFramebuffer> m_vram_update_depth_framebuffer;
  std::unique_ptr<GPUFramebuffer> m_vram_readback_framebuffer;
  std::unique_ptr<GPUFramebuffer> m_display_framebuffer;

  std::unique_ptr<GPUTextureBuffer> m_vram_upload_buffer;
  std::unique_ptr<GPUTexture> m_vram_write_texture;

  FixedHeapArray<u16, VRAM_WIDTH * VRAM_HEIGHT> m_vram_shadow;

  std::unique_ptr<GPU_SW_Backend> m_sw_renderer;

  struct FastMADBuffer
  {
    std::unique_ptr<GPUTexture> texture;
    std::unique_ptr<GPUFramebuffer> framebuffer;
  };
  std::array<FastMADBuffer, FASTMAD_BUFFER_COUNT> m_fastmad_buffers;
  u32 m_current_fastmad_buffer = 0;

  BatchVertex* m_batch_start_vertex_ptr = nullptr;
  BatchVertex* m_batch_end_vertex_ptr = nullptr;
  BatchVertex* m_batch_current_vertex_ptr = nullptr;
  u32 m_batch_base_vertex = 0;
  s32 m_current_depth = 0;
  float m_last_depth_z = 1.0f;

  u32 m_resolution_scale = 1;
  u32 m_multisamples = 1;
  u32 m_max_resolution_scale = 1;
  bool m_true_color = true;

  union
  {
    BitField<u8, bool, 0, 1> m_supports_per_sample_shading;
    BitField<u8, bool, 1, 1> m_supports_dual_source_blend;
    BitField<u8, bool, 2, 1> m_supports_disable_color_perspective;
    BitField<u8, bool, 3, 1> m_per_sample_shading;
    BitField<u8, bool, 4, 1> m_scaled_dithering;
    BitField<u8, bool, 5, 1> m_chroma_smoothing;
    BitField<u8, bool, 6, 1> m_disable_color_perspective;
    BitField<u8, bool, 7, 1> m_use_fastmad;

    u8 bits = 0;
  };

  GPUTextureFilter m_texture_filtering = GPUTextureFilter::Nearest;
  GPUDownsampleMode m_downsample_mode = GPUDownsampleMode::Disabled;
  bool m_using_uv_limits = false;
  bool m_pgxp_depth_buffer = false;

  BatchConfig m_batch;
  BatchUBOData m_batch_ubo_data = {};

  // Bounding box of VRAM area that the GPU has drawn into.
  Common::Rectangle<u32> m_vram_dirty_rect;

  // Changed state
  bool m_batch_ubo_dirty = true;

  // [depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing]
  DimensionalArray<std::unique_ptr<GPUPipeline>, 2, 2, 5, 9, 4, 3> m_batch_pipelines{};

  // [wrapped][interlaced]
  DimensionalArray<std::unique_ptr<GPUPipeline>, 2, 2> m_vram_fill_pipelines{};

  // [depth_test]
  std::array<std::unique_ptr<GPUPipeline>, 2> m_vram_write_pipelines{};
  std::array<std::unique_ptr<GPUPipeline>, 2> m_vram_copy_pipelines{};

  std::unique_ptr<GPUPipeline> m_vram_readback_pipeline;
  std::unique_ptr<GPUPipeline> m_vram_update_depth_pipeline;

  // [depth_24][interlace_mode]
  DimensionalArray<std::unique_ptr<GPUPipeline>, 3, 2> m_display_pipelines{};

  std::unique_ptr<GPUPipeline> m_interleaved_field_extract_pipeline{};
  std::unique_ptr<GPUPipeline> m_fastmad_reconstruct_pipeline{};

  // TODO: get rid of this, and use image blits instead where supported
  std::unique_ptr<GPUPipeline> m_copy_pipeline;

  std::unique_ptr<GPUTexture> m_downsample_texture;
  std::unique_ptr<GPUTexture> m_downsample_render_texture;
  std::unique_ptr<GPUFramebuffer> m_downsample_framebuffer;
  std::unique_ptr<GPUTexture> m_downsample_weight_texture;
  std::unique_ptr<GPUFramebuffer> m_downsample_weight_framebuffer;
  std::unique_ptr<GPUPipeline> m_downsample_first_pass_pipeline;
  std::unique_ptr<GPUPipeline> m_downsample_mid_pass_pipeline;
  std::unique_ptr<GPUPipeline> m_downsample_blur_pass_pipeline;
  std::unique_ptr<GPUPipeline> m_downsample_composite_pass_pipeline;
  std::unique_ptr<GPUSampler> m_downsample_lod_sampler;
  std::unique_ptr<GPUSampler> m_downsample_composite_sampler;

  // Statistics
  RendererStats m_renderer_stats = {};
  RendererStats m_last_renderer_stats = {};

private:
  void LoadVertices();

  ALWAYS_INLINE void AddVertex(const BatchVertex& v)
  {
    std::memcpy(m_batch_current_vertex_ptr, &v, sizeof(BatchVertex));
    m_batch_current_vertex_ptr++;
  }

  template<typename... Args>
  ALWAYS_INLINE void AddNewVertex(Args&&... args)
  {
    m_batch_current_vertex_ptr->Set(std::forward<Args>(args)...);
    m_batch_current_vertex_ptr++;
  }

  void PrintSettingsToLog();
};
