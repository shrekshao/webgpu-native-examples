#include "example_base.h"
#include "examples.h"

#include <string.h>

#include "../webgpu/imgui_overlay.h"

/* -------------------------------------------------------------------------- *
 * WebGPU Example - N-Body Simulation
 *
 * A simple N-body simulation implemented using WebGPU.
 *
 * Ref:
 * https://github.com/jrprice/NBody-WebGPU
 * https://en.wikipedia.org/wiki/N-body_simulation
 * -------------------------------------------------------------------------- */

#define NUM_BODIES 8192
#define WORKGROUP_SIZE 64
#define INITIAL_EYE_POSITION                                                   \
  {                                                                            \
    0.0f, 0.0f, -1.5f                                                          \
  }

// Simulation parameters
static uint32_t num_bodies = (uint32_t)NUM_BODIES;

// Shader parameters.
static uint32_t workgroup_size = (uint32_t)WORKGROUP_SIZE;

// Render parameters
static vec3 eye_position = INITIAL_EYE_POSITION;

// Uniform buffer block object
static struct {
  struct {
    WGPUBuffer buffer;
    uint64_t size;
  } render_params;
} uniform_buffers;

static struct {
  mat4 view_projection_matrix;
  mat4 projection_matrix;
  bool changed;
} render_params = {
  .view_projection_matrix = GLM_MAT4_ZERO_INIT,
  .projection_matrix      = GLM_MAT4_ZERO_INIT,
  .changed                = true,
};

// Storage buffer block objects
static struct {
  struct {
    WGPUBuffer buffer;
    uint64_t size;
    float positions[NUM_BODIES * 4];
  } positions_in;
  struct {
    WGPUBuffer buffer;
    uint64_t size;
  } positions_out;
  struct {
    WGPUBuffer buffer;
    uint64_t size;
  } velocities;
} storage_buffers;

// Bind group layouts
static struct {
  WGPUBindGroupLayout compute;
  WGPUBindGroupLayout render;
} bind_group_layouts;

// Bind groups
static struct {
  WGPUBindGroup compute[2];
  WGPUBindGroup render;
} bind_groups;

// Pipeline layouts
static struct {
  WGPUPipelineLayout compute;
  WGPUPipelineLayout render;
} pipeline_layouts;

// Pipelines
static struct {
  WGPUComputePipeline compute;
  WGPURenderPipeline render;
} pipelines;

// Render pass descriptor for frame buffer writes
static struct {
  WGPURenderPassColorAttachment color_attachments[1];
  WGPURenderPassDescriptor descriptor;
} render_pass;

// FPS counter
static struct {
  float fps_update_interval;
  float num_frames_since_fps_update;
  float last_fps_update_time;
  float fps;
  bool last_fps_update_time_valid;
} fps_counter = {
  .fps_update_interval         = 500.0f,
  .num_frames_since_fps_update = 0.0f,
  .last_fps_update_time_valid  = false,
};

static uint32_t frame_idx = 0;

// Other variables
static const char* example_title = "N-Body Simulation";
static bool prepared             = false;

// https://github.com/toji/gl-matrix/commit/e906eb7bb02822a81b1d197c6b5b33563c0403c0
static mat4* perspective_zo(mat4* out, float fovy, float aspect, float near,
                            float far)
{
  const float f  = 1.0f / tan(fovy / 2.0f);
  (*out)[0][0]   = f / aspect;
  (*out)[0][1]   = 0.0f;
  (*out)[0][2]   = 0.0f;
  (*out)[0][3]   = 0.0f;
  (*out)[1][0]   = 0.0f;
  (*out)[1][1]   = f;
  (*out)[1][2]   = 0.0f;
  (*out)[1][3]   = 0.0f;
  (*out)[2][0]   = 0.0f;
  (*out)[2][1]   = 0.0f;
  (*out)[2][3]   = -1.0f;
  (*out)[3][0]   = 0.0f;
  (*out)[3][1]   = 0.0f;
  (*out)[3][3]   = 0.0f;
  const float nf = 1.0f / (near - far);
  (*out)[2][2]   = far * nf;
  (*out)[3][2]   = far * near * nf;
  return out;
}

