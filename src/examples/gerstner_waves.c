#include "example_base.h"
#include "examples.h"
#include "meshes.h"

#include <string.h>

/* -------------------------------------------------------------------------- *
 * WebGPU Example - Gerstner Waves
 *
 * This example is a WebGPU implementation of the Gerstner Waves algorithm.
 *
 * Ref:
 * https://github.com/artemhlezin/webgpu-gerstner-waves
 * https://en.wikipedia.org/wiki/Trochoidal_wave
 * https://www.reddit.com/r/webgpu/comments/s2elkb/webgpu_gerstner_waves_implementation
 * -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- *
 * Camera control
 * -------------------------------------------------------------------------- */

// Controls
static struct {
  bool is_mouse_dragging;
  vec2 prev_mouse_position;
  vec2 mouse_drag_distance;
  vec2 current_mouse_position;
} controls = {
  .is_mouse_dragging      = false,
  .prev_mouse_position    = {50.0f, -25.0f},
  .mouse_drag_distance    = GLM_VEC2_ZERO_INIT,
  .current_mouse_position = {50.0f, -25.0f},
};

static void update_controls(wgpu_example_context_t* context)
{
  if (!controls.is_mouse_dragging && context->mouse_buttons.left) {
    glm_vec2_copy(context->mouse_position, controls.prev_mouse_position);
    controls.is_mouse_dragging = true;
  }
  else if (controls.is_mouse_dragging && context->mouse_buttons.left) {
    glm_vec2_sub(context->mouse_position, controls.prev_mouse_position,
                 controls.mouse_drag_distance);
    glm_vec2_sub(controls.current_mouse_position, controls.mouse_drag_distance,
                 controls.current_mouse_position);
    glm_vec2_copy(context->mouse_position, controls.prev_mouse_position);
  }
  else if (controls.is_mouse_dragging && !context->mouse_buttons.left) {
    controls.is_mouse_dragging = false;
  }

  // Limit x and y
  int x = (int)controls.current_mouse_position[0];
  int y = (int)controls.current_mouse_position[1];

  controls.current_mouse_position[0] = x % 360;
  controls.current_mouse_position[1] = MAX(-90, MIN(-10, y));
}

/* -------------------------------------------------------------------------- *
 * Matrix utility functions
 * -------------------------------------------------------------------------- */

static void create_orbit_view_matrix(float radius, versor rotation, mat4* dest)
{
  // inv(R*T)
  mat4 view_matrix = GLM_MAT4_ZERO_INIT;
  glm_quat_mat4(rotation, view_matrix);
  glm_translate(view_matrix, (vec3){0.0f, 0.0f, radius});
  glm_mat4_inv(view_matrix, *dest);
}

static void position_from_view_matrix(mat4 view_matrix, vec3* dest)
{
  mat4 inv_view = GLM_MAT4_ZERO_INIT;
  glm_mat4_inv(view_matrix, inv_view);
  glm_vec3_copy((vec3){inv_view[3][0], inv_view[3][1], inv_view[3][2]}, *dest);
}

/**
 * @brief Creates a quaternion from the given euler angle x, y, z.
 *
 * @param {quat} out the receiving quaternion
 * @param {x} Angle to rotate around X axis in degrees.
 * @param {y} Angle to rotate around Y axis in degrees.
 * @param {z} Angle to rotate around Z axis in degrees.
 * @returns {quat} out
 * @function
 * @ref https://glmatrix.net/docs/module-quat.html
 * @see https://glmatrix.net/docs/quat.js.html#line459
 */
static void from_euler(float x, float y, float z, versor* dest)
{
  float halfToRad = (0.5f * PI) / 180.0f;

  x *= halfToRad;
  y *= halfToRad;
  z *= halfToRad;

  float sx = sin(x);
  float cx = cos(x);
  float sy = sin(y);
  float cy = cos(y);
  float sz = sin(z);
  float cz = cos(z);

  (*dest)[0] = sx * cy * cz - cx * sy * sz;
  (*dest)[1] = cx * sy * cz + sx * cy * sz;
  (*dest)[2] = cx * cy * sz - sx * sy * cz;
  (*dest)[3] = cx * cy * cz + sx * sy * sz;
}

/* -------------------------------------------------------------------------- *
 * Gerstner Waves example
 * -------------------------------------------------------------------------- */

