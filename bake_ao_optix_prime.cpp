/*-----------------------------------------------------------------------
  Copyright (c) 2015-2016, NVIDIA. All rights reserved.
  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
   * Neither the name of its contributors may be used to endorse 
     or promote products derived from this software without specific
     prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
  PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
  OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
-----------------------------------------------------------------------*/

#include "bake_ao_optix_prime.h"
#include "bake_kernels.h"
#include "bake_util.h"

#include "Buffer.h"
#include <optix_prime/optix_primepp.h>
#include <optixu/optixu_math_namespace.h>
#include <optixu/optixu_matrix_namespace.h>

#include "random.h"

#include <algorithm>
#include <cstring> // memcpy
#include <float.h>
#include <iostream>
#include <map>

using namespace optix::prime;

#define ACCUM_TIME( t, x )        \
do {                              \
  t.start();                      \
  x;                              \
  t.stop();                       \
} while( false )

namespace
{

void createInstances( Context& context,
    const bake::Mesh* meshes, const size_t num_meshes, const bake::Instance* instances, const size_t num_instances, 
    const bool conserve_memory, const bool cpu_memory,
    // output, to keep allocations around
    std::vector<Buffer<float3>* >& allocated_vertex_buffers,
    std::vector<Buffer<int3>* >& allocated_index_buffers,
    std::vector<Model>& models, std::vector<RTPmodel>& prime_instances, std::vector<optix::Matrix4x4>& transforms)
{

  // For sharing identical buffers between Models
  std::map< float*, Buffer<float3>* > unique_vertex_buffers;
  std::map< unsigned int*, Buffer<int3>* > unique_index_buffers;

  const RTPbuffertype buffertype = cpu_memory ? RTP_BUFFER_TYPE_HOST : RTP_BUFFER_TYPE_CUDA_LINEAR;

  const size_t model_offset = models.size();
  models.reserve(models.size() + num_meshes);
  for (size_t meshIdx = 0; meshIdx < num_meshes; ++meshIdx) {
    Model model = context->createModel();
    if (conserve_memory){
      model->setBuilderParameter(RTP_BUILDER_PARAM_USE_CALLER_TRIANGLES, 1);
      model->setBuilderParameter<size_t>(RTP_BUILDER_PARAM_CHUNK_SIZE, 512 * 1024 * 1024);
    }
    const bake::Mesh& mesh = meshes[meshIdx];

    // Allocate or reuse vertex buffer and index buffer

    Buffer<float3>* vertex_buffer = NULL;
    if (unique_vertex_buffers.find(mesh.vertices) != unique_vertex_buffers.end()) {
      vertex_buffer = unique_vertex_buffers.find(mesh.vertices)->second;
    } else {
      // Note: copy disabled for Buffer, so need pointer here
      vertex_buffer = new Buffer<float3>( mesh.num_vertices, buffertype, UNLOCKED, mesh.vertex_stride_bytes );
      if ( cpu_memory ) {
        memcpy( vertex_buffer->ptr(), mesh.vertices, vertex_buffer->sizeInBytes() );
      } else {
        cudaMemcpy( vertex_buffer->ptr(), mesh.vertices, vertex_buffer->sizeInBytes(), cudaMemcpyHostToDevice );
      }
      unique_vertex_buffers[mesh.vertices] = vertex_buffer;
      allocated_vertex_buffers.push_back(vertex_buffer);
    }
    
    Buffer<int3>* index_buffer = NULL;
    if (unique_index_buffers.find(mesh.tri_vertex_indices) != unique_index_buffers.end()) {
      index_buffer = unique_index_buffers.find(mesh.tri_vertex_indices)->second;
    } else {
      index_buffer = new Buffer<int3>( mesh.num_triangles, buffertype );
      if ( cpu_memory ) {
        memcpy( index_buffer->ptr(), mesh.tri_vertex_indices, index_buffer->sizeInBytes() );
      } else {
        cudaMemcpy( index_buffer->ptr(), mesh.tri_vertex_indices, index_buffer->sizeInBytes(), cudaMemcpyHostToDevice );
      }
      unique_index_buffers[mesh.tri_vertex_indices] = index_buffer;
      allocated_index_buffers.push_back(index_buffer);
    }

    model->setTriangles(
        index_buffer->count(), index_buffer->type(), index_buffer->ptr(),
        vertex_buffer->count(), vertex_buffer->type(), vertex_buffer->ptr(), vertex_buffer->stride()
        );
    model->update( 0 );
    models.push_back(model);  // Model is ref counted, so need to return it to prevent destruction
  }

  prime_instances.reserve(prime_instances.size() + num_instances);
  for (int i = 0; i < num_instances; ++i) {
    const size_t index = model_offset + instances[i].mesh_index;
    RTPmodel rtp_model = models[index]->getRTPmodel();
    prime_instances.push_back(rtp_model);
    transforms.push_back(optix::Matrix4x4(instances[i].xform));
  }

}

inline size_t idivCeil( size_t x, size_t y )                                              
{                                                                                
    return (x + y-1)/y;                                                            
}


} // end namespace


