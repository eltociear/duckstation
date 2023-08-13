// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "postprocessing_shader_fx.h"
#include "shadergen.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/image.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

// TODO: REMOVE ME
#include "core/settings.h"

#include "effect_codegen.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"

#include "fmt/format.h"

#include <cctype>
#include <cstring>
#include <sstream>

Log_SetChannel(PostProcessingShaderFX);

static constexpr s32 DEFAULT_BUFFER_WIDTH = 3840;
static constexpr s32 DEFAULT_BUFFER_HEIGHT = 2160;

static RenderAPI GetRenderAPI()
{
  return g_gpu_device ? g_gpu_device->GetRenderAPI() : RenderAPI::D3D11;
}

static std::unique_ptr<reshadefx::codegen> CreateRFXCodegen()
{
  const bool debug_info = g_gpu_device ? g_gpu_device->IsDebugDevice() : false;
  const bool uniforms_to_spec_constants = false;

  switch (GetRenderAPI())
  {
    case RenderAPI::None:
    case RenderAPI::D3D11:
    case RenderAPI::D3D12:
      return std::unique_ptr<reshadefx::codegen>(
        reshadefx::create_codegen_hlsl(50, debug_info, uniforms_to_spec_constants));

    case RenderAPI::Vulkan:
    case RenderAPI::Metal:
      return std::unique_ptr<reshadefx::codegen>(
        reshadefx::create_codegen_glsl(true, debug_info, uniforms_to_spec_constants));

    case RenderAPI::OpenGL:
    case RenderAPI::OpenGLES:
    default:
      return std::unique_ptr<reshadefx::codegen>(
        reshadefx::create_codegen_glsl(false, debug_info, uniforms_to_spec_constants));
  }
}

static s32 GetDefaultBufferWidth()
{
  return g_gpu_device ? g_gpu_device->GetWindowWidth() : DEFAULT_BUFFER_WIDTH;
}

static s32 GetDefaultBufferHeight()
{
  return g_gpu_device ? g_gpu_device->GetWindowHeight() : DEFAULT_BUFFER_HEIGHT;
}

static GPUTexture::Format MapTextureFormat(reshadefx::texture_format format)
{
  static constexpr GPUTexture::Format s_mapping[] = {
    GPUTexture::Format::Unknown, // unknown
    GPUTexture::Format::R8,      // r8
    GPUTexture::Format::R16,     // r16
    GPUTexture::Format::R16F,    // r16f
    GPUTexture::Format::R32I,    // r32i
    GPUTexture::Format::R32U,    // r32u
    GPUTexture::Format::R32F,    // r32f
    GPUTexture::Format::RG8,     // rg8
    GPUTexture::Format::RG16,    // rg16
    GPUTexture::Format::RG16F,   // rg16f
    GPUTexture::Format::RG32F,   // rg32f
    GPUTexture::Format::RGBA8,   // rgba8
    GPUTexture::Format::RGBA16,  // rgba16
    GPUTexture::Format::RGBA16F, // rgba16f
    GPUTexture::Format::RGBA32F, // rgba32f
    GPUTexture::Format::RGB10A2, // rgb10a2
  };
  DebugAssert(static_cast<u32>(format) < std::size(s_mapping));
  return s_mapping[static_cast<u32>(format)];
}

