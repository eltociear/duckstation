// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "postprocessing_shader.h"

#include "common/timer.h"

// reshadefx
#include "effect_module.hpp"

class PostProcessingShaderFX final : public PostProcessingShader
{
public:
  PostProcessingShaderFX();
  ~PostProcessingShaderFX();

  bool IsValid() const override;

  bool LoadFromFile(std::string name, const char* filename);

  bool ResizeOutput(GPUTexture::Format format, u32 width, u32 height) override;
  bool CompilePipeline(GPUTexture::Format format, u32 width, u32 height) override;
  bool Apply(GPUTexture* input, GPUFramebuffer* final_target, s32 final_left, s32 final_top, s32 final_width,
             s32 final_height, s32 orig_width, s32 orig_height, u32 target_width, u32 target_height) override;

private:
  using TextureID = s32;

  static constexpr TextureID INPUT_COLOR_TEXTURE = -1;
  static constexpr TextureID INPUT_DEPTH_TEXTURE = -2;
  static constexpr TextureID OUTPUT_COLOR_TEXTURE = -3;

  enum class SourceOption
  {
    None,
    Zero,
    Timer,
    FrameCount,
    FrameCountF,
    BufferWidth,
    BufferHeight,
    BufferWidthF,
    BufferHeightF,

    MaxCount
  };

  bool CreateModule(s32 buffer_width, s32 buffer_height, reshadefx::module* mod);
  bool CreateOptions(const reshadefx::module& mod);
  bool GetSourceOption(const reshadefx::uniform_info& ui, SourceOption* si);
  bool CreatePasses(GPUTexture::Format backbuffer_format, reshadefx::module& mod, reshadefx::technique_info& tech);

  const char* GetTextureNameForID(TextureID id) const;
  GPUTexture* GetTextureByID(TextureID id, GPUTexture* input, GPUFramebuffer* final_target) const;
  GPUFramebuffer* GetFramebufferByID(TextureID id, GPUTexture* input, GPUFramebuffer* final_target) const;

  std::string m_filename;

  struct Texture
  {
    std::unique_ptr<GPUTexture> texture;
    std::unique_ptr<GPUFramebuffer> framebuffer;
    std::string reshade_name; // TODO: we might be able to drop this
    GPUTexture::Format format;
    float rt_scale;
  };

  struct Sampler
  {
    u32 slot;
    TextureID texture_id;
    std::string reshade_name;
    std::unique_ptr<GPUSampler> sampler;
  };

  struct Pass
  {
    std::unique_ptr<GPUPipeline> pipeline;
    TextureID render_target;
    std::vector<Sampler> samplers;
    u32 num_vertices;

#ifdef _DEBUG
    std::string name;
#endif
  };

  std::vector<Pass> m_passes;
  std::vector<Texture> m_textures;
  std::vector<std::pair<SourceOption, u32>> m_source_options;
  u32 m_uniforms_size = 0;
  bool m_valid = false;

  Common::Timer m_timer;
  u32 m_frame_count = 0;
};