static void update_uniform_buffers(wgpu_example_context_t* context)
{
  wgpu_context_t* wgpu_context = context->wgpu_context;

  // Generate the view projection matrix
  glm_mat4_identity(render_params.projection_matrix);
  glm_mat4_identity(render_params.view_projection_matrix);
  const float aspect
    = (float)wgpu_context->surface.width / (float)wgpu_context->surface.height;
  perspective_zo(&render_params.projection_matrix, 1.0f, aspect, 0.1f, 50.0f);
  glm_translate(render_params.view_projection_matrix, eye_position);
  glm_mat4_mul(render_params.projection_matrix,
               render_params.view_projection_matrix,
               render_params.view_projection_matrix);

  // Write the render parameters to the uniform buffer
  wgpu_queue_write_buffer(wgpu_context, uniform_buffers.render_params.buffer, 0,
                          render_params.view_projection_matrix,
                          uniform_buffers.render_params.size);

  render_params.changed = false;
}

// Prepare and initialize uniform buffer containing shader uniforms
static void prepare_uniform_buffers(wgpu_example_context_t* context)
{
  // Vertex shader uniform buffer block
  WGPUBufferDescriptor buffer_desc = {
    .usage            = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform,
    .size             = sizeof(mat4), // sizeof(mat4x4<f32>)
    .mappedAtCreation = false,
  };
  uniform_buffers.render_params.buffer
    = wgpuDeviceCreateBuffer(context->wgpu_context->device, &buffer_desc);
  uniform_buffers.render_params.size = buffer_desc.size;

  update_uniform_buffers(context);
}

static float float_random(float min, float max)
{
  const float scale = rand() / (float)RAND_MAX; /* [0, 1.0] */
  return min + scale * (max - min);             /* [min, max] */
}

// Generate initial positions on the surface of a sphere
static void init_bodies(wgpu_context_t* wgpu_context)
{
  const float radius = 0.6f;
  float* positions   = storage_buffers.positions_in.positions;
  ASSERT(positions)
  float longitude = 0.0f, latitude = 0.0f;
  for (uint32_t i = 0; i < num_bodies; ++i) {
    longitude            = 2.0f * PI * float_random(0.0f, 1.0f);
    latitude             = acos((2.0f * float_random(0.0f, 1.0f) - 1.0f));
    positions[i * 4 + 0] = radius * sin(latitude) * cos(longitude);
    positions[i * 4 + 1] = radius * sin(latitude) * sin(longitude);
    positions[i * 4 + 2] = radius * cos(latitude);
    positions[i * 4 + 3] = 1.0f;
  }

  // Write the render parameters to the uniform buffer
  wgpu_queue_write_buffer(wgpu_context, storage_buffers.positions_in.buffer, 0,
                          storage_buffers.positions_in.positions,
                          storage_buffers.positions_in.size);
}

// Create buffers for body positions and velocities.
static void prepare_storage_buffers(wgpu_example_context_t* context)
{
  {
    WGPUBufferDescriptor buffer_desc = {
      .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst
               | WGPUBufferUsage_Vertex,
      .size             = num_bodies * 4 * 4,
      .mappedAtCreation = false,
    };
    storage_buffers.positions_in.buffer
      = wgpuDeviceCreateBuffer(context->wgpu_context->device, &buffer_desc);
    storage_buffers.positions_in.size = buffer_desc.size;
    ASSERT(storage_buffers.positions_in.buffer);
  }

  {
    WGPUBufferDescriptor buffer_desc = {
      .usage            = WGPUBufferUsage_Storage | WGPUBufferUsage_Vertex,
      .size             = num_bodies * 4 * 4,
      .mappedAtCreation = false,
    };
    storage_buffers.positions_out.buffer
      = wgpuDeviceCreateBuffer(context->wgpu_context->device, &buffer_desc);
    storage_buffers.positions_out.size = buffer_desc.size;
    ASSERT(storage_buffers.positions_out.buffer);
  }

  {
    WGPUBufferDescriptor buffer_desc = {
      .usage            = WGPUBufferUsage_Storage,
      .size             = num_bodies * 4 * 4,
      .mappedAtCreation = false,
    };
    storage_buffers.velocities.buffer
      = wgpuDeviceCreateBuffer(context->wgpu_context->device, &buffer_desc);
    storage_buffers.velocities.size = buffer_desc.size;
    ASSERT(storage_buffers.velocities.buffer);
  }

  // Generate initial positions on the surface of a sphere
  init_bodies(context->wgpu_context);
}