static GPUSampler::Config MapSampler(const reshadefx::sampler_info& si)
{
  GPUSampler::Config config = GPUSampler::GetNearestConfig();

  switch (si.filter)
  {
    case reshadefx::filter_mode::min_mag_mip_point:
      config.min_filter = GPUSampler::Filter::Nearest;
      config.mag_filter = GPUSampler::Filter::Nearest;
      config.mip_filter = GPUSampler::Filter::Nearest;
      break;

    case reshadefx::filter_mode::min_mag_point_mip_linear:
      config.min_filter = GPUSampler::Filter::Nearest;
      config.mag_filter = GPUSampler::Filter::Nearest;
      config.mip_filter = GPUSampler::Filter::Linear;
      break;

    case reshadefx::filter_mode::min_point_mag_linear_mip_point:
      config.min_filter = GPUSampler::Filter::Linear;
      config.mag_filter = GPUSampler::Filter::Linear;
      config.mip_filter = GPUSampler::Filter::Nearest;
      break;

    case reshadefx::filter_mode::min_point_mag_mip_linear:
      config.min_filter = GPUSampler::Filter::Nearest;
      config.mag_filter = GPUSampler::Filter::Linear;
      config.mip_filter = GPUSampler::Filter::Linear;
      break;

    case reshadefx::filter_mode::min_linear_mag_mip_point:
      config.min_filter = GPUSampler::Filter::Linear;
      config.mag_filter = GPUSampler::Filter::Nearest;
      config.mip_filter = GPUSampler::Filter::Nearest;
      break;

    case reshadefx::filter_mode::min_linear_mag_point_mip_linear:
      config.min_filter = GPUSampler::Filter::Linear;
      config.mag_filter = GPUSampler::Filter::Nearest;
      config.mip_filter = GPUSampler::Filter::Linear;
      break;

    case reshadefx::filter_mode::min_mag_linear_mip_point:
      config.min_filter = GPUSampler::Filter::Linear;
      config.mag_filter = GPUSampler::Filter::Linear;
      config.mip_filter = GPUSampler::Filter::Nearest;
      break;

    case reshadefx::filter_mode::min_mag_mip_linear:
      config.min_filter = GPUSampler::Filter::Linear;
      config.mag_filter = GPUSampler::Filter::Linear;
      config.mip_filter = GPUSampler::Filter::Linear;
      break;

    default:
      break;
  }

  static constexpr auto map_address_mode = [](const reshadefx::texture_address_mode m) {
    switch (m)
    {
      case reshadefx::texture_address_mode::wrap:
        return GPUSampler::AddressMode::Repeat;
      case reshadefx::texture_address_mode::mirror:
        Panic("Not implemented");
        return GPUSampler::AddressMode::Repeat;
      case reshadefx::texture_address_mode::clamp:
        return GPUSampler::AddressMode::ClampToEdge;
      case reshadefx::texture_address_mode::border:
      default:
        return GPUSampler::AddressMode::ClampToBorder;
    }
  };

  config.address_u = map_address_mode(si.address_u);
  config.address_v = map_address_mode(si.address_v);
  config.address_w = map_address_mode(si.address_w);

  return config;
}

static GPUPipeline::BlendState MapBlendState(const reshadefx::pass_info& pi)
{
  static constexpr auto map_blend_op = [](const reshadefx::pass_blend_op o) {
    switch (o)
    {
      case reshadefx::pass_blend_op::add:
        return GPUPipeline::BlendOp::Add;
      case reshadefx::pass_blend_op::subtract:
        return GPUPipeline::BlendOp::Subtract;
      case reshadefx::pass_blend_op::reverse_subtract:
        return GPUPipeline::BlendOp::ReverseSubtract;
      case reshadefx::pass_blend_op::min:
        return GPUPipeline::BlendOp::Min;
      case reshadefx::pass_blend_op::max:
      default:
        return GPUPipeline::BlendOp::Max;
    }
  };
  static constexpr auto map_blend_factor = [](const reshadefx::pass_blend_factor f) {
    switch (f)
    {
      case reshadefx::pass_blend_factor::zero:
        return GPUPipeline::BlendFunc::Zero;
      case reshadefx::pass_blend_factor::one:
        return GPUPipeline::BlendFunc::One;
      case reshadefx::pass_blend_factor::source_color:
        return GPUPipeline::BlendFunc::SrcColor;
      case reshadefx::pass_blend_factor::one_minus_source_color:
        return GPUPipeline::BlendFunc::InvSrcColor;
      case reshadefx::pass_blend_factor::dest_color:
        return GPUPipeline::BlendFunc::DstColor;
      case reshadefx::pass_blend_factor::one_minus_dest_color:
        return GPUPipeline::BlendFunc::InvDstColor;
      case reshadefx::pass_blend_factor::source_alpha:
        return GPUPipeline::BlendFunc::SrcAlpha;
      case reshadefx::pass_blend_factor::one_minus_source_alpha:
        return GPUPipeline::BlendFunc::InvSrcAlpha;
      case reshadefx::pass_blend_factor::dest_alpha:
      default:
        return GPUPipeline::BlendFunc::DstAlpha;
    }
  };

  GPUPipeline::BlendState bs = GPUPipeline::BlendState::GetNoBlendingState();
  bs.enable = (pi.blend_enable[0] != 0);
  bs.blend_op = map_blend_op(pi.blend_op[0]);
  bs.src_blend = map_blend_factor(pi.src_blend[0]);
  bs.dst_blend = map_blend_factor(pi.dest_blend[0]);
  bs.alpha_blend_op = map_blend_op(pi.blend_op_alpha[0]);
  bs.src_alpha_blend = map_blend_factor(pi.src_blend_alpha[0]);
  bs.dst_alpha_blend = map_blend_factor(pi.dest_blend_alpha[0]);
  bs.write_mask = pi.color_write_mask[0];
  return bs;
}