void bake::ao_optix_prime(
    const Scene& scene,
    const Scene& blockers,
    const bake::AOSamples& ao_samples,
    const int rays_per_sample,
    const float  scene_offset,
    const float  scene_maxdistance,
    const bool   cpu_mode,
    const bool   conserve_memory,
    float* ao_values
    )
{

  Timer setup_timer;
  setup_timer.start( );

  Context ctx = Context::create(cpu_mode ? RTP_CONTEXT_TYPE_CPU : RTP_CONTEXT_TYPE_CUDA);

  std::vector<Model> models;
  std::vector<RTPmodel> prime_instances;
  std::vector<optix::Matrix4x4> transforms;
  std::vector< Buffer<float3>* > allocated_vertex_buffers;
  std::vector< Buffer<int3>* > allocated_index_buffers;
  createInstances(ctx, scene.meshes, scene.num_meshes, scene.instances, scene.num_instances, conserve_memory, cpu_mode,
    allocated_vertex_buffers, allocated_index_buffers, models, prime_instances, transforms );
  if (blockers.num_instances > 0) {
    createInstances(ctx, blockers.meshes, blockers.num_meshes, blockers.instances, blockers.num_instances, conserve_memory, cpu_mode,
      allocated_vertex_buffers, allocated_index_buffers, models, prime_instances, transforms ); 
  }
  Model scene_model = ctx->createModel();
  scene_model->setInstances( prime_instances.size(), RTP_BUFFER_TYPE_HOST, &prime_instances[0],
                      RTP_BUFFER_FORMAT_TRANSFORM_FLOAT4x4, RTP_BUFFER_TYPE_HOST, &transforms[0] );
  scene_model->update( 0 );

  Query query = scene_model->createQuery( RTP_QUERY_TYPE_ANY );

  const int sqrt_rays_per_sample = static_cast<int>( sqrtf( static_cast<float>( rays_per_sample ) ) + .5f );
  setup_timer.stop();

  Timer raygen_timer;
  Timer query_timer;
  Timer updateao_timer;
  Timer copyao_timer;

  unsigned seed = 0;

  // Split sample points into batches
  const size_t batch_size = 2000000;  // Note: fits on GTX 750 (1 GB) along with Hunter model
  const size_t num_batches = std::max(idivCeil(ao_samples.num_samples, batch_size), size_t(1));

  for (size_t batch_idx = 0; batch_idx < num_batches; batch_idx++, seed++) {

    setup_timer.start();
    const size_t sample_offset = batch_idx*batch_size;
    const size_t num_samples = std::min(batch_size, ao_samples.num_samples - sample_offset);

    // Set up sample buffers for generating rays on device, even when using host for ray query.
    // This keeps the rays similar for cpu queries vs. device queries.
    Buffer<float3> sample_normals     ( num_samples, RTP_BUFFER_TYPE_CUDA_LINEAR );
    Buffer<float3> sample_face_normals( num_samples, RTP_BUFFER_TYPE_CUDA_LINEAR );
    Buffer<float3> sample_positions   ( num_samples, RTP_BUFFER_TYPE_CUDA_LINEAR );

    cudaMemcpy( sample_normals.ptr(),      ao_samples.sample_normals + 3*sample_offset,      sample_normals.sizeInBytes(),  cudaMemcpyHostToDevice );
    cudaMemcpy( sample_face_normals.ptr(), ao_samples.sample_face_normals + 3*sample_offset, sample_face_normals.sizeInBytes(),  cudaMemcpyHostToDevice );
    cudaMemcpy( sample_positions.ptr(),    ao_samples.sample_positions + 3*sample_offset,    sample_positions.sizeInBytes(),  cudaMemcpyHostToDevice );

    bake::AOSamples ao_samples_device;
    ao_samples_device.num_samples = num_samples;
    ao_samples_device.sample_normals      = reinterpret_cast<float*>( sample_normals.ptr() );
    ao_samples_device.sample_face_normals = reinterpret_cast<float*>( sample_face_normals.ptr() );
    ao_samples_device.sample_positions    = reinterpret_cast<float*>( sample_positions.ptr() );
    ao_samples_device.sample_infos = 0;

    // Set up ray buffer on device.  For host queries, rays will be automatically copied to host by Prime.
    Buffer<Ray>   rays( num_samples, RTP_BUFFER_TYPE_CUDA_LINEAR );

    // Output buffer depends on query type
    Buffer<float> hits( num_samples, cpu_mode ? RTP_BUFFER_TYPE_HOST : RTP_BUFFER_TYPE_CUDA_LINEAR );
    
    query->setRays( rays.count(), Ray::format,             rays.type(), rays.ptr() );
    query->setHits( hits.count(), RTP_BUFFER_FORMAT_HIT_T, hits.type(), hits.ptr() );

    std::fill( ao_values, ao_values + ao_samples.num_samples, 0.0f );

    setup_timer.stop();

    if ( cpu_mode ) {

      for( int i = 0; i < sqrt_rays_per_sample; ++i )
      for( int j = 0; j < sqrt_rays_per_sample; ++j )
      {
        ACCUM_TIME(raygen_timer,    generateRaysDevice(seed, i, j, sqrt_rays_per_sample, scene_offset, scene_maxdistance, ao_samples_device, rays.ptr()));
        ACCUM_TIME( query_timer,    query->execute( 0 ) );
        ACCUM_TIME(updateao_timer,  updateAOHost((int)num_samples, hits.ptr(), &ao_values[sample_offset]));
      }
    } else {

      // Extra device buffer for accumulation
      Buffer<float> ao  ( num_samples, RTP_BUFFER_TYPE_CUDA_LINEAR );
      cudaMemset( ao.ptr(), 0, ao.sizeInBytes() );

      for( int i = 0; i < sqrt_rays_per_sample; ++i )
      for( int j = 0; j < sqrt_rays_per_sample; ++j )
      {
        ACCUM_TIME(raygen_timer,    generateRaysDevice(seed, i, j, sqrt_rays_per_sample, scene_offset, scene_maxdistance, ao_samples_device, rays.ptr()));
        ACCUM_TIME( query_timer,    query->execute( 0 ) );
        ACCUM_TIME(updateao_timer,  updateAODevice((int)num_samples, hits.ptr(), ao.ptr()));
      }

      // copy ao values back to host
      copyao_timer.start();
      cudaMemcpy( &ao_values[sample_offset], ao.ptr(), ao.sizeInBytes(), cudaMemcpyDeviceToHost ); 
      copyao_timer.stop();
    }
  }

  // normalize
  for( size_t  i = 0; i < ao_samples.num_samples; ++i ) {
    ao_values[i] = 1.0f - ao_values[i] / rays_per_sample; 
  }

  // clean up Buffer pointers.  Could be avoided with unique_ptr.
  for (size_t i = 0; i < allocated_vertex_buffers.size(); ++i) {
    delete allocated_vertex_buffers[i];
  }
  allocated_vertex_buffers.clear();
  for (size_t i = 0; i < allocated_index_buffers.size(); ++i) {
    delete allocated_index_buffers[i];
  }
  allocated_index_buffers.clear();


  std::cerr << "\n\tsetup ...           ";  printTimeElapsed( setup_timer );
  std::cerr << "\taccum raygen ...    ";  printTimeElapsed( raygen_timer );
  std::cerr << "\taccum query ...     ";  printTimeElapsed( query_timer );
  std::cerr << "\taccum update AO ... ";  printTimeElapsed( updateao_timer );
  std::cerr << "\tcopy AO out ...     ";  printTimeElapsed( copyao_timer );
}