static void setup_compute_pipeline_layout(wgpu_context_t* wgpu_context)
{
  /* Compute pipeline layout */
  WGPUBindGroupLayoutEntry bgl_entries[3] = {
    [0] = (WGPUBindGroupLayoutEntry) {
      .binding = 0,
      .visibility = WGPUShaderStage_Compute,
      .buffer = (WGPUBufferBindingLayout) {
        .type = WGPUBufferBindingType_ReadOnlyStorage,
        .minBindingSize = storage_buffers.positions_in.size,
      },
      .sampler = {0},
    },
    [1] = (WGPUBindGroupLayoutEntry) {
      .binding = 1,
      .visibility = WGPUShaderStage_Compute,
      .buffer = (WGPUBufferBindingLayout) {
        .type = WGPUBufferBindingType_Storage,
        .minBindingSize = storage_buffers.positions_out.size,
      },
      .sampler = {0},
    },
    [2] = (WGPUBindGroupLayoutEntry) {
      .binding = 2,
      .visibility = WGPUShaderStage_Compute,
      .buffer = (WGPUBufferBindingLayout) {
        .type = WGPUBufferBindingType_Storage,
        .minBindingSize = storage_buffers.velocities.size,
      },
      .sampler = {0},
    },
  };
  WGPUBindGroupLayoutDescriptor bgl_desc = {
    .entryCount = (uint32_t)ARRAY_SIZE(bgl_entries),
    .entries    = bgl_entries,
  };
  bind_group_layouts.compute
    = wgpuDeviceCreateBindGroupLayout(wgpu_context->device, &bgl_desc);
  ASSERT(bind_group_layouts.compute != NULL)

  WGPUPipelineLayoutDescriptor compute_pipeline_layout_desc = {
    .bindGroupLayoutCount = 1,
    .bindGroupLayouts     = &bind_group_layouts.compute,
  };
  pipeline_layouts.compute = wgpuDeviceCreatePipelineLayout(
    wgpu_context->device, &compute_pipeline_layout_desc);
  ASSERT(pipeline_layouts.compute != NULL)
}

static void setup_render_pipeline_layout(wgpu_context_t* wgpu_context)
{
  /* Compute pipeline layout */
  WGPUBindGroupLayoutEntry bgl_entries[1] = {
    [0] = (WGPUBindGroupLayoutEntry) {
      .binding = 0,
      .visibility = WGPUShaderStage_Vertex,
      .buffer = (WGPUBufferBindingLayout) {
        .type = WGPUBufferBindingType_Uniform,
        .minBindingSize = uniform_buffers.render_params.size,
      },
      .sampler = {0},
    },
  };
  WGPUBindGroupLayoutDescriptor bgl_desc = {
    .entryCount = (uint32_t)ARRAY_SIZE(bgl_entries),
    .entries    = bgl_entries,
  };
  bind_group_layouts.render
    = wgpuDeviceCreateBindGroupLayout(wgpu_context->device, &bgl_desc);
  ASSERT(bind_group_layouts.render != NULL)

  WGPUPipelineLayoutDescriptor render_pipeline_layout_desc = {
    .bindGroupLayoutCount = 1,
    .bindGroupLayouts     = &bind_group_layouts.render,
  };
  pipeline_layouts.render = wgpuDeviceCreatePipelineLayout(
    wgpu_context->device, &render_pipeline_layout_desc);
  ASSERT(pipeline_layouts.render != NULL)
}