static GPUPipeline::Primitive MapPrimitive(reshadefx::primitive_topology topology)
{
  switch (topology)
  {
    case reshadefx::primitive_topology::point_list:
      return GPUPipeline::Primitive::Points;
    case reshadefx::primitive_topology::line_list:
      return GPUPipeline::Primitive::Lines;
    case reshadefx::primitive_topology::line_strip:
      Panic("Unhandled line strip");
      return GPUPipeline::Primitive::Lines;
    case reshadefx::primitive_topology::triangle_list:
      return GPUPipeline::Primitive::Triangles;
    case reshadefx::primitive_topology::triangle_strip:
    default:
      return GPUPipeline::Primitive::TriangleStrips;
  }
}

PostProcessingShaderFX::PostProcessingShaderFX() = default;

PostProcessingShaderFX::~PostProcessingShaderFX() = default;

bool PostProcessingShaderFX::LoadFromFile(std::string name, const char* filename)
{
  m_filename = filename;
  m_name = std::move(name);

  reshadefx::module temp_module;
  if (!CreateModule(GetDefaultBufferWidth(), GetDefaultBufferHeight(), &temp_module))
    return false;

  if (!CreateOptions(temp_module))
    return false;

  // Might go invalid when creating pipelines.
  m_valid = true;
  return true;
}

bool PostProcessingShaderFX::IsValid() const
{
  return m_valid;
}

bool PostProcessingShaderFX::CreateModule(s32 buffer_width, s32 buffer_height, reshadefx::module* mod)
{
  reshadefx::preprocessor pp;
  pp.add_include_path(std::filesystem::path(Path::GetDirectory(m_filename)));
  pp.add_macro_definition("__RESHADE__", "50901");
  pp.add_macro_definition("BUFFER_WIDTH", std::to_string(buffer_width)); // TODO: can we make these uniforms?
  pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(buffer_height));
  pp.add_macro_definition("BUFFER_RCP_WIDTH", fmt::format("({}.0 / BUFFER_WIDTH)", buffer_width));
  pp.add_macro_definition("BUFFER_RCP_HEIGHT", fmt::format("({}.0 / BUFFER_HEIGHT)", buffer_height));

  switch (GetRenderAPI())
  {
    case RenderAPI::D3D11:
    case RenderAPI::D3D12:
      pp.add_macro_definition("__RENDERER__", "0x0B000");
      break;

    case RenderAPI::OpenGL:
    case RenderAPI::OpenGLES:
    case RenderAPI::Vulkan:
    case RenderAPI::Metal:
      pp.add_macro_definition("__RENDERER__", "0x14300");
      break;

    default:
      UnreachableCode();
      break;
  }

  if (!pp.append_file(std::filesystem::path(m_filename)))
  {
    Log_ErrorPrintf("Failed to preprocess '%s':\n%s", m_filename.c_str(), pp.errors().c_str());
    return false;
  }

  std::unique_ptr<reshadefx::codegen> cg = CreateRFXCodegen();
  if (!cg)
    return false;

  reshadefx::parser parser;
  if (!parser.parse(pp.output(), cg.get()))
  {
    Log_ErrorPrintf("Failed to parse '%s':\n%s", m_filename.c_str(), parser.errors().c_str());
    return false;
  }

  cg->write_result(*mod);

  FileSystem::WriteBinaryFile("D:\\out.txt", mod->code.data(), mod->code.size());
  return true;
}

static std::string_view GetStringAnnotationValue(const std::vector<reshadefx::annotation>& annotations,
                                                 const std::string_view& annotation_name,
                                                 const std::string_view& default_value)
{
  for (const reshadefx::annotation& an : annotations)
  {
    if (an.name != annotation_name)
      continue;

    if (an.type.base != reshadefx::type::t_string)
      continue;

    return an.value.string_data;
  }

  return default_value;
}