// Plane mesh
static plane_mesh_t plane_mesh = {0};

// Vertex buffer
static struct {
  WGPUBuffer buffer;
  uint64_t buffer_size;
  uint32_t count;
} vertices = {0};

// Index buffer
static struct {
  WGPUBuffer buffer;
  uint64_t buffer_size;
  uint32_t count;
} indices = {0};

// Uniform buffers
static struct {
  WGPUBuffer scene;
  WGPUBuffer gerstner_wave_params;
} uniform_buffers;

// Uniform buffer data
static float start_time = 0;
static struct {
  float elapsed_time;
  float padding[3];
  mat4 model_matrix;
  mat4 view_projection_matrix;
  vec3 view_position;
} scene_data = {0};

static struct {
  mat4 view_matrix;
  versor rotation;
  mat4 projection_matrix;
} tmp_mtx = {
  .view_matrix       = GLM_MAT4_ZERO_INIT,
  .rotation          = GLM_VEC4_ZERO_INIT,
  .projection_matrix = GLM_MAT4_ZERO_INIT,
};

// Gerstner Waves parameters
static struct {
  // Uniform storage requires that array elements be aligned to 16 bytes.
  // 4 bytes waveLength + 4 bytes amplitude + 4+4 bytes steepness
  // + 8+8 bytes direction = 32 Bytes
  struct {
    float wave_length; // 0 < L
    float amplitude;   // 0 < A
    float steepness;   // Steepness of the peak of the wave. 0 <= S <= 1
    float padding1;
    vec2 direction; // Normalized direction of the wave
    vec2 padding2;
  } waves[5];
  float amplitude_sum; // Sum of waves amplitudes
} gerstner_wave_params = {
  .waves = {
    {
      .wave_length = 8.0f, // f32 - 4 bytes
      .amplitude   = 0.1f, // f32 - 4 bytes
      .steepness   = 1.0f, // f32 - 4 bytes, but 8 bytes will be reserved to match 32 bytes stride
      .direction   = {1.0f, 1.3f}, // vec2<f32> - 8 bytes but 16 bytes will be reserved
    },
    {
      .wave_length = 4.0f,
      .amplitude   = 0.1f,
      .steepness   = 0.8f,
      .direction   ={-0.7f, 0.0f},
    },
    {
      .wave_length = 5.0f,
      .amplitude   = 0.2f,
      .steepness   = 1.0f,
      .direction   = {0.3f, 0.2f},
    },
    {
      .wave_length = 10.f,
      .amplitude   = 0.5f,
      .steepness   = 1.0f,
      .direction   = {4.3f, 1.2f},
    },
    {
      .wave_length = 3.0f,
      .amplitude   = 0.1f,
      .steepness   = 1.0f,
      .direction   = {0.5f, 0.5f},
    },
  },
};
static bool gerstner_waves_normalized = false;

// Texture and sampler for sea color image
static texture_t sea_color_texture;
static WGPUSampler non_filtering_sampler;

static struct {
  WGPUBindGroupLayout uniforms;
  WGPUBindGroupLayout textures;
} bind_group_layouts;

static struct {
  WGPUBindGroup uniforms;
  WGPUBindGroup textures;
} bind_groups;

static WGPUPipelineLayout pipeline_layout;
static WGPURenderPipeline pipeline;

static const uint32_t sample_count = 4;
static struct {
  WGPURenderPassColorAttachment color_attachments[1];
  WGPURenderPassDescriptor descriptor;
  // Multi-sampled texture
  struct {
    WGPUTexture texture;
    WGPUTextureView view;
    uint32_t sample_count;
  } multisampled_framebuffer;
} render_pass = {
  .multisampled_framebuffer.sample_count = sample_count,
};

// Other variables
static const char* example_title = "Gerstner Waves";
static bool prepared             = false;

static void prepare_example(wgpu_example_context_t* context)
{
  start_time = context->run_time;
}

// Prepare the plane mesh
static void prepare_plane_mesh()
{
  plane_mesh_init(&plane_mesh, &(plane_mesh_init_options_t){
                                 .width   = 12.0f,
                                 .height  = 12.0f,
                                 .rows    = 100,
                                 .columns = 100,
                               });
}