// Create the bind group for the compute shader.
static void setup_compute_bind_group(wgpu_context_t* wgpu_context)
{
  {
    WGPUBindGroupEntry bg_entries[3] = {
    [0] = (WGPUBindGroupEntry) {
      // Binding 0 : Input Positions
      .binding = 0,
      .buffer = storage_buffers.positions_in.buffer,
      .offset = 0,
      .size = storage_buffers.positions_in.size,
    },
    [1] = (WGPUBindGroupEntry) {
      // Binding 1 : Output Positions
      .binding = 1,
      .buffer = storage_buffers.positions_out.buffer,
      .offset = 0,
      .size = storage_buffers.positions_out.size,
    },
    [2] = (WGPUBindGroupEntry) {
      // Binding 2 : Velocities
      .binding = 2,
      .buffer = storage_buffers.velocities.buffer,
      .offset = 0,
      .size = storage_buffers.velocities.size,
    },
  };

    bind_groups.compute[0] = wgpuDeviceCreateBindGroup(
      wgpu_context->device, &(WGPUBindGroupDescriptor){
                              .layout     = bind_group_layouts.compute,
                              .entryCount = (uint32_t)ARRAY_SIZE(bg_entries),
                              .entries    = bg_entries,
                            });
    ASSERT(bind_groups.compute[0] != NULL)
  }

  {
    WGPUBindGroupEntry bg_entries[3] = {
      [0] = (WGPUBindGroupEntry) {
        // Binding 0 : Output Positions
        .binding = 0,
        .buffer = storage_buffers.positions_out.buffer,
        .offset = 0,
        .size = storage_buffers.positions_out.size,
      },
      [1] = (WGPUBindGroupEntry) {
        // Binding 1 : Input Positions
        .binding = 1,
        .buffer = storage_buffers.positions_in.buffer,
        .offset = 0,
        .size = storage_buffers.positions_in.size,
      },
    [2] = (WGPUBindGroupEntry) {
      // Binding 2 : Velocities
      .binding = 2,
      .buffer = storage_buffers.velocities.buffer,
      .offset = 0,
      .size = storage_buffers.velocities.size,
    },
  };

    bind_groups.compute[1] = wgpuDeviceCreateBindGroup(
      wgpu_context->device, &(WGPUBindGroupDescriptor){
                              .layout     = bind_group_layouts.compute,
                              .entryCount = (uint32_t)ARRAY_SIZE(bg_entries),
                              .entries    = bg_entries,
                            });
    ASSERT(bind_groups.compute[1] != NULL)
  }
}

// Create the bind group for the compute shader.
static void setup_render_bind_group(wgpu_context_t* wgpu_context)
{
  WGPUBindGroupEntry bg_entries[1] = {
    [0] = (WGPUBindGroupEntry) {
      // Binding 0 : Render params uniform buffer
      .binding = 0,
      .buffer = uniform_buffers.render_params.buffer,
      .offset = 0,
      .size = uniform_buffers.render_params.size,
    },
  };

  bind_groups.render = wgpuDeviceCreateBindGroup(
    wgpu_context->device, &(WGPUBindGroupDescriptor){
                            .layout     = bind_group_layouts.render,
                            .entryCount = (uint32_t)ARRAY_SIZE(bg_entries),
                            .entries    = bg_entries,
                          });
  ASSERT(bind_groups.render != NULL)
}