static PostProcessingShader::Option::ValueVector
GetVectorAnnotationValue(const reshadefx::uniform_info& uniform, const std::string_view& annotation_name,
                         const PostProcessingShader::Option::ValueVector& default_value)
{
  PostProcessingShader::Option::ValueVector vv = default_value;
  for (const reshadefx::annotation& an : uniform.annotations)
  {
    if (an.name != annotation_name)
      continue;

    const u32 components = std::min<u32>(an.type.components(), PostProcessingShader::Option::MAX_VECTOR_COMPONENTS);

    if (an.type.base == uniform.type.base)
    {
      if (components > 0)
        std::memcpy(&vv[0].float_value, &an.value.as_float[0], sizeof(float) * components);

      break;
    }
    else if (an.type.base == reshadefx::type::t_string)
    {
      // Convert from string.
      if (uniform.type.base == reshadefx::type::t_float)
      {
        if (an.value.string_data == "BUFFER_WIDTH")
          vv[0].float_value = static_cast<float>(GetDefaultBufferWidth());
        else if (an.value.string_data == "BUFFER_HEIGHT")
          vv[0].float_value = static_cast<float>(GetDefaultBufferHeight());
        else
          vv[0].float_value = StringUtil::FromChars<float>(an.value.string_data).value_or(1000.0f);
      }
      else if (uniform.type.base == reshadefx::type::t_int)
      {
        if (an.value.string_data == "BUFFER_WIDTH")
          vv[0].int_value = static_cast<s32>(GetDefaultBufferWidth());
        else if (an.value.string_data == "BUFFER_HEIGHT")
          vv[0].int_value = static_cast<s32>(GetDefaultBufferHeight());
        else
          vv[0].int_value = StringUtil::FromChars<s32>(an.value.string_data).value_or(1000);
      }
      else
      {
        Log_ErrorPrint(fmt::format("Unhandled string value for '{}' (annotation type: {}, uniform type {})",
                                   uniform.name, an.type.description(), uniform.type.description())
                         .c_str());
      }

      break;
    }
    else if (an.type.base == reshadefx::type::t_int)
    {
      // Convert from int.
      if (uniform.type.base == reshadefx::type::t_float)
      {
        for (u32 i = 0; i < components; i++)
          vv[i].float_value = static_cast<float>(an.value.as_int[i]);
      }
      else if (uniform.type.base == reshadefx::type::t_bool)
      {
        for (u32 i = 0; i < components; i++)
          vv[i].int_value = (an.value.as_int[i] != 0) ? 1 : 0;
      }
    }
    else if (an.type.base == reshadefx::type::t_float)
    {
      // Convert from float.
      if (uniform.type.base == reshadefx::type::t_int)
      {
        for (u32 i = 0; i < components; i++)
          vv[i].int_value = static_cast<int>(an.value.as_float[i]);
      }
      else if (uniform.type.base == reshadefx::type::t_bool)
      {
        for (u32 i = 0; i < components; i++)
          vv[i].int_value = (an.value.as_float[i] != 0.0f) ? 1 : 0;
      }
    }

    break;
  }

  return vv;
}

bool PostProcessingShaderFX::CreateOptions(const reshadefx::module& mod)
{
  for (const reshadefx::uniform_info& ui : mod.uniforms)
  {
    SourceOption so;
    if (!GetSourceOption(ui, &so))
      return false;

    if (so != SourceOption::None)
    {
      Log_DevPrintf("Add source based option %u at offset %u (%s)", static_cast<u32>(so), ui.offset, ui.name.c_str());
      m_source_options.emplace_back(so, ui.offset);
      continue;
    }

    const std::string_view label = GetStringAnnotationValue(ui.annotations, "ui_label", ui.name);

    Option opt;
    switch (ui.type.base)
    {
      case reshadefx::type::t_float:
        opt.type = Option::Type::Float;
        break;

      case reshadefx::type::t_int:
      case reshadefx::type::t_uint:
        opt.type = Option::Type::Int;
        break;

      case reshadefx::type::t_bool:
        opt.type = Option::Type::Bool;
        break;

      default:
        Log_ErrorPrintf(fmt::format("Unhandled uniform type {} ({})", static_cast<u32>(ui.type.base), ui.name).c_str());
        return false;
    }

    opt.buffer_offset = ui.offset;
    opt.buffer_size = ui.size;
    opt.vector_size = ui.type.components();
    if (opt.vector_size == 0 || opt.vector_size > Option::MAX_VECTOR_COMPONENTS)
    {
      Log_ErrorPrintf(
        fmt::format("Unhandled vector size {} ({})", static_cast<u32>(ui.type.components()), ui.name).c_str());
      return false;
    }

    opt.min_value = GetVectorAnnotationValue(ui, "ui_min", {});
    opt.max_value = GetVectorAnnotationValue(ui, "ui_max", {});
    Option::ValueVector default_step = {};
    switch (opt.type)
    {
      case Option::Type::Float:
      {
        for (u32 i = 0; i < opt.vector_size; i++)
        {
          const float range = opt.max_value[i].float_value - opt.min_value[i].float_value;
          default_step[i].float_value = range / 100.0f;
        }
      }
      break;

      case Option::Type::Int:
      {
        for (u32 i = 0; i < opt.vector_size; i++)
        {
          const s32 range = opt.max_value[i].int_value - opt.min_value[i].int_value;
          default_step[i].int_value = std::max(range / 100, 1);
        }
      }
      break;

      default:
        break;
    }
    opt.step_value = GetVectorAnnotationValue(ui, "ui_step", default_step);

    if (ui.has_initializer_value)
    {
      std::memcpy(&opt.default_value[0].float_value, &ui.initializer_value.as_float[0],
                  sizeof(float) * opt.vector_size);
    }
    else
    {
      opt.default_value = {};
    }

    // Assume default if user doesn't set it.
    opt.value = opt.default_value;
    opt.name = ui.name;
    opt.ui_name = label.empty() ? ui.name : label;

    m_options.push_back(std::move(opt));
  }

  m_uniforms_size = mod.total_uniform_size;
  Log_DevPrintf("%s: %zu options", m_filename.c_str(), m_options.size());
  return true;
}