// Prepare vertex and index buffers for an indexed plane mesh
static void prepare_vertex_and_index_buffers(wgpu_context_t* wgpu_context)
{
  // Create vertex buffer
  vertices.count       = plane_mesh.vertex_count;
  vertices.buffer_size = vertices.count * sizeof(plane_vertex_t);
  vertices.buffer      = wgpu_create_buffer_from_data(
    wgpu_context, plane_mesh.vertices, vertices.buffer_size,
    WGPUBufferUsage_Vertex);

  // Create index buffer
  indices.count       = plane_mesh.index_count;
  indices.buffer_size = indices.count * sizeof(uint32_t);
  indices.buffer
    = wgpu_create_buffer_from_data(wgpu_context, plane_mesh.indices,
                                   indices.buffer_size, WGPUBufferUsage_Index);
}

static void prepare_texture(wgpu_context_t* wgpu_context)
{
  const char* file  = "textures/sea-color.jpg";
  sea_color_texture = wgpu_create_texture_from_file(wgpu_context, file, NULL);

  // Create non-filtering sampler
  WGPUSamplerDescriptor sampler_desc = {
    .addressModeU  = WGPUAddressMode_ClampToEdge,
    .addressModeV  = WGPUAddressMode_ClampToEdge,
    .addressModeW  = WGPUAddressMode_ClampToEdge,
    .maxAnisotropy = 1,
  };
  non_filtering_sampler
    = wgpuDeviceCreateSampler(wgpu_context->device, &sampler_desc);
  ASSERT(non_filtering_sampler != NULL)
}

static void setup_pipeline_layout(wgpu_context_t* wgpu_context)
{
  // Bind group layout for Gerstner Waves mesh rendering & parameters
  {
    WGPUBindGroupLayoutEntry bgl_entries[2] = {
      [0] = (WGPUBindGroupLayoutEntry) {
        // Binding 0: Uniforms
        .binding = 0,
        .visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
        .buffer = (WGPUBufferBindingLayout) {
          .type = WGPUBufferBindingType_Uniform,
          .hasDynamicOffset = false,
          .minBindingSize = sizeof(scene_data),
        },
        .sampler = {0},
      },
      [1] = (WGPUBindGroupLayoutEntry) {
        // Binding 1: GerstnerWavesUniforms
        .binding = 1,
        .visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
        .buffer = (WGPUBufferBindingLayout) {
          .type = WGPUBufferBindingType_Uniform,
          .hasDynamicOffset = false,
          .minBindingSize = sizeof(gerstner_wave_params),
        },
        .sampler = {0},
      },
    };

    // Create the bind group layout
    bind_group_layouts.uniforms = wgpuDeviceCreateBindGroupLayout(
      wgpu_context->device, &(WGPUBindGroupLayoutDescriptor){
                              .entryCount = (uint32_t)ARRAY_SIZE(bgl_entries),
                              .entries    = bgl_entries,
                            });
    ASSERT(bind_group_layouts.uniforms != NULL)
  }

  // Bind group layout for sea color texture
  {
    WGPUBindGroupLayoutEntry bgl_entries[2] = {
      [0] = (WGPUBindGroupLayoutEntry) {
        // Binding 0: Sampler
        .binding = 0,
        .visibility = WGPUShaderStage_Fragment,
        .sampler = (WGPUSamplerBindingLayout){
          .type=WGPUSamplerBindingType_NonFiltering,
        },
        .texture = {0},
      },
      [1] = (WGPUBindGroupLayoutEntry) {
        // Binding 1: Texture view
        .binding = 1,
        .visibility = WGPUShaderStage_Fragment,
        .texture = (WGPUTextureBindingLayout) {
          .sampleType = WGPUTextureSampleType_Float,
          .viewDimension = WGPUTextureViewDimension_2D,
          .multisampled = false,
        },
        .storageTexture = {0},
      }
    };
    bind_group_layouts.textures = wgpuDeviceCreateBindGroupLayout(
      wgpu_context->device, &(WGPUBindGroupLayoutDescriptor){
                              .entryCount = (uint32_t)ARRAY_SIZE(bgl_entries),
                              .entries    = bgl_entries,
                            });
    ASSERT(bind_group_layouts.textures != NULL)
  }

  // Create the pipeline layout from bind group layouts
  WGPUBindGroupLayout bind_groups_layout_array[2] = {
    bind_group_layouts.uniforms, // Group 0
    bind_group_layouts.textures  // Group 1
  };
  pipeline_layout = wgpuDeviceCreatePipelineLayout(
    wgpu_context->device,
    &(WGPUPipelineLayoutDescriptor){
      .bindGroupLayoutCount = (uint32_t)ARRAY_SIZE(bind_groups_layout_array),
      .bindGroupLayouts     = bind_groups_layout_array,
    });
  ASSERT(pipeline_layout != NULL)
}