static void setup_render_pass(wgpu_context_t* wgpu_context)
{
  UNUSED_VAR(wgpu_context);

  // Color attachment
  render_pass.color_attachments[0] = (WGPURenderPassColorAttachment) {
      .view       = NULL,
      .loadOp     = WGPULoadOp_Clear,
      .storeOp    = WGPUStoreOp_Store,
      .clearColor = (WGPUColor) {
        .r = 0.0f,
        .g = 0.0f,
        .b = 0.1f,
        .a = 1.0f,
      },
  };

  // Render pass descriptor
  render_pass.descriptor = (WGPURenderPassDescriptor){
    .colorAttachmentCount   = 1,
    .colorAttachments       = render_pass.color_attachments,
    .depthStencilAttachment = NULL,
  };
}

// Create the compute pipeline
static void prepare_compute_pipeline(wgpu_context_t* wgpu_context)
{
  // Compute shader
  wgpu_shader_t compute_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Compute shader WGSL
                    .file  = "shaders/n_body_simulation/n_body_simulation.wgsl",
                    .entry = "cs_main",
                  });

  pipelines.compute = wgpuDeviceCreateComputePipeline(
    wgpu_context->device,
    &(WGPUComputePipelineDescriptor){
      .label   = "n_body_simulation_compute_pipeline",
      .layout  = pipeline_layouts.compute,
      .compute = compute_shader.programmable_stage_descriptor,
    });

  // Partial cleanup
  wgpu_shader_release(&compute_shader);
}

// Create the graphics pipeline
static void prepare_render_pipeline(wgpu_context_t* wgpu_context)
{
  // Primitive state
  WGPUPrimitiveState primitive_state_desc = {
    .topology  = WGPUPrimitiveTopology_TriangleList,
    .frontFace = WGPUFrontFace_CW,
    .cullMode  = WGPUCullMode_None,
  };

  // Color target state
  WGPUBlendState blend_state = {
    .color.operation = WGPUBlendOperation_Add,
    .color.srcFactor = WGPUBlendFactor_One,
    .color.dstFactor = WGPUBlendFactor_One,
    .alpha.operation = WGPUBlendOperation_Add,
    .alpha.srcFactor = WGPUBlendFactor_One,
    .alpha.dstFactor = WGPUBlendFactor_One,
  };
  WGPUColorTargetState color_target_state_desc = (WGPUColorTargetState){
    .format    = wgpu_context->swap_chain.format,
    .blend     = &blend_state,
    .writeMask = WGPUColorWriteMask_All,
  };

  // Vertex buffer layout
  WGPU_VERTEX_BUFFER_LAYOUT(
    position, 4 * sizeof(float),
    /* Attribute descriptions */
    // Attribute location 0: Position
    WGPU_VERTATTR_DESC(0, WGPUVertexFormat_Float32x4, 0))
  position_vertex_buffer_layout.stepMode = WGPUVertexStepMode_Instance;

  // Vertex state
  WGPUVertexState vertex_state_desc = wgpu_create_vertex_state(
                wgpu_context, &(wgpu_vertex_state_t){
                .shader_desc = (wgpu_shader_desc_t){
                  // Vertex shader WGSL
                  .file = "shaders/n_body_simulation/n_body_simulation.wgsl",
                  .entry = "vs_main",
                },
                .buffer_count = 1,
                .buffers = &position_vertex_buffer_layout,
              });

  // Fragment state
  WGPUFragmentState fragment_state_desc = wgpu_create_fragment_state(
                wgpu_context, &(wgpu_fragment_state_t){
                .shader_desc = (wgpu_shader_desc_t){
                  // Fragment shader WGSL
                  .file = "shaders/n_body_simulation/n_body_simulation.wgsl",
                  .entry = "fs_main",
                },
                .target_count = 1,
                .targets = &color_target_state_desc,
              });

  // Multisample state
  WGPUMultisampleState multisample_state_desc
    = wgpu_create_multisample_state_descriptor(
      &(create_multisample_state_desc_t){
        .sample_count = 1,
      });

  // Create rendering pipeline using the specified states
  pipelines.render = wgpuDeviceCreateRenderPipeline(
    wgpu_context->device, &(WGPURenderPipelineDescriptor){
                            .label        = "n_body_simulation_render_pipeline",
                            .layout       = pipeline_layouts.render,
                            .primitive    = primitive_state_desc,
                            .vertex       = vertex_state_desc,
                            .fragment     = &fragment_state_desc,
                            .depthStencil = NULL,
                            .multisample  = multisample_state_desc,
                          });

  // Partial cleanup
  WGPU_RELEASE_RESOURCE(ShaderModule, vertex_state_desc.module);
  WGPU_RELEASE_RESOURCE(ShaderModule, fragment_state_desc.module);
}