bool PostProcessingShaderFX::GetSourceOption(const reshadefx::uniform_info& ui,
                                             PostProcessingShaderFX::SourceOption* si)
{
  const std::string_view source = GetStringAnnotationValue(ui.annotations, "source", {});
  if (!source.empty())
  {
    if (source == "timer")
    {
      if (ui.type.base != reshadefx::type::t_float || ui.type.components() > 1)
      {
        Log_ErrorPrint(
          fmt::format("Unexpected type '{}' for timer source in uniform '{}'", ui.type.description(), ui.name).c_str());
        return false;
      }

      *si = SourceOption::Timer;
      return true;
    }
    else if (source == "framecount")
    {
      if ((!ui.type.is_integral() && !ui.type.is_floating_point()) || ui.type.components() > 1)
      {
        Log_ErrorPrint(
          fmt::format("Unexpected type '{}' for timer source in uniform '{}'", ui.type.description(), ui.name).c_str());
        return false;
      }

      *si = (ui.type.base == reshadefx::type::t_float) ? SourceOption::FrameCountF : SourceOption::FrameCount;
      return true;
    }
    else if (source == "overlay_active")
    {
      *si = SourceOption::Zero;
      return true;
    }
    else
    {
      Log_ErrorPrint(fmt::format("Unknown source '{}' in uniform '{}'", source, ui.name).c_str());
      return false;
    }
  }

  if (ui.has_initializer_value)
  {
    if (ui.initializer_value.string_data == "BUFFER_WIDTH")
    {
      *si = (ui.type.base == reshadefx::type::t_float) ? SourceOption::BufferWidthF : SourceOption::BufferWidth;
      return true;
    }
    else if (ui.initializer_value.string_data == "BUFFER_HEIGHT")
    {
      *si = (ui.type.base == reshadefx::type::t_float) ? SourceOption::BufferHeightF : SourceOption::BufferHeight;
      return true;
    }
  }

  *si = SourceOption::None;
  return true;
}

bool PostProcessingShaderFX::CreatePasses(GPUTexture::Format backbuffer_format, reshadefx::module& mod,
                                          reshadefx::technique_info& tech)
{
  if (tech.passes.empty())
  {
    Log_ErrorPrintf("Technique %s has no passes.", tech.name.c_str());
    return false;
  }

  m_passes.reserve(tech.passes.size());

  // Named render targets.
  for (const reshadefx::texture_info& ti : mod.textures)
  {
    Texture tex;

    if (!ti.semantic.empty())
    {
      Log_DevPrint(fmt::format("Ignoring semantic {} texture {}", ti.semantic, ti.unique_name).c_str());
      continue;
    }
    if (ti.render_target)
    {
      tex.rt_scale = 1.0f;
      tex.format = MapTextureFormat(ti.format);
      Log_DevPrint(
        fmt::format("Creating render target '{}' {}", ti.unique_name, GPUTexture::GetFormatName(tex.format)).c_str());
    }
    else
    {
      const std::string_view source = GetStringAnnotationValue(ti.annotations, "source", {});
      if (source.empty())
      {
        Log_ErrorPrint(fmt::format("Non-render target texture '{}' is missing source.", ti.unique_name).c_str());
        return false;
      }

      const std::string image_path =
        Path::Combine(EmuFolders::Shaders, Path::Combine("reshade" FS_OSPATH_SEPARATOR_STR "Textures", source));
      Common::RGBA8Image image;
      if (!image.LoadFromFile(image_path.c_str()))
      {
        Log_ErrorPrint(fmt::format("Failed to load image '{}' (from '{}')", source, image_path).c_str());
        return false;
      }

      tex.rt_scale = 0.0f;
      tex.texture = g_gpu_device->CreateTexture(image.GetWidth(), image.GetHeight(), 1, 1, 1, GPUTexture::Type::Texture,
                                                GPUTexture::Format::RGBA8, image.GetPixels(), image.GetPitch());
      if (!tex.texture)
      {
        Log_ErrorPrint(
          fmt::format("Failed to create {}x{} texture ({})", image.GetWidth(), image.GetHeight(), source).c_str());
        return false;
      }

      Log_DevPrint(fmt::format("Loaded {}x{} texture ({})", image.GetWidth(), image.GetHeight(), source).c_str());
    }

    tex.reshade_name = ti.unique_name;
    m_textures.push_back(std::move(tex));
  }

  TextureID last_output = INPUT_COLOR_TEXTURE;

  for (reshadefx::pass_info& pi : tech.passes)
  {
    const bool is_final = (&pi == &tech.passes.back());

    Pass pass;

    if (is_final)
    {
      pass.render_target = OUTPUT_COLOR_TEXTURE;
    }
    else if (!pi.render_target_names[0].empty())
    {
      pass.render_target = static_cast<TextureID>(m_textures.size());
      for (u32 i = 0; i < static_cast<u32>(m_textures.size()); i++)
      {
        if (m_textures[i].reshade_name == pi.render_target_names[0])
        {
          pass.render_target = static_cast<TextureID>(i);
          break;
        }
      }
      if (pass.render_target == static_cast<TextureID>(m_textures.size()))
      {
        Log_ErrorPrint(
          fmt::format("Unknown texture '{}' used as render target in pass '{}'", pi.render_target_names[0], pi.name)
            .c_str());
        return false;
      }
    }
    else
    {
      Texture new_rt;
      new_rt.rt_scale = 1.0f;
      new_rt.format = backbuffer_format;
      pass.render_target = static_cast<TextureID>(m_textures.size());
      m_textures.push_back(std::move(new_rt));
    }

    u32 texture_slot = 0;
    for (const reshadefx::sampler_info& si : pi.samplers)
    {
      Sampler sampler;
      sampler.slot = texture_slot++;
      sampler.reshade_name = si.unique_name;

      sampler.texture_id = static_cast<TextureID>(m_textures.size());
      for (const reshadefx::texture_info& ti : mod.textures)
      {
        if (ti.unique_name == si.texture_name)
        {
          // found the texture, now look for our side of it
          if (ti.semantic == "COLOR")
          {
            sampler.texture_id = INPUT_COLOR_TEXTURE;
            break;
          }
          else if (ti.semantic == "DEPTH")
          {
            sampler.texture_id = INPUT_DEPTH_TEXTURE;
            break;
          }
          else if (!ti.semantic.empty())
          {
            Log_ErrorPrint(fmt::format("Unknown semantic {} in texture {}", ti.semantic, ti.name).c_str());
            return false;
          }

          // must be a render target, or another texture
          for (u32 i = 0; i < static_cast<u32>(m_textures.size()); i++)
          {
            if (m_textures[i].reshade_name == si.texture_name)
            {
              // hook it up
              sampler.texture_id = static_cast<TextureID>(i);
              break;
            }
          }

          break;
        }
      }
      if (sampler.texture_id == static_cast<TextureID>(m_textures.size()))
      {
        Log_ErrorPrint(
          fmt::format("Unknown texture {} (sampler {}) in pass {}", si.texture_name, si.name, pi.name).c_str());
        return false;
      }

      Log_DevPrint(fmt::format("Pass {} Texture {} => {}", pi.name, si.texture_name, sampler.texture_id).c_str());

      // TODO FIXME
      sampler.sampler = g_gpu_device->CreateSampler(MapSampler(si));
      if (!sampler.sampler)
      {
        Log_ErrorPrintf("Failed to create sampler.");
        return false;
      }

      pass.samplers.push_back(std::move(sampler));
    }

#ifdef _DEBUG
    pass.name = std::move(pi.name);
#endif
    last_output = pass.render_target;
    m_passes.push_back(std::move(pass));
  }

  return true;
}

const char* PostProcessingShaderFX::GetTextureNameForID(TextureID id) const
{
  if (id == INPUT_COLOR_TEXTURE)
    return "Input Color Texture / Backbuffer";
  else if (id == INPUT_DEPTH_TEXTURE)
    return "Input Depth Texture";
  else if (id == OUTPUT_COLOR_TEXTURE)
    return "Output Color Texture";
  else if (id < 0 || static_cast<size_t>(id) >= m_textures.size())
    return "UNKNOWN";
  else
    return m_textures[static_cast<size_t>(id)].reshade_name.c_str();
}

GPUTexture* PostProcessingShaderFX::GetTextureByID(TextureID id, GPUTexture* input, GPUFramebuffer* final_target) const
{
  if (id < 0)
  {
    if (id == INPUT_COLOR_TEXTURE)
    {
      return input;
    }
    else if (id == INPUT_DEPTH_TEXTURE)
    {
      Panic("Not implemented");
      return nullptr;
    }
    else if (id == OUTPUT_COLOR_TEXTURE)
    {
      Panic("Wrong state for final target");
      return nullptr;
    }
    else
    {
      Panic("Unexpected reserved texture ID");
      return nullptr;
    }
  }

  if (static_cast<size_t>(id) >= m_textures.size())
    Panic("Unexpected texture ID");

  return m_textures[static_cast<size_t>(id)].texture.get();
}

GPUFramebuffer* PostProcessingShaderFX::GetFramebufferByID(TextureID id, GPUTexture* input,
                                                           GPUFramebuffer* final_target) const
{
  if (id < 0)
  {
    if (id == OUTPUT_COLOR_TEXTURE)
    {
      return final_target;
    }
    else
    {
      Panic("Unexpected reserved texture ID");
      return nullptr;
    }
  }

  if (static_cast<size_t>(id) >= m_textures.size())
    Panic("Unexpected texture ID");

  const Texture& tex = m_textures[static_cast<size_t>(id)];
  Assert(tex.framebuffer);
  return tex.framebuffer.get();
}

bool PostProcessingShaderFX::CompilePipeline(GPUTexture::Format format, u32 width, u32 height)
{
  const RenderAPI api = g_gpu_device->GetRenderAPI();
  const bool needs_main_defn = (api != RenderAPI::D3D11 && api != RenderAPI::D3D12);

  m_valid = false;
  m_textures.clear();
  m_passes.clear();

  reshadefx::module mod;
  if (!CreateModule(width, height, &mod))
    return false;

  if (mod.techniques.size() != 1)
  {
    Log_ErrorPrintf("Unexpected number of techniques: %zu in %s", mod.techniques.size(), m_name.c_str());
    return false;
  }

  reshadefx::technique_info& tech = mod.techniques.front();

  if (!CreatePasses(format, mod, tech))
    return false;

  const std::string_view code(mod.code.data(), mod.code.size());

  auto get_shader = [this, needs_main_defn, &code](const std::string& name, const std::vector<Sampler>& samplers,
                                                   GPUShaderStage stage) {
    std::string real_code;
    if (needs_main_defn)
      real_code = fmt::format("#version 460 core\n#define ENTRY_POINT_{}\n{}", name, code);
    else
      real_code = std::string(code);

    for (const Sampler& sampler : samplers)
    {
      std::string decl = fmt::format("__{}_t : register( t0);", sampler.reshade_name);
      std::string replacement =
        fmt::format("__{}_t : register({}t{});", sampler.reshade_name, (sampler.slot < 10) ? " " : "", sampler.slot);
      StringUtil::ReplaceAll(&real_code, decl, replacement);

      decl = fmt::format("__{}_s : register( s0);", sampler.reshade_name);
      replacement =
        fmt::format("__{}_s : register({}s{});", sampler.reshade_name, (sampler.slot < 10) ? " " : "", sampler.slot);
      StringUtil::ReplaceAll(&real_code, decl, replacement);
    }

    // FileSystem::WriteStringToFile("D:\\foo.txt", real_code);

    std::unique_ptr<GPUShader> sshader =
      g_gpu_device->CreateShader(stage, real_code, needs_main_defn ? "main" : name.c_str());
    if (!sshader)
      Log_ErrorPrintf("Failed to compile function '%s'", name.c_str());

    return sshader;
  };

  GPUPipeline::GraphicsConfig plconfig;
  plconfig.layout = GPUPipeline::Layout::MultiTextureAndUBO;
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.depth_format = GPUTexture::Format::Unknown;
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;

  DebugAssert(m_passes.size() == tech.passes.size());
  for (u32 i = 0; i < static_cast<u32>(m_passes.size()); i++)
  {
    const reshadefx::pass_info& info = tech.passes[i];
    Pass& pass = m_passes[i];

    auto vs = get_shader(info.vs_entry_point, pass.samplers, GPUShaderStage::Vertex);
    auto fs = get_shader(info.ps_entry_point, pass.samplers, GPUShaderStage::Fragment);
    if (!vs || !fs)
      return false;

    plconfig.color_format = (pass.render_target >= 0) ? m_textures[pass.render_target].format : format;
    plconfig.blend = MapBlendState(info);
    plconfig.primitive = MapPrimitive(info.topology);
    plconfig.vertex_shader = vs.get();
    plconfig.fragment_shader = fs.get();
    if (!plconfig.vertex_shader || !plconfig.fragment_shader)
      return false;

    pass.pipeline = g_gpu_device->CreatePipeline(plconfig);
    if (!pass.pipeline)
    {
      Log_ErrorPrintf("Failed to create pipeline for pass '%s'", info.name.c_str());
      return false;
    }
  }

  m_valid = true;
  return true;
}

bool PostProcessingShaderFX::ResizeOutput(GPUTexture::Format format, u32 width, u32 height)
{
  m_valid = false;

  for (Texture& tex : m_textures)
  {
    if (tex.rt_scale == 0.0f)
      continue;

    tex.framebuffer.reset();
    tex.texture.reset();

    const u32 t_width = std::max(static_cast<u32>(static_cast<float>(width) * tex.rt_scale), 1u);
    const u32 t_height = std::max(static_cast<u32>(static_cast<float>(height) * tex.rt_scale), 1u);
    tex.texture = g_gpu_device->CreateTexture(t_width, t_height, 1, 1, 1, GPUTexture::Type::RenderTarget, tex.format);
    if (!tex.texture)
    {
      Log_ErrorPrintf("Failed to create %ux%u texture", t_width, t_height);
      return {};
    }

    tex.framebuffer = g_gpu_device->CreateFramebuffer(tex.texture.get());
    if (!tex.framebuffer)
    {
      Log_ErrorPrintf("Failed to create %ux%u texture framebuffer", t_width, t_height);
      return {};
    }
  }

  m_valid = true;
  return true;
}

bool PostProcessingShaderFX::Apply(GPUTexture* input, GPUFramebuffer* final_target, s32 final_left, s32 final_top,
                                   s32 final_width, s32 final_height, s32 orig_width, s32 orig_height, u32 target_width,
                                   u32 target_height)
{
  GL_PUSH("PostProcessingShaderFX %s", m_name.c_str());

  m_frame_count++;

  // Reshade always draws at full size.
  g_gpu_device->SetViewportAndScissor(0, 0, target_width, target_height);

  if (m_uniforms_size > 0)
  {
    GL_SCOPE("Uniforms: %u bytes", m_uniforms_size);

    u8* uniforms = static_cast<u8*>(g_gpu_device->MapUniformBuffer(m_uniforms_size));
    for (const Option& opt : m_options)
    {
      DebugAssert((opt.buffer_offset + opt.buffer_size) <= m_uniforms_size);
      std::memcpy(uniforms + opt.buffer_offset, &opt.value[0].float_value, opt.buffer_size);
    }
    for (const auto& [so, offset] : m_source_options)
    {
      u8* dst = uniforms + offset;
      switch (so)
      {
        case SourceOption::Zero:
        {
          const u32 value = 0;
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOption::Timer:
        {
          const float value = static_cast<float>(m_timer.GetTimeSeconds());
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOption::FrameCount:
        {
          std::memcpy(dst, &m_frame_count, sizeof(m_frame_count));
        }
        break;

        case SourceOption::FrameCountF:
        {
          const float value = static_cast<float>(m_frame_count);
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOption::BufferWidth:
        case SourceOption::BufferHeight:
        {
          const s32 value = (so == SourceOption::BufferWidth) ? GetDefaultBufferWidth() : GetDefaultBufferHeight();
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        case SourceOption::BufferWidthF:
        case SourceOption::BufferHeightF:
        {
          const float value =
            static_cast<float>((so == SourceOption::BufferWidth) ? GetDefaultBufferWidth() : GetDefaultBufferHeight());
          std::memcpy(dst, &value, sizeof(value));
        }
        break;

        default:
          UnreachableCode();
          break;
      }
    }
    g_gpu_device->UnmapUniformBuffer(m_uniforms_size);
  }

  for (const Pass& pass : m_passes)
  {
    GL_SCOPE("Draw pass %s", pass.name.c_str());

    GL_INS("Render Target: ID %d [%s]", pass.render_target, GetTextureNameForID(pass.render_target));
    GPUFramebuffer* output_fb = GetFramebufferByID(pass.render_target, input, final_target);
    g_gpu_device->SetFramebuffer(output_fb);
    g_gpu_device->SetPipeline(pass.pipeline.get());

    // Set all inputs first, before the render pass starts.
    for (const Sampler& sampler : pass.samplers)
    {
      GL_INS("Texture Sampler %u: ID %d [%s]", sampler.slot, sampler.texture_id,
             GetTextureNameForID(sampler.texture_id));
      g_gpu_device->SetTextureSampler(sampler.slot, GetTextureByID(sampler.texture_id, input, final_target),
                                      sampler.sampler.get());
    }

    if (!output_fb)
    {
      // Drawing to final buffer.
      if (!g_gpu_device->BeginPresent(false))
        return false;
    }

    g_gpu_device->Draw(pass.num_vertices, 0);
  }

  return true;
}