static void setup_bind_groups(wgpu_context_t* wgpu_context)
{
  // Bind group for Gerstner Waves mesh rendering & parameters
  {
    WGPUBindGroupEntry bg_entries[2] = {
      [0] = (WGPUBindGroupEntry) {
        // Binding 0: Uniforms
        .binding = 0,
        .buffer = uniform_buffers.scene,
        .offset = 0,
        .size = sizeof(scene_data),
      },
      [1] = (WGPUBindGroupEntry) {
        // Binding 1: GerstnerWavesUniforms
        .binding = 1,
        .buffer = uniform_buffers.gerstner_wave_params,
        .offset = 0,
        .size = sizeof(gerstner_wave_params),
      },
    };

    bind_groups.uniforms = wgpuDeviceCreateBindGroup(
      wgpu_context->device, &(WGPUBindGroupDescriptor){
                              .layout     = bind_group_layouts.uniforms,
                              .entryCount = (uint32_t)ARRAY_SIZE(bg_entries),
                              .entries    = bg_entries,
                            });
    ASSERT(bind_groups.uniforms != NULL)
  }

  // Bind group layout for sea color texture
  {
    WGPUBindGroupEntry bg_entries[2] = {
      [0] = (WGPUBindGroupEntry) {
         // Binding 0: Sampler
        .binding = 0,
        .sampler = non_filtering_sampler,
      },
      [1] = (WGPUBindGroupEntry) {
        // Binding 1: Texture view
        .binding = 1,
        .textureView = sea_color_texture.view,
      }
    };
    WGPUBindGroupDescriptor bg_desc = {
      .layout     = bind_group_layouts.textures,
      .entryCount = (uint32_t)ARRAY_SIZE(bg_entries),
      .entries    = bg_entries,
    };
    bind_groups.textures
      = wgpuDeviceCreateBindGroup(wgpu_context->device, &bg_desc);
    ASSERT(bind_groups.textures != NULL)
  }
}

static void prepare_pipelines(wgpu_context_t* wgpu_context)
{
  // Primitive state
  WGPUPrimitiveState primitive_state_desc = {
    .topology  = WGPUPrimitiveTopology_TriangleList,
    .frontFace = WGPUFrontFace_CCW,
    .cullMode  = WGPUCullMode_None,
  };

  // Color target state
  WGPUBlendState blend_state                   = wgpu_create_blend_state(false);
  WGPUColorTargetState color_target_state_desc = (WGPUColorTargetState){
    .format    = wgpu_context->swap_chain.format,
    .blend     = &blend_state,
    .writeMask = WGPUColorWriteMask_All,
  };

  // Depth stencil state
  // Enable depth testing so that the fragment closest to the camera is rendered
  // in front.
  WGPUDepthStencilState depth_stencil_state_desc
    = wgpu_create_depth_stencil_state(&(create_depth_stencil_state_desc_t){
      .format              = WGPUTextureFormat_Depth32Float,
      .depth_write_enabled = true,
    });
  depth_stencil_state_desc.depthCompare = WGPUCompareFunction_Less;

  // Vertex buffer layout
  WGPU_VERTEX_BUFFER_LAYOUT(
    plane, sizeof(plane_vertex_t),
    // Attribute location 0: Position
    WGPU_VERTATTR_DESC(0, WGPUVertexFormat_Float32x3,
                       offsetof(plane_vertex_t, position)),
    // Attribute location 1: Normal
    WGPU_VERTATTR_DESC(1, WGPUVertexFormat_Float32x3,
                       offsetof(plane_vertex_t, normal)),
    // Attribute location 2: UV
    WGPU_VERTATTR_DESC(2, WGPUVertexFormat_Float32x2,
                       offsetof(plane_vertex_t, uv)))

  // Vertex state
  WGPUVertexState vertex_state_desc = wgpu_create_vertex_state(
             wgpu_context, &(wgpu_vertex_state_t){
             .shader_desc = (wgpu_shader_desc_t){
                // Vertex shader WGSL
                .file  = "shaders/gerstner_waves/gerstner-waves.wgsl",
                .entry = "vertex_main",
             },
             .buffer_count = 1,
             .buffers = &plane_vertex_buffer_layout,
           });

  // Fragment state
  WGPUFragmentState fragment_state_desc = wgpu_create_fragment_state(
             wgpu_context, &(wgpu_fragment_state_t){
             .shader_desc = (wgpu_shader_desc_t){
                // Fragment shader WGSL
                .file  = "shaders/gerstner_waves/gerstner-waves.wgsl",
                .entry = "fragment_main",
             },
             .target_count = 1,
             .targets = &color_target_state_desc,
           });

  // Multisample state
  WGPUMultisampleState multisample_state_desc
    = wgpu_create_multisample_state_descriptor(
      &(create_multisample_state_desc_t){
        .sample_count = sample_count,
      });

  // Create rendering pipeline using the specified states
  pipeline = wgpuDeviceCreateRenderPipeline(
    wgpu_context->device, &(WGPURenderPipelineDescriptor){
                            .label        = "gerstner_waves_render_pipeline",
                            .layout       = pipeline_layout,
                            .primitive    = primitive_state_desc,
                            .vertex       = vertex_state_desc,
                            .fragment     = &fragment_state_desc,
                            .depthStencil = &depth_stencil_state_desc,
                            .multisample  = multisample_state_desc,
                          });

  // Partial cleanup
  WGPU_RELEASE_RESOURCE(ShaderModule, vertex_state_desc.module);
  WGPU_RELEASE_RESOURCE(ShaderModule, fragment_state_desc.module);
}

static void setup_render_pass(wgpu_context_t* wgpu_context)
{
  UNUSED_VAR(wgpu_context);

  // Color attachment
  render_pass.color_attachments[0] = (WGPURenderPassColorAttachment) {
      .view          = NULL,
      .resolveTarget = NULL,
      .loadOp        = WGPULoadOp_Clear,
      .storeOp       = WGPUStoreOp_Store,
      .clearColor = (WGPUColor) {
        .r = 0.3f,
        .g = 0.3f,
        .b = 0.3f,
        .a = 1.0f,
      },
  };

  // Depth attachment
  wgpu_setup_deph_stencil(wgpu_context,
                          &(struct deph_stencil_texture_creation_options_t){
                            .format       = WGPUTextureFormat_Depth32Float,
                            .sample_count = sample_count,
                          });

  // Render pass descriptor
  render_pass.descriptor = (WGPURenderPassDescriptor){
    .colorAttachmentCount   = 1,
    .colorAttachments       = render_pass.color_attachments,
    .depthStencilAttachment = &wgpu_context->depth_stencil.att_desc,
  };
}

// Create attachment for multisampling support
static void create_multisampled_framebuffer(wgpu_context_t* wgpu_context)
{
  // Create the multi-sampled texture
  WGPUTextureDescriptor multisampled_frame_desc = {
    .size          = (WGPUExtent3D){
      .width               = wgpu_context->surface.width,
      .height              = wgpu_context->surface.height,
      .depthOrArrayLayers  = 1,
     },
    .mipLevelCount = 1,
    .sampleCount   = sample_count,
    .dimension     = WGPUTextureDimension_2D,
    .format        = wgpu_context->swap_chain.format,
    .usage         = WGPUTextureUsage_RenderAttachment,
  };
  render_pass.multisampled_framebuffer.texture
    = wgpuDeviceCreateTexture(wgpu_context->device, &multisampled_frame_desc);

  // Create the multi-sampled texture view
  render_pass.multisampled_framebuffer.view
    = wgpuTextureCreateView(render_pass.multisampled_framebuffer.texture,
                            &(WGPUTextureViewDescriptor){
                              .format         = wgpu_context->swap_chain.format,
                              .dimension      = WGPUTextureViewDimension_2D,
                              .baseMipLevel   = 0,
                              .mipLevelCount  = 1,
                              .baseArrayLayer = 0,
                              .arrayLayerCount = 1,
                            });
}

static void init_orbit_camera_matrices()
{
  // Model matrix
  glm_mat4_identity(scene_data.model_matrix);
  glm_rotate(scene_data.model_matrix, glm_rad(-90.0f),
             (vec3){1.0f, 0.0f, 0.0f});
  glm_translate(scene_data.model_matrix,
                (vec3){-plane_mesh.width / 2.0f,  // center plane x
                       -plane_mesh.height / 2.0f, // center plane y
                       0.0f});
}

static void update_uniform_buffers_scene(wgpu_example_context_t* context)
{
  wgpu_context_t* wgpu_context = context->wgpu_context;

  // Elapsed time
  if (!context->paused) {
    scene_data.elapsed_time = (context->run_time - start_time);
  }

  // MVP
  from_euler(controls.current_mouse_position[1],
             controls.current_mouse_position[0], 0.0f, &tmp_mtx.rotation);
  create_orbit_view_matrix(15, tmp_mtx.rotation, &tmp_mtx.view_matrix);

  // View position
  position_from_view_matrix(tmp_mtx.view_matrix, &scene_data.view_position);

  // Projection matrix
  const float aspect_ratio
    = (float)wgpu_context->surface.width / (float)wgpu_context->surface.height;
  glm_perspective(glm_rad(50.0f), aspect_ratio, 0.1f, 100.0f,
                  tmp_mtx.projection_matrix);

  // View projection matrix
  glm_mat4_mul(tmp_mtx.projection_matrix, tmp_mtx.view_matrix,
               scene_data.view_projection_matrix);

  // Update uniform buffer
  wgpu_queue_write_buffer(context->wgpu_context, uniform_buffers.scene, 0,
                          &scene_data, sizeof(scene_data));
}

static void
update_uniform_buffers_gerstner_waves(wgpu_example_context_t* context)
{
  // Normalize wave directions
  const uint32_t wave_count = (uint32_t)ARRAY_SIZE(gerstner_wave_params.waves);
  if (!gerstner_waves_normalized) {
    for (uint32_t i = 0; i < wave_count; ++i) {
      glm_vec2_normalize(gerstner_wave_params.waves[i].direction);
    }
    gerstner_waves_normalized = true;
  }

  // Calculate sum of wave amplitudes
  for (uint32_t i = 0; i < wave_count; ++i) {
    gerstner_wave_params.amplitude_sum
      += gerstner_wave_params.waves[i].amplitude;
  }

  // Update uniform buffer
  wgpu_queue_write_buffer(context->wgpu_context,
                          uniform_buffers.gerstner_wave_params, 0,
                          &gerstner_wave_params, sizeof(gerstner_wave_params));
}

static void prepare_uniform_buffers(wgpu_example_context_t* context)
{
  // Scene uniform buffer
  uniform_buffers.scene = wgpuDeviceCreateBuffer(
    context->wgpu_context->device,
    &(WGPUBufferDescriptor){
      .usage            = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform,
      .size             = sizeof(scene_data),
      .mappedAtCreation = false,
    });

  // Gerstner Waves parameters buffer
  uniform_buffers.gerstner_wave_params = wgpuDeviceCreateBuffer(
    context->wgpu_context->device,
    &(WGPUBufferDescriptor){
      .usage            = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform,
      .size             = sizeof(gerstner_wave_params),
      .mappedAtCreation = false,
    });

  // Initialize uniform buffers
  update_uniform_buffers_scene(context);
  update_uniform_buffers_gerstner_waves(context);
}

static int example_initialize(wgpu_example_context_t* context)
{
  if (context) {
    prepare_example(context);
    prepare_plane_mesh();
    init_orbit_camera_matrices();
    prepare_vertex_and_index_buffers(context->wgpu_context);
    prepare_uniform_buffers(context);
    prepare_texture(context->wgpu_context);
    setup_pipeline_layout(context->wgpu_context);
    setup_bind_groups(context->wgpu_context);
    prepare_pipelines(context->wgpu_context);
    create_multisampled_framebuffer(context->wgpu_context);
    setup_render_pass(context->wgpu_context);
    prepared = true;
    return 0;
  }

  return 1;
}