static int example_initialize(wgpu_example_context_t* context)
{
  if (context) {
    prepare_uniform_buffers(context);
    prepare_storage_buffers(context);
    setup_compute_pipeline_layout(context->wgpu_context);
    setup_render_pipeline_layout(context->wgpu_context);
    prepare_compute_pipeline(context->wgpu_context);
    prepare_render_pipeline(context->wgpu_context);
    setup_compute_bind_group(context->wgpu_context);
    setup_render_bind_group(context->wgpu_context);
    setup_render_pass(context->wgpu_context);
    prepared = true;
    return 0;
  }

  return 1;
}

static void example_on_update_ui_overlay(wgpu_example_context_t* context)
{
  if (imgui_overlay_header("Settings")) {
    imgui_overlay_checkBox(context->imgui_overlay, "Paused", &context->paused);
  }
}

static WGPUCommandBuffer build_command_buffer(wgpu_example_context_t* context)
{
  wgpu_context_t* wgpu_context          = context->wgpu_context;
  render_pass.color_attachments[0].view = wgpu_context->swap_chain.frame_buffer;

  // Create command encoder
  wgpu_context->cmd_enc
    = wgpuDeviceCreateCommandEncoder(wgpu_context->device, NULL);

  // Compute pass
  if (!context->paused) {
    // Set up the compute shader dispatch
    wgpu_context->cpass_enc
      = wgpuCommandEncoderBeginComputePass(wgpu_context->cmd_enc, NULL);
    wgpuComputePassEncoderSetPipeline(wgpu_context->cpass_enc,
                                      pipelines.compute);
    wgpuComputePassEncoderSetBindGroup(wgpu_context->cpass_enc, 0,
                                       bind_groups.compute[frame_idx], 0, NULL);
    wgpuComputePassEncoderDispatch(
      wgpu_context->cpass_enc, ceil(num_bodies / (float)workgroup_size), 1, 1);
    wgpuComputePassEncoderEndPass(wgpu_context->cpass_enc);
    WGPU_RELEASE_RESOURCE(ComputePassEncoder, wgpu_context->cpass_enc)
    frame_idx = (frame_idx + 1) % 2;
  }

  // Render pass
  {
    wgpu_context->rpass_enc = wgpuCommandEncoderBeginRenderPass(
      wgpu_context->cmd_enc, &render_pass.descriptor);
    wgpuRenderPassEncoderSetPipeline(wgpu_context->rpass_enc, pipelines.render);
    wgpuRenderPassEncoderSetBindGroup(wgpu_context->rpass_enc, 0,
                                      bind_groups.render, 0, NULL);
    wgpuRenderPassEncoderSetVertexBuffer(wgpu_context->rpass_enc, 0,
                                         frame_idx == 0 ?
                                           storage_buffers.positions_in.buffer :
                                           storage_buffers.positions_out.buffer,
                                         0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderDraw(wgpu_context->rpass_enc, 6, num_bodies, 0, 0);
    wgpuRenderPassEncoderEndPass(wgpu_context->rpass_enc);
    WGPU_RELEASE_RESOURCE(RenderPassEncoder, wgpu_context->rpass_enc)
  }

  // Draw ui overlay
  draw_ui(wgpu_context->context, example_on_update_ui_overlay);

  // Get command buffer
  WGPUCommandBuffer command_buffer
    = wgpu_get_command_buffer(wgpu_context->cmd_enc);
  ASSERT(command_buffer != NULL)
  WGPU_RELEASE_RESOURCE(CommandEncoder, wgpu_context->cmd_enc)

  return command_buffer;
}

static int example_draw(wgpu_example_context_t* context)
{
  // Prepare frame
  prepare_frame(context);

  // Command buffer to be submitted to the queue
  wgpu_context_t* wgpu_context                   = context->wgpu_context;
  wgpu_context->submit_info.command_buffer_count = 1;
  wgpu_context->submit_info.command_buffers[0] = build_command_buffer(context);

  // Submit to queue
  submit_command_buffers(context);

  // Submit frame
  submit_frame(context);

  return 0;
}

static void update_fps_counter(wgpu_example_context_t* context)
{
  if (fps_counter.last_fps_update_time_valid) {
    const float now                 = context->frame.timestamp_millis;
    const float time_since_last_log = now - fps_counter.last_fps_update_time;
    if (time_since_last_log >= fps_counter.fps_update_interval) {
      fps_counter.fps = fps_counter.num_frames_since_fps_update
                        / (time_since_last_log / 1000.0);
      fps_counter.last_fps_update_time        = now;
      fps_counter.num_frames_since_fps_update = 0;
    }
  }
  else {
    fps_counter.last_fps_update_time       = context->frame.timestamp_millis;
    fps_counter.last_fps_update_time_valid = true;
  }
  ++fps_counter.num_frames_since_fps_update;
}

static int example_render(wgpu_example_context_t* context)
{
  if (!prepared) {
    return 1;
  }
  update_fps_counter(context);
  bool result = example_draw(context);
  if (render_params.changed) {
    update_uniform_buffers(context);
  }
  return result;
}

static void example_on_key_pressed(keycode_t key)
{
  if (key == KEY_UP || key == KEY_DOWN) {
    static const float z_inc = 0.025f;
    if (key == KEY_UP) {
      eye_position[2] += z_inc;
    }
    else if (key == KEY_DOWN) {
      eye_position[2] -= z_inc;
    }
    // Update render parameters based on key presses
    render_params.changed = true;
  }
}

// Clean up used resources
static void example_destroy(wgpu_example_context_t* context)
{
  UNUSED_VAR(context);

  WGPU_RELEASE_RESOURCE(Buffer, uniform_buffers.render_params.buffer)
  WGPU_RELEASE_RESOURCE(Buffer, storage_buffers.positions_in.buffer)
  WGPU_RELEASE_RESOURCE(Buffer, storage_buffers.positions_out.buffer)
  WGPU_RELEASE_RESOURCE(Buffer, storage_buffers.velocities.buffer)
  WGPU_RELEASE_RESOURCE(BindGroupLayout, bind_group_layouts.compute)
  WGPU_RELEASE_RESOURCE(BindGroupLayout, bind_group_layouts.render)
  WGPU_RELEASE_RESOURCE(BindGroup, bind_groups.compute[0])
  WGPU_RELEASE_RESOURCE(BindGroup, bind_groups.compute[1])
  WGPU_RELEASE_RESOURCE(BindGroup, bind_groups.render)
  WGPU_RELEASE_RESOURCE(PipelineLayout, pipeline_layouts.compute)
  WGPU_RELEASE_RESOURCE(PipelineLayout, pipeline_layouts.render)
  WGPU_RELEASE_RESOURCE(ComputePipeline, pipelines.compute)
  WGPU_RELEASE_RESOURCE(RenderPipeline, pipelines.render)
}

void example_n_body_simulation(int argc, char* argv[])
{
  // clang-format off
  example_run(argc, argv, &(refexport_t){
    .example_settings = (wgpu_example_settings_t){
      .title   = example_title,
      .overlay = true,
    },
    .example_initialize_func      = &example_initialize,
    .example_render_func          = &example_render,
    .example_destroy_func         = &example_destroy,
    .example_on_key_pressed_func  = &example_on_key_pressed,
  });
  // clang-format on
}