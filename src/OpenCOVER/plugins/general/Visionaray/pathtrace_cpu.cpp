/* This file is part of COVISE.

   You can use it under the terms of the GNU Lesser General Public License
   version 2.1 or later, see lgpl-2.1.txt.

 * License: LGPL 2+ */

#include <osg/Geometry>

#include <visionaray/kernels.h>

#include "mask_intersector.h"
#include "pathtrace_cpu.h"
#include "renderer.h"

void pathtrace_cpu(
        const visionaray::index_bvh<visionaray::index_bvh<visionaray::basic_triangle<3, float>>::bvh_inst>::bvh_ref* pbegin,
        const visionaray::index_bvh<visionaray::index_bvh<visionaray::basic_triangle<3, float>>::bvh_inst>::bvh_ref* pend,
        two_array_ref<visionaray::aligned_vector<visionaray::vec3>> const& normals,
        two_array_ref<visionaray::aligned_vector<visionaray::vec3>> const& shading_normals,
        two_array_ref<visionaray::aligned_vector<visionaray::vec2>> const& tex_coords,
        two_array_ref<material_list> const& materials,
        two_array_ref<visionaray::aligned_vector<visionaray::vec3>> const& colors,
        two_array_ref<texture_list> const& texture_refs,
        const visionaray::area_light<float, visionaray::basic_triangle<3, float>>* lbegin,
        const visionaray::area_light<float, visionaray::basic_triangle<3, float>>* lend,
        unsigned bounces,
        float epsilon,
        visionaray::vec4 clear_color,
        visionaray::vec4 ambient,
        visionaray::tiled_sched<host_ray_type>& sched,
        visionaray::mat4 view_matrix,
        visionaray::mat4 proj_matrix,
        visionaray::renderer* rend,
        unsigned& frame_num
        )
{
    using namespace visionaray;

    auto kparams = make_kernel_params(
        normals_per_vertex_binding{},
        colors_per_vertex_binding{},
        pbegin,
        pend,
        normals,
        shading_normals,
        tex_coords,
        materials,
        colors,
        texture_refs,
        lbegin,
        lend,
        bounces,
        epsilon,
        clear_color,
        vec4(1.0f));

    mask_intersector<two_array_ref<tex_coord_list>, two_array_ref<texture_list>> intersector;

    intersector.tex_coords = kparams.tex_coords;
    intersector.textures = kparams.textures;

    float alpha = 1.0f / ++frame_num;
    pixel_sampler::jittered_blend_type blend_params;
    blend_params.sfactor = alpha;
    blend_params.dfactor = 1.0f - alpha;

    auto sparams = make_sched_params(blend_params,
                                     view_matrix,
                                     proj_matrix,
                                     *rend,
                                     intersector);

    pathtracing::kernel<decltype(kparams)> k;
    k.params = kparams;
    sched.frame(k, sparams);
}