static WGPUCommandBuffer build_command_buffer(wgpu_context_t* wgpu_context)
{
  // Set target frame buffer
  if (sample_count == 1) {
    render_pass.color_attachments[0].view
      = wgpu_context->swap_chain.frame_buffer;
    render_pass.color_attachments[0].resolveTarget = NULL;
  }
  else {
    render_pass.color_attachments[0].view
      = render_pass.multisampled_framebuffer.view;
    render_pass.color_attachments[0].resolveTarget
      = wgpu_context->swap_chain.frame_buffer;
  }

  // Create command encoder
  wgpu_context->cmd_enc
    = wgpuDeviceCreateCommandEncoder(wgpu_context->device, NULL);

  // Create render pass
  wgpu_context->rpass_enc = wgpuCommandEncoderBeginRenderPass(
    wgpu_context->cmd_enc, &render_pass.descriptor);

  // Record record render pass
  wgpuRenderPassEncoderSetPipeline(wgpu_context->rpass_enc, pipeline);
  wgpuRenderPassEncoderSetVertexBuffer(wgpu_context->rpass_enc, 0,
                                       vertices.buffer, 0, WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderSetIndexBuffer(wgpu_context->rpass_enc, indices.buffer,
                                      WGPUIndexFormat_Uint32, 0,
                                      WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderSetBindGroup(wgpu_context->rpass_enc, 0,
                                    bind_groups.uniforms, 0, 0);
  wgpuRenderPassEncoderSetBindGroup(wgpu_context->rpass_enc, 1,
                                    bind_groups.textures, 0, 0);
  wgpuRenderPassEncoderDrawIndexed(wgpu_context->rpass_enc, indices.count, 1, 0,
                                   0, 0);

  // End render pass
  wgpuRenderPassEncoderEndPass(wgpu_context->rpass_enc);
  WGPU_RELEASE_RESOURCE(RenderPassEncoder, wgpu_context->rpass_enc)

  // Get command buffer
  WGPUCommandBuffer command_buffer
    = wgpu_get_command_buffer(wgpu_context->cmd_enc);
  WGPU_RELEASE_RESOURCE(CommandEncoder, wgpu_context->cmd_enc)

  return command_buffer;
}

static int example_draw(wgpu_context_t* wgpu_context)
{
  // Get next image in the swap chain (back/front buffer)
  wgpu_swap_chain_get_current_image(wgpu_context);

  // Create command buffer
  WGPUCommandBuffer command_buffer = build_command_buffer(wgpu_context);
  ASSERT(command_buffer != NULL);

  // Submit command buffer to the queue
  wgpu_flush_command_buffers(wgpu_context, &command_buffer, 1);

  // Present the current buffer to the swap chain
  wgpu_swap_chain_present(wgpu_context);

  return 0;
}

static int example_render(wgpu_example_context_t* context)
{
  if (!prepared) {
    return 1;
  }
  update_controls(context);
  update_uniform_buffers_scene(context);
  return example_draw(context->wgpu_context);
}

static void example_destroy(wgpu_example_context_t* context)
{
  UNUSED_VAR(context);

  wgpu_destroy_texture(&sea_color_texture);
  WGPU_RELEASE_RESOURCE(Buffer, vertices.buffer)
  WGPU_RELEASE_RESOURCE(Buffer, indices.buffer)
  WGPU_RELEASE_RESOURCE(Buffer, uniform_buffers.scene)
  WGPU_RELEASE_RESOURCE(Buffer, uniform_buffers.gerstner_wave_params)
  WGPU_RELEASE_RESOURCE(BindGroupLayout, bind_group_layouts.uniforms)
  WGPU_RELEASE_RESOURCE(BindGroupLayout, bind_group_layouts.textures)
  WGPU_RELEASE_RESOURCE(BindGroup, bind_groups.uniforms)
  WGPU_RELEASE_RESOURCE(BindGroup, bind_groups.textures)
  WGPU_RELEASE_RESOURCE(PipelineLayout, pipeline_layout)
  WGPU_RELEASE_RESOURCE(RenderPipeline, pipeline)
  WGPU_RELEASE_RESOURCE(Sampler, non_filtering_sampler)
  WGPU_RELEASE_RESOURCE(Texture, render_pass.multisampled_framebuffer.texture)
  WGPU_RELEASE_RESOURCE(TextureView, render_pass.multisampled_framebuffer.view)
}

void example_gerstner_waves(int argc, char* argv[])
{
  // clang-format off
  example_run(argc, argv, &(refexport_t){
    .example_settings = (wgpu_example_settings_t){
      .title = example_title,
      .vsync = true,
    },
    .example_initialize_func = &example_initialize,
    .example_render_func     = &example_render,
    .example_destroy_func    = &example_destroy,
  });
  // clang-format on
}