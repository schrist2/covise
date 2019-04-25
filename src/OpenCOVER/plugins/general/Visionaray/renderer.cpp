/* This file is part of COVISE.

   You can use it under the terms of the GNU Lesser General Public License
   version 2.1 or later, see lgpl-2.1.txt.

 * License: LGPL 2+ */

#include <cassert>

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <type_traits>

#include <osg/io_utils>
#include <osg/LightModel>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Sequence>
#include <osg/Switch>
#include <osg/TriangleIndexFunctor>

#include <osgViewer/Renderer>

#include <cover/coVRConfig.h>
#include <cover/coVRLighting.h>
#include <cover/coVRPluginSupport.h>
#include <cover/VRSceneGraph.h>
#include <cover/VRViewer.h>

#include <visionaray/gl/bvh_outline_renderer.h>

#include "kernels/bvh_costs_kernel.h"
#include "kernels/normals_kernel.h"
#include "kernels/tex_coords_kernel.h"
#include "common.h"
#include "mask_intersector.h"
#include "pathtrace_cpu.h"
#include "render_cpu.h"
#ifdef __CUDACC__
#include "pathtrace_gpu.h"
#include "render_gpu.h"
#endif
#include "renderer.h"
#include "scene_monitor.h"
#include "state.h"
#include "two_array_ref.h"

namespace visionaray
{
    //-------------------------------------------------------------------------------------------------
    // Get stereo mode from osg::RenderInfo
    //

    osg::DisplaySettings::StereoMode get_stereo_mode(osg::RenderInfo const &info)
    {
        if (auto state = info.getState())
        {
            if (auto ds = state->getDisplaySettings())
            {
                return ds->getStereoMode();
            }
        }

        // StereoMode unfortunately provides no reasonable default
        return osg::DisplaySettings::StereoMode();
    }

    //-------------------------------------------------------------------------------------------------
    // Get default Visionaray material
    //

    material_type get_default_material()
    {
        plastic<float> vsnray_mat;
        vsnray_mat.ca() = from_rgb(0.2f, 0.2f, 0.2f);
        vsnray_mat.cd() = from_rgb(0.8f, 0.8f, 0.8f);
        vsnray_mat.cs() = from_rgb(0.1f, 0.1f, 0.1f);
        vsnray_mat.ka() = 1.0f;
        vsnray_mat.kd() = 1.0f;
        vsnray_mat.ks() = 1.0f;
        vsnray_mat.specular_exp() = 32.0f;
        return material_type(vsnray_mat);
    }


    //-------------------------------------------------------------------------------------------------
    // Insert a new texture into a list and return a reference,
    // or
    // return a reference to a texture from the list that was already inserted
    //

    host_tex_type& get_or_insert_texture(osg::Texture2D const* tex, osg::Image const* img, texture_map& textures)
    {
        assert(img->isDataContiguous()); // TODO

        auto dest_format = PF_RGBA8;
        auto source_format = map_gl_format(
                img->getPixelFormat(),
                img->getDataType(),
                osg::Image::computeNumComponents(img->getPixelFormat()) * sizeof(uint8_t) /* TODO */
                );

        auto source_info = map_pixel_format(source_format);

        assert(source_info.components == 3 || source_info.components == 4);

        std::string filename = img->getFileName();

        if (filename.empty())
        {
            filename = std::string("TEXTURE") + std::to_string(textures.size());
        }

        auto p = textures.emplace(std::make_pair(
                    filename,
                    host_tex_type(img->s(), img->t())));

        bool inserted = p.second;
        auto it = inserted ? p.first : textures.find(img->getFileName());
        assert(it != textures.end());

        auto &result = it->second;

        if (inserted)
        {
            result.set_address_mode(0, osg_cast(tex->getWrap(osg::Texture::WRAP_S)));
            result.set_address_mode(1, osg_cast(tex->getWrap(osg::Texture::WRAP_T)));

//          auto min_filter = tex->getFilter(osg::Texture::MIN_FILTER);
            auto mag_filter = tex->getFilter(osg::Texture::MAG_FILTER);

            result.set_filter_mode(osg_cast(mag_filter));

            if (source_info.components == 3)
            {
                auto data_ptr = reinterpret_cast<vector<3, unorm<8> > const *>(img->data());
                result.reset(data_ptr, source_format, dest_format, AlphaIsOne);
            }
            else if (source_info.components == 4)
            {
                auto data_ptr = reinterpret_cast<vector<4, unorm<8> > const *>(img->data());
                result.reset(data_ptr, source_format, dest_format);
            }
            else
            {
                assert(0); // TODO
            }
        }

        return result;
    }

    //-------------------------------------------------------------------------------------------------
    // Functor that stores triangles from osg::Drawable
    //

    class store_triangle
    {
    public:
        void init(
            osg::Vec3Array const *in_vertices,
            osg::Vec3Array const *in_normals,
            osg::Vec4Array const *in_colors,
            osg::Vec2Array const *in_tex_coords,
            unsigned in_geom_id,
            triangle_list &out_triangles,
            normal_list &out_normals,
            normal_list &out_shading_normals,
            color_list &out_colors,
            tex_coord_list &out_tex_coords)
        {
            in.vertices = in_vertices;
            in.normals = in_normals;
            in.colors = in_colors;
            in.tex_coords = in_tex_coords;
            in.geom_id = in_geom_id;
            out.triangles = &out_triangles;
            out.normals = &out_normals;
            out.shading_normals = &out_shading_normals;
            out.colors = &out_colors;
            out.tex_coords = &out_tex_coords;
        }

        void operator()(unsigned i1, unsigned i2, unsigned i3) const
        {

            // triangles

            assert(in.vertices && out.triangles);
            assert(in.vertices->size() > i1 && in.vertices->size() > i2 && in.vertices->size() > i3);

            auto v1 = (*in.vertices)[i1];
            auto v2 = (*in.vertices)[i2];
            auto v3 = (*in.vertices)[i3];

            triangle_type tri;

            tri.v1 = osg_cast(v1);
            tri.e1 = osg_cast(v2) - tri.v1;
            tri.e2 = osg_cast(v3) - tri.v1;

            if (length(cross(tri.e1, tri.e2)) == 0.0f)
            {
                // TODO: implement some kind of error logging
                return;
            }

            tri.prim_id = out.triangles->size();
            tri.geom_id = in.geom_id;
            out.triangles->push_back(tri);

            // normals

            assert(in.normals && out.normals);
            assert(in.normals->size() > i1 && in.normals->size() > i2 && in.normals->size() > i3);

            // mul left instead of transposing the matrix
            // see http://forum.openscenegraph.org/viewtopic.php?t=2494
            auto osg_n1 = osg::Vec4((*in.normals)[i1], 1.0);
            auto osg_n2 = osg::Vec4((*in.normals)[i2], 1.0);
            auto osg_n3 = osg::Vec4((*in.normals)[i3], 1.0);

            auto n1 = osg_cast(osg_n1).xyz();
            auto n2 = osg_cast(osg_n2).xyz();
            auto n3 = osg_cast(osg_n3).xyz();

            // assign normalize(vec3(1, 1, 1)) to zero-length normals
            auto validate = [=](vec3 n)->vec3
            {
                vec3 result = n;
                if (length(result) < numeric_limits<float>::epsilon())
                {
                    result = vec3(1.0f);
                }
                return normalize(result);
            };

            out.normals->push_back(normalize(cross(tri.e1, tri.e2)));

            out.shading_normals->push_back(validate(n1));
            out.shading_normals->push_back(validate(n2));
            out.shading_normals->push_back(validate(n3));

            assert(out.triangles->size() == out.shading_normals->size() / 3);

            // colors

            if (in.colors && in.colors->getBinding() == osg::Array::BIND_PER_VERTEX && in.colors->size() > max(i1, i2, i3))
            {
                out.colors->push_back(osg_cast((*in.colors)[i1]).xyz());
                out.colors->push_back(osg_cast((*in.colors)[i2]).xyz());
                out.colors->push_back(osg_cast((*in.colors)[i3]).xyz());
            }
            else if (in.colors && in.colors->getBinding() == osg::Array::BIND_OVERALL && in.colors->size() >= 1)
            {
                out.colors->push_back(osg_cast((*in.colors)[0]).xyz());
                out.colors->push_back(osg_cast((*in.colors)[0]).xyz());
                out.colors->push_back(osg_cast((*in.colors)[0]).xyz());
            }
            else
            {
                out.colors->emplace_back(1.0f);
                out.colors->emplace_back(1.0f);
                out.colors->emplace_back(1.0f);
            }

            // tex coords

            if (in.tex_coords && in.tex_coords->size() > max(i1, i2, i3))
            {
                out.tex_coords->push_back(osg_cast((*in.tex_coords)[i1]));
                out.tex_coords->push_back(osg_cast((*in.tex_coords)[i2]));
                out.tex_coords->push_back(osg_cast((*in.tex_coords)[i3]));
            }
            else
            {
                out.tex_coords->emplace_back(0.0f);
                out.tex_coords->emplace_back(0.0f);
                out.tex_coords->emplace_back(0.0f);
            }
        }

    private:
        // Store pointers because osg::TriangleIndexFunctor is shitty..

        struct
        {
            osg::Vec3Array const *vertices = nullptr;
            osg::Vec3Array const *normals = nullptr;
            osg::Vec4Array const *colors = nullptr;
            osg::Vec2Array const *tex_coords = nullptr;
            unsigned geom_id;
        } in;

        struct
        {
            triangle_list *triangles = nullptr;
            normal_list *normals = nullptr;
            normal_list *shading_normals = nullptr;
            color_list *colors = nullptr;
            tex_coord_list *tex_coords = nullptr;
        } out;
    };

    //-------------------------------------------------------------------------------------------------
    // Visitor to check visibility by traversing upwards to a node's parents
    //

    class visibility_visitor : public osg::NodeVisitor
    {
    public:
        using base_type = osg::NodeVisitor;
        using base_type::apply;

    public:
        visibility_visitor(osg::Node *node)
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_PARENTS)
            , last_child_(node)
            , visible_(true)
        {
        }

        bool is_visible() const
        {
            return visible_;
        }

        void apply(osg::Node &node)
        {
            auto sw = dynamic_cast<osg::Switch *>(&node);
            if (sw && sw->containsNode(last_child_))
            {
                visible_ &= sw->getChildValue(last_child_);
            }

            auto seq = dynamic_cast<osg::Sequence *>(&node);
            if (seq && seq->containsNode(last_child_))
            {
                auto ts = seq->getValue();
                visible_ &= seq->getChild(ts) == last_child_;
            }

            if (visible_)
            {
                last_child_ = &node;
                traverse(node);
            }
        }

    private:
        osg::Node *last_child_;
        bool visible_;
    };

    //-------------------------------------------------------------------------------------------------
    // Visitor to acquire scene data
    //

    class get_scene_visitor : public osg::NodeVisitor
    {
    public:
        using base_type = osg::NodeVisitor;
        using base_type::apply;

    public:
        get_scene_visitor(
            triangle_list &triangles,
            normal_list &normals,
            normal_list &shading_normals,
            color_list &colors,
            tex_coord_list &tex_coords,
            material_list &materials,
            texture_map &textures,
            texture_list &texture_refs,
            node_mask_map &node_masks,
            scene::Monitor &mon,
            std::vector<host_bvh_type>& bvhs,
            index_bvh<host_bvh_type::bvh_inst>& top_level_bvh,
            std::vector<size_t>& instance_indices,
            std::vector<mat4>& instance_transforms,
            const std::vector<osg::Sequence *> &managed_seqs = {},
            TraversalMode tm = TRAVERSE_ALL_CHILDREN)
            : base_type(tm)
            , triangles_(triangles)
            , normals_(normals)
            , shading_normals_(shading_normals)
            , colors_(colors)
            , tex_coords_(tex_coords)
            , materials_(materials)
            , textures_(textures)
            , texture_refs_(texture_refs)
            , node_masks_(node_masks)
            , scene_monitor_(mon)
            , current_transform_(mat4::identity())
            , bvhs_(bvhs)
            , top_level_bvh_(top_level_bvh)
            , instance_indices_(instance_indices)
            , instance_transforms_(instance_transforms)
            , managed_seqs_(managed_seqs)
        {
        }

        void apply(osg::Sequence &seq)
        {
            // Ignore sequences that are explicitly marked so
            if (std::find(managed_seqs_.begin(), managed_seqs_.end(), &seq) != managed_seqs_.end())
                return;

            base_type::traverse(seq);
        }

        void apply(osg::Node &node)
        {
            // State from node is propagated to children
            auto parent_set = node.getStateSet();
            auto parent_mattr = parent_set ? parent_set->getAttribute(osg::StateAttribute::MATERIAL) : nullptr;

            if (auto pmat = dynamic_cast<osg::Material *>(parent_mattr))
            {
                parent_mat_ = pmat;
            }

            auto parent_tattr = parent_set ? parent_set->getTextureAttribute(0, osg::StateAttribute::TEXTURE) : nullptr;
            if (auto ptex = dynamic_cast<osg::Texture2D *>(parent_tattr))
            {
                if (ptex->getImage())
                {
                    parent_tex_ = ptex;
                    parent_img_ = ptex->getImage();
                }
            }

            if (auto transform = dynamic_cast<osg::Transform*>(&node))
            {
                mat4 prev = current_transform_;

                current_transform_ = current_transform_ * osg_cast(transform->asMatrixTransform()->getMatrix());

                transform_nodes_list_.push_back(transform);

                base_type::traverse(node);

                transform_nodes_list_.pop_back();

                current_transform_ = prev;

                return;
            }
            else if (auto geode = dynamic_cast<osg::Geode *>(&node))
            {

                // Record number of encountered triangles to check if this node
                // is handled by Visionaray
                size_t num_triangles = triangles_.size();

                for (size_t i = 0; i < geode->getNumDrawables(); ++i)
                {
                    auto drawable = geode->getDrawable(i);
                    if (!drawable)
                    {
                        continue;
                    }

                    auto geom = drawable->asGeometry();
                    if (!geom)
                    {
                        continue;
                    }

                    auto node_vertices = static_cast<osg::Vec3Array *>(geom->getVertexArray());
                    if (!node_vertices || node_vertices->getType() != osg::Array::Vec3ArrayType)
                    {
                        continue;
                    }

                    if (node_vertices->size() == 0)
                    {
                        continue;
                    }

                    auto node_normals = static_cast<osg::Vec3Array *>(geom->getNormalArray());
                    if (!node_normals || node_normals->getType() != osg::Array::Vec3ArrayType)
                    {
                        continue;
                    }

                    if (node_normals->size() == 0)
                    {
                        continue;
                    }

                    osg::Vec4Array *node_colors = nullptr;
                    if (geom->getColorArray() && geom->getColorArray()->getType() == osg::Array::Vec4ArrayType)
                    {
                        node_colors = static_cast<osg::Vec4Array *>(geom->getColorArray());
                    }
                    // ok if node_colors == 0


                    // Simple checks are done - traverse parents to see if node is visible

                    visibility_visitor visible(geode);
                    geode->accept(visible);

                    // TODO: scene is no longer acquired in drawImplementation
                    /*                if (!visible.is_visible())
                                      {
                    // no other children will be visible, either
                    break;
                    }*/

                    unsigned tex_unit = 0;
                    auto node_tex_coords = dynamic_cast<osg::Vec2Array *>(geom->getTexCoordArray(tex_unit));
                    // ok if node_tex_coords == 0

                    auto set = geom->getStateSet();

                    // material

                    auto mattr = set ? set->getAttribute(osg::StateAttribute::MATERIAL) : nullptr;
                    auto mat = dynamic_cast<osg::Material *>(mattr);

                    bool spec = opencover::coVRLighting::instance()->specularlightState;

                    if (mat)
                    {
                        materials_.push_back(osg_cast(mat, spec));
                        scene_monitor_.add_observable(std::make_shared<scene::Material>(mat,
                                                                                        materials_,
                                                                                        materials_.size() - 1));
                    }
                    else
                    {
                        if (parent_mat_)
                        {
                            materials_.push_back(osg_cast(parent_mat_, spec));
                            scene_monitor_.add_observable(std::make_shared<scene::Material>(parent_mat_,
                                                                                            materials_,
                                                                                            materials_.size() - 1));
                        }
                        else
                            materials_.push_back(get_default_material());
                    }

                    // texture

                    auto tattr = set != nullptr ? set->getTextureAttribute(0, osg::StateAttribute::TEXTURE) : nullptr;
                    auto tex = dynamic_cast<osg::Texture2D *>(tattr);
                    auto img = tex != nullptr ? tex->getImage() : nullptr;

                    if (tex && img)
                    {
                        auto &vsnray_tex = get_or_insert_texture(tex, img, textures_);
                        texture_refs_.emplace_back(vsnray_tex);
                    }
                    else
                    {
                        if (parent_tex_ && parent_img_)
                        {
                            auto &vsnray_tex = get_or_insert_texture(parent_tex_, parent_img_, textures_);
                            texture_refs_.emplace_back(vsnray_tex);
                        }
                        else
                        {
                            host_tex_type dummy_texture(1, 1);
                            dummy_texture.set_address_mode(Wrap);
                            dummy_texture.set_filter_mode(Nearest);

                            vector<4, unorm<8>> dummy_texel(1.0f, 1.0f, 1.0f, 1.0f);
                            dummy_texture.reset(&dummy_texel);

                            textures_.insert(std::make_pair("null", std::move(dummy_texture)));

                            // Maybe a "null" texture was already present and thus not inserted
                            //  ==> find the one that was already inserted
                            auto it = textures_.find("null");

                            // Insert a ref
                            texture_refs_.push_back(host_tex_type::ref_type(it->second));
                        }
                    }

                    assert(materials_.size() == texture_refs_.size());

                    // transform

                    //auto world_transform = osg::computeLocalToWorld(getNodePath());

                    // geometry

                    assert(static_cast<material_list::size_type>(static_cast<unsigned>(materials_.size()) == materials_.size()));
                    unsigned geom_id = static_cast<unsigned>(materials_.size() - 1);

                    osg::TriangleIndexFunctor<store_triangle> tif;
                    tif.init(
                        node_vertices,
                        node_normals,
                        node_colors,
                        node_tex_coords,
                        geom_id,
                        triangles_,
                        normals_,
                        shading_normals_,
                        colors_,
                        tex_coords_);
                    geom->accept(tif);

                    // Build single bvh
                    bvhs_.emplace_back(build<host_bvh_type>(
                        triangles_.data() + num_triangles,
                        triangles_.size() - num_triangles,
                        false//builder == Split
                        ));

                    instance_indices_.push_back(bvhs_.size() - 1);
                    instance_transforms_.push_back(current_transform_);

                    scene_monitor_.add_observable(std::make_shared<scene::Transform>(transform_nodes_list_, top_level_bvh_, bvhs_.size() - 1));
                }

                if (triangles_.size() > num_triangles)
                {
                    // Store old nodemask so we can restore it e.g. when
                    // the plugin is unloaded
                    node_masks_.insert(std::make_pair(geode, geode->getNodeMask()));

                    // Geometry node is handled by Visionaray, cull for rendering
                    geode->setNodeMask(
                            opencover::cover->getObjectsRoot()->getNodeMask()
                            & ~opencover::VRViewer::instance()->getCullMask()
                            & ~opencover::VRViewer::instance()->getCullMaskLeft()
                            & ~opencover::VRViewer::instance()->getCullMaskRight());

                }
                else
                {
                    // Geometry node not handled by Visionaray - if there is
                    // an old nodemask, reset culling accordingly
                    auto it = node_masks_.find(geode);
                    if (it != node_masks_.end())
                        geode->setNodeMask(it->second);
                }
            }

            base_type::traverse(node);
        }

    private:
        triangle_list &triangles_;
        normal_list &normals_;
        normal_list &shading_normals_;
        color_list &colors_;
        tex_coord_list &tex_coords_;
        material_list &materials_;
        texture_map &textures_;
        texture_list &texture_refs_;
        node_mask_map &node_masks_;
        scene::Monitor &scene_monitor_;
        const std::vector<osg::Sequence *> &managed_seqs_;

        // Propagate state to child nodes
        osg::Material *parent_mat_ = nullptr;
        osg::Texture2D *parent_tex_ = nullptr;
        osg::Image *parent_img_ = nullptr;

        std::vector<osg::Transform*> transform_nodes_list_;
        mat4 current_transform_ = mat4::identity();
        std::vector<host_bvh_type>& bvhs_;
        index_bvh<host_bvh_type::bvh_inst>& top_level_bvh_;
        std::vector<size_t>& instance_indices_;
        std::vector<mat4>& instance_transforms_;
    };

    //-------------------------------------------------------------------------------------------------
    // Visitor to set node masks of the scene graph
    // E.g. called when the plugin gets unloaded to restore the original node masks
    //

    class set_node_masks_visitor : public osg::NodeVisitor
    {
    public:
        using base_type = osg::NodeVisitor;
        using base_type::apply;

    public:
        set_node_masks_visitor(node_mask_map &node_masks,          // the node masks
                               node_mask_map *old_masks = nullptr, // optionally store the old masks
                               TraversalMode tm = TRAVERSE_ALL_CHILDREN)
            : base_type(tm)
            , node_masks_(node_masks)
            , old_masks_(old_masks)
        {
        }

        void apply(osg::Node &node)
        {
            auto it = node_masks_.find(&node);
            if (it != node_masks_.end())
            {
                // Optionally store the old node mask before applying the new one
                if (old_masks_)
                    old_masks_->insert(std::make_pair(&node, node.getNodeMask()));

                // Set the new mask
                node.setNodeMask(it->second);
            }

            base_type::traverse(node);
        }

    private:
        node_mask_map &node_masks_;
        node_mask_map *old_masks_;
    };


    //-------------------------------------------------------------------------------------------------
    // Visitor to acquire scene lights
    //

    class get_light_visitor : public osg::NodeVisitor
    {
    public:
        using base_type = osg::NodeVisitor;
        using base_type::apply;

        // Two ways to find lights:
        // By calling LightSource::getLight()
        // By inspecting a general node's stateset

        enum CheckMode { CheckLightSources, CheckStateSets };

        void setCheckMode(CheckMode cm)
        {
            checkMode_ = cm;
        }


    public:
        get_light_visitor(light_list& lights, TraversalMode tm)
            : base_type(tm)
            , lights_(lights)
        {
        }

        void apply(osg::LightSource &ls)
        {
            if (checkMode_ == CheckStateSets)
            {
                base_type::traverse(ls);
                return;
            }

            bool isOn = false;

            // Ignore what's in the light source,
            // cover doesn't set that properly
//          isOn |= lightIsOn(ls.getLight(), ls.getStateSet());

            for (auto &n: getNodePath())
            {
                if (n == &ls) // ignore the light source again
                    continue;

                if (n != nullptr)
                    isOn |= lightIsOn(ls.getLight(), n->getStateSet());
            }

            if (isOn)
                process_light(ls.getLight());

            base_type::traverse(ls);
            return;
        }

        void apply(osg::Node &node)
        {
            if (&node == opencover::cover->getMenuGroup())
                return;

            if (checkMode_ == CheckLightSources)
            {
                base_type::traverse(node);
                return;
            }

            auto set = node.getStateSet();
            if (set == nullptr)
            {
                base_type::traverse(node);
                return;
            }

            for (int i = 0; i < opencover::coVRLighting::MaxNumLights; ++i)
            {
                auto l = dynamic_cast<osg::Light *>(set->getAttribute(osg::StateAttribute::LIGHT, i));
                if (l != nullptr && lightIsOn(l, set))
                    process_light(l);
            }

            base_type::traverse(node);
        }

    private:

        light_list& lights_;
        std::vector<osg::Light *> processed_;
        CheckMode checkMode_ = CheckLightSources;

        bool lightIsOn(const osg::Light *l, const osg::StateSet *set) const
        {
            if (l == nullptr || set == nullptr)
                return false;

            auto mode = set->getMode(GL_LIGHT0 + l->getLightNum());

            if ((mode & osg::StateAttribute::ON) == osg::StateAttribute::ON)
                return true;
            else
                return false;
        }

        void process_light(osg::Light *l)
        {
            // Append a visionaray light if this light was found anew

            if (l == nullptr)
                return;

            if (std::find(processed_.begin(), processed_.end(), l) != processed_.end())
                return;

            auto lpos = osg_cast(l->getPosition());
            auto spot_dir = vec4(osg_cast(l->getDirection()), 1.0f);

            auto world_trans = osg::computeLocalToWorld(getNodePath());
            auto obj_trans = osg::computeLocalToWorld(opencover::cover->getObjectsRoot()->getParentalNodePaths()[0]);
            lpos = inverse(osg_cast(obj_trans)) * osg_cast(world_trans) * lpos;

            auto ldiff = osg_cast(l->getDiffuse());

            // transform spot dir
            spot_dir = inverse(transpose(inverse(osg_cast(obj_trans)))) * inverse(transpose(osg_cast(world_trans))) * spot_dir;


            // map OpenGL [-1,1] to Visionaray [0,1]
            ldiff += 1.0f;
            ldiff /= 2.0f;

            light_type light;

            light.set_position(lpos.xyz());
            light.set_cl(ldiff.xyz());
            light.set_kl(ldiff.w);


            light.set_spot_direction(normalize(spot_dir.xyz()));
            light.set_spot_cutoff(l->getSpotCutoff() * constants::degrees_to_radians<float>());
            light.set_spot_exponent(l->getSpotExponent());

            light.set_constant_attenuation(l->getConstantAttenuation());
            light.set_linear_attenuation(l->getLinearAttenuation());
            light.set_quadratic_attenuation(l->getQuadraticAttenuation());

            lights_.push_back(light);

            // Intentionally only mark as processed if
            // the light is turned on
            processed_.push_back(l);
        }
    };

    //-------------------------------------------------------------------------------------------------
    // Private implementation
    //

    struct renderer::impl
    {

        impl()
            : host_sched(0)
#ifdef __CUDACC__
            , device_sched(8, 8)
#endif
            , multi_channel_drawer(nullptr)
        {
        }

        std::vector<triangle_list>                              triangles;
        std::vector<normal_list>                                normals;
        std::vector<normal_list>                                shading_normals;
        std::vector<tex_coord_list>                             tex_coords;
        std::vector<color_list>                                 colors;
        std::vector<material_list>                              materials;
        std::vector<texture_list>                               texture_refs;
        std::vector<host_bvh_type>                              host_bvhs;
        std::vector<host_bvh_type::bvh_inst>                    host_instances;
        index_bvh<host_bvh_type::bvh_inst>                      host_top_level_bvh;
        std::vector<size_t>                                     instance_indices;
        std::vector<mat4>                                       instance_transforms;
        texture_map                                             textures;
        host_sched_type                                         host_sched;
        mask_intersector<
            two_array_ref<tex_coord_list>,
            two_array_ref<texture_list>>                        host_intersector;
        aligned_vector<area_light<float, triangle_type>>        host_area_lights;

#ifdef __CUDACC__
        std::vector<thrust::device_vector<vec3>>                device_normals;
        std::vector<thrust::device_vector<vec2>>                device_tex_coords;
        std::vector<thrust::device_vector<vertex_color_type>>   device_colors;
        std::vector<thrust::device_vector<material_type>>       device_materials;
        std::vector<device_texture_list>                        device_texture_refs;
        std::vector<device_bvh_type>                            device_bvhs;
        device_texture_map                                      device_textures;
        device_sched_type                                       device_sched;
        mask_intersector<
            two_array_ref<device_tex_coord_list>,
            two_array_ref<device_texture_list>>                 device_intersector;
        thrust::device_vector<area_light<float, triangle_type>> device_area_lights;
#endif

        scene::Monitor                                          scene_monitor;

        struct viewing_params
        {
            int channel_index;
            mat4 view_matrix;
            mat4 proj_matrix;
            int width;
            int height;
            unsigned frame_num = 0;
            bool need_clear_frame = false;

            viewing_params(int index) :
                channel_index(index)
            {

            }
    };

        osg::ref_ptr<opencover::MultiChannelDrawer> multi_channel_drawer;

        std::vector<viewing_params> channel_viewing_params;

        size_t total_frame_num = 0;

        algorithm algo_current = Simple;
        unsigned num_bounces = 4;
        device_type device = CPU;

        // Store the scene graph nodes' original node masks
        // so we can restore them later
        node_mask_map node_masks;

        // If we suppress ray tracing rendering and let OpenCOVER render
        // instead, we keep a copy of the ray tracing node masks so that
        // we can reapply them later on
        node_mask_map ray_tracing_masks;

        std::vector<gl::bvh_outline_renderer> outlines;
        std::vector<bool> outlines_initialized;

        std::shared_ptr<render_state> state = nullptr;
        std::shared_ptr<debug_state> dev_state = nullptr;

        void reset_multi_channel_drawer()
        {
            if (multi_channel_drawer != nullptr)
            {
                opencover::cover->getScene()->removeChild(multi_channel_drawer);

                multi_channel_drawer = nullptr;
            }

            multi_channel_drawer = new opencover::MultiChannelDrawer(false, state->device == GPU);

            multi_channel_drawer->setMode(opencover::MultiChannelDrawer::AsIs);

            opencover::cover->getScene()->addChild(multi_channel_drawer);

            channel_viewing_params.clear();
            for (int i = 0; i < multi_channel_drawer->numViews(); i++)
            {
                channel_viewing_params.push_back(viewing_params(i));
            }
        }

        void update_state(
            std::shared_ptr<render_state> const &state,
            std::shared_ptr<debug_state> const &dev_state)
        {
            this->state = state;
            this->dev_state = dev_state;

            if (state)
            {
                host_sched.reset(
                    state->num_threads > 0
                        ? state->num_threads
                        : std::thread::hardware_concurrency());
            }
        }

        void update_viewing_params(int channel_index, osg::DisplaySettings::StereoMode mode);
        bool scene_valid();
        void update_device_data(unsigned bits = 0xFFFFFFFF);
        void commit_state();
    };

    void renderer::impl::update_viewing_params(int channel_index, osg::DisplaySettings::StereoMode mode)
    {
        viewing_params& vparams = channel_viewing_params[channel_index];

        auto osg_cam = opencover::coVRConfig::instance()->channels[channel_index].camera;
        auto osg_viewport = osg_cam->getViewport();
        int w = osg_viewport->width();
        int h = osg_viewport->height();

        auto view = osg_cast(multi_channel_drawer->modelMatrix(channel_index) * multi_channel_drawer->viewMatrix(channel_index));
        auto proj = osg_cast(multi_channel_drawer->projectionMatrix(channel_index));

        bool isAnaglyphic = mode == osg::DisplaySettings::StereoMode::ANAGLYPHIC;

        if (state->data_var == Dynamic || state->algo != algo_current || state->device != device
            || state->num_bounces != num_bounces || scene_monitor.need_clear_frame()
            || ((vparams.view_matrix != view || vparams.proj_matrix != proj) && !isAnaglyphic)
            || vparams.width != w || vparams.height != h)
        {
            vparams.frame_num = 0;

            if (state->algo == Pathtracing)
            {
                vparams.need_clear_frame = true;
            }
        }

        vparams.view_matrix = view;
        vparams.proj_matrix = proj;

        if (vparams.width != w || vparams.height != h || device != state->device)
        {
            pixel_format_info dpfi = map_pixel_format(VSNRAY_DEPTH_PIXEL_FORMAT);
            pixel_format_info cpfi = map_pixel_format(VSNRAY_COLOR_PIXEL_FORMAT);

            vparams.width = w;
            vparams.height = h;
            multi_channel_drawer->resizeView(channel_index, w, h, dpfi.type, cpfi.type);
        }
    }

    bool renderer::impl::scene_valid()
    {
        if (host_bvhs.size() == 0)
            return false;

        bool equal_size = true;
        size_t size = host_bvhs.size();
        if (normals.size() != size)         equal_size = false;
        size = normals.size();
        if (tex_coords.size() != size)      equal_size = false;
        size = tex_coords.size();
        if (colors.size() != size)          equal_size = false;
        size = colors.size();
        if (materials.size() != size)       equal_size = false;
        size = materials.size();
        if (texture_refs.size() != size)    equal_size = false;

        return equal_size;
    }

    void renderer::impl::update_device_data(unsigned bits)
    {
#ifdef __CUDACC__
        if (!scene_valid())
            return;

        try
        {
            if (bits & scene::Monitor::UpdateGeometry)
            {
                device_bvhs.resize(host_bvhs.size());
                for (size_t i = 0; i < host_bvhs.size(); ++i)
                    device_bvhs[i] = device_bvh_type(host_bvhs[i]);
            }

            if (bits & scene::Monitor::UpdateNormals)
            {
                device_normals.resize(normals.size());
                for (size_t i = 0; i < normals.size(); ++i)
                    device_normals[i] = normals[i];
            }

            if (bits & scene::Monitor::UpdateTexCoords)
            {
                device_tex_coords.resize(tex_coords.size());
                for (size_t i = 0; i < tex_coords.size(); ++i)
                    device_tex_coords[i] = tex_coords[i];
            }

            if (bits & scene::Monitor::UpdateColors)
            {
                device_colors.resize(colors.size());
                for (size_t i = 0; i < colors.size(); ++i)
                    device_colors[i] = colors[i];
            }

            if (bits & scene::Monitor::UpdateMaterials)
            {
                device_materials.resize(materials.size());
                for (size_t i = 0; i < materials.size(); ++i)
                    device_materials[i] = materials[i];
            }

            if (bits & scene::Monitor::UpdateTextures)
            {
                device_texture_refs.resize(texture_refs.size());
                for (size_t i = 0; i < texture_refs.size(); ++i)
                    device_texture_refs[i].resize(texture_refs[i].size());

                device_textures.clear();

                for (auto const &pair_host_tex : textures)
                {
                    auto const &host_tex = pair_host_tex.second;
                    device_tex_type device_tex(pair_host_tex.second);
                    auto const &p = device_textures.emplace(pair_host_tex.first, std::move(device_tex));

                    assert(p.second /* inserted */);

                    auto it = p.first;

                    // TODO: construct GPU data during get_scene_visitor traversal
                    for (size_t r = 0; r < texture_refs.size(); ++r)
                    {
                        for (size_t i = 0; i < texture_refs[r].size(); ++i)
                        {
                            if (texture_refs[r][i].data() == host_tex.data())
                            {
                                device_texture_refs[r][i] = device_tex_ref_type(it->second);
                            }
                        }
                    }
                }
            }

            if (bits & scene::Monitor::UpdateAreaLights)
            {
                device_area_lights = host_area_lights;
            }
        }
        catch (std::bad_alloc&)
        {
            std::cerr << "GPU memory allocation failed\n";
            device_bvhs.clear();
            device_bvhs.shrink_to_fit();
            device_normals.clear();
            device_normals.shrink_to_fit();
            device_tex_coords.clear();
            device_tex_coords.shrink_to_fit();
            device_materials.clear();
            device_materials.shrink_to_fit();
            device_textures.clear();
            device_texture_refs.clear();
            device_texture_refs.shrink_to_fit();
            device_area_lights.clear();
            device_area_lights.shrink_to_fit();
        }
#else
        (void)bits;
#endif
    }

    void renderer::impl::commit_state()
    {
        algo_current = state->algo;
        num_bounces = state->num_bounces;
        device = state->device;
    }

    //-------------------------------------------------------------------------------------------------
    //
    //

    renderer::renderer()
        : impl_(new impl)
        , cur_channel_(0)
    {

    }

    renderer::~renderer()
    {
        set_node_masks_visitor visitor(impl_->node_masks);
        opencover::cover->getObjectsRoot()->accept(visitor);
        for (size_t i = 0; i < impl_->outlines_initialized.size(); ++i)
            if (impl_->outlines_initialized[i])
                impl_->outlines[i].destroy();

        opencover::cover->getScene()->removeChild(impl_->multi_channel_drawer);
    }

    renderer::color_type* renderer::color()
    {
        return reinterpret_cast<color_type*>(impl_->multi_channel_drawer->rgba(cur_channel_));
    }

    renderer::depth_type* renderer::depth()
    {
        return reinterpret_cast<depth_type*>(impl_->multi_channel_drawer->depth(cur_channel_));
    }

    renderer::color_type const* renderer::color() const
    {
        return reinterpret_cast<color_type*>(impl_->multi_channel_drawer->rgba(cur_channel_));
    }

    renderer::depth_type const* renderer::depth() const
    {
        return reinterpret_cast<depth_type*>(impl_->multi_channel_drawer->depth(cur_channel_));
    }

    int renderer::width() const
    {
        return impl_->channel_viewing_params[cur_channel_].width;
    }

    int renderer::height() const
    {
        return impl_->channel_viewing_params[cur_channel_].height;
    }

    renderer::ref_type renderer::ref()
    {
        return { color(), depth(), width(), height() };
    }

    void renderer::update_state(
        std::shared_ptr<render_state> const &state,
        std::shared_ptr<debug_state> const &dev_state)
    {
        impl_->update_state(state, dev_state);
    }

    void renderer::acquire_scene_data(const std::vector<osg::Sequence *> &seqs)
    {
        // TODO: real dynamic scenes :)

        int max_seq_len = 0;
        for (const auto &seq : seqs)
        {
            max_seq_len = max(max_seq_len, static_cast<int>(seq->getNumFrames()));
        }

        // static data + sequences
        int num_frames = 1 + max_seq_len;

        impl_->triangles.clear();
        impl_->normals.clear();
        impl_->shading_normals.clear();
        impl_->colors.clear();
        impl_->tex_coords.clear();
        impl_->materials.clear();
        impl_->texture_refs.clear();
        impl_->host_bvhs.clear();
        impl_->host_instances.clear();
        impl_->instance_indices.clear();
        impl_->instance_transforms.clear();

        impl_->triangles.resize(num_frames);
        impl_->normals.resize(num_frames);
        impl_->shading_normals.resize(num_frames);
        impl_->colors.resize(num_frames);
        impl_->tex_coords.resize(num_frames);
        impl_->materials.resize(num_frames);
        impl_->texture_refs.resize(num_frames);

        // Acquire static scene data
        get_scene_visitor visitor(
                impl_->triangles[0],
                impl_->normals[0],
                impl_->shading_normals[0],
                impl_->colors[0],
                impl_->tex_coords[0],
                impl_->materials[0],
                impl_->textures,
                impl_->texture_refs[0],
                impl_->node_masks,
                impl_->scene_monitor,
                impl_->host_bvhs,
                impl_->host_top_level_bvh,
                impl_->instance_indices,
                impl_->instance_transforms,
                seqs
                );
       opencover::cover->getObjectsRoot()->accept(visitor); 

        // Acquire dynamic sequence data
        for (int i = 1; i < num_frames; ++i)
        {
            // Ignore sequences that are managed
            // by coVRAnimationManager
            get_scene_visitor visitor(
                    impl_->triangles[i],
                    impl_->normals[i],
                    impl_->shading_normals[i],
                    impl_->colors[i],
                    impl_->tex_coords[i],
                    impl_->materials[i],
                    impl_->textures,
                    impl_->texture_refs[i],
                    impl_->node_masks,
                    impl_->scene_monitor,
                    impl_->host_bvhs,
                    impl_->host_top_level_bvh,
                    impl_->instance_indices,
                    impl_->instance_transforms,
                    seqs
                    );

            for (auto &seq : seqs)
            {
                if (seq && seq->getChild(i - 1))
                    seq->getChild(i - 1)->accept(visitor);
            }
        }

        impl_->host_instances.resize(impl_->instance_indices.size());
        for (size_t i = 0; i < impl_->instance_indices.size(); ++i)
        {
            size_t index = impl_->instance_indices[i];
            impl_->host_instances[i] = impl_->host_bvhs[index].inst(impl_->instance_transforms[i]);
        }

        impl_->host_top_level_bvh = build<index_bvh<host_bvh_type::bvh_inst>>(
            impl_->host_instances.data(),
            impl_->host_instances.size(),
            false
            );

        impl_->outlines = std::vector<gl::bvh_outline_renderer>(impl_->triangles.size()); // outlines are not copyable!
        impl_->outlines_initialized.resize(impl_->triangles.size());
        std::fill(impl_->outlines_initialized.begin(), impl_->outlines_initialized.end(), false);

        // Loop over all triangles, check if their
        // material is emissive, and if so, build
        // BVHs to create area lights from.

        struct range
        {
            std::size_t begin;
            std::size_t end; 
            unsigned    geom_id;
        };   

        std::vector<range> ranges;

        for (std::size_t i = 0; i < impl_->triangles[0].size(); ++i) 
        {    
            auto pi = impl_->triangles[0][i];
            if (impl_->materials[0][pi.geom_id].as<emissive<float>>() != nullptr)
            {    
                range r;
                r.begin = i; 

                std::size_t j = i + 1; 
                for (;j < impl_->triangles[0].size(); ++j) 
                {    
                    auto pii = impl_->triangles[0][j];
                    if (impl_->materials[0][pii.geom_id].as<emissive<float>>() == nullptr
                            || pii.geom_id != pi.geom_id)
                    {    
                        break;
                    }    
                }    

                r.end = j; 
                r.geom_id = pi.geom_id;
                ranges.push_back(r);

                i = r.end - 1; 
            }    
        }

        for (auto r : ranges)
        {    
            for (std::size_t i = r.begin; i != r.end; ++i) 
            {    
                area_light<float, basic_triangle<3, float>> light(impl_->triangles[0][i]);
                auto mat = *impl_->materials[0][r.geom_id].as<emissive<float>>();
                light.set_cl(to_rgb(mat.ce()));
                light.set_kl(mat.ls());
                impl_->host_area_lights.push_back(light);
            }
        }

        // Copy scene to GPU
        impl_->update_device_data();
    }

    void renderer::set_suppress_rendering(bool enable)
    {
        if (enable)
        {
            // Apply the node masks we stored when acquiring the scene,
            // store the masks used for ray tracing so we can reapply
            // them later on
            set_node_masks_visitor visitor(impl_->node_masks, &impl_->ray_tracing_masks);
            opencover::cover->getObjectsRoot()->accept(visitor);
        }
        else
        {
            // Reset to the ray tracing node masks obtained when
            // acquiring the scene data
            set_node_masks_visitor visitor(impl_->ray_tracing_masks);
            opencover::cover->getObjectsRoot()->accept(visitor);
        }

        impl_->dev_state->suppress_rendering = enable;
    }

    void renderer::expandBoundingSphere(osg::BoundingSphere &bs)
    {
        aabb bounds;
        bounds.invalidate();

        for (auto const &b : impl_->host_bvhs)
        {
            if (b.num_nodes() > 0)
                bounds = combine(bounds, b.node(0).get_bounds());
        }

        auto c = bounds.center();
        osg::BoundingSphere::vec_type center(c.x, c.y, c.z);
        osg::BoundingSphere::value_type radius = length(c - bounds.min);
        bs.set(center, radius);
    }

    //-------------------------------------------------------------------------------------------------
    // Render a frame with Visionaray
    //

    void renderer::render_frame(osg::RenderInfo &info)
    {
        if (!impl_->state || !impl_->dev_state)
            return;

        if (impl_->dev_state->suppress_rendering)
            return;

        if (impl_->multi_channel_drawer == nullptr || impl_->device != impl_->state->device)
        {
            // Init MultiChannelDrawer on startup or reset when switching device.
            impl_->reset_multi_channel_drawer();

            // Rebuild scene first thing after launch.
            if (impl_->multi_channel_drawer == nullptr)
            {
                impl_->state->rebuild = true;

                return;
            }
        }

        // Update scene state

        impl_->scene_monitor.update();
        impl_->update_device_data(impl_->scene_monitor.update_bits());

        light_list lights;

        get_light_visitor lvisitor(lights, osg::NodeVisitor::TRAVERSE_ALL_CHILDREN);

        // Check all light sources
        lvisitor.setCheckMode(get_light_visitor::CheckLightSources);
        opencover::cover->getScene()->accept(lvisitor);

        // Now check all state sets if they contain lights
        // (lights that are associated with a light source
        // should already be in the lights list and thus ignored,
        // order matters here!)
        lvisitor.setCheckMode(get_light_visitor::CheckStateSets);
        opencover::cover->getScene()->accept(lvisitor);

        aabb bounds;
        bounds.invalidate();
        for (auto &b : impl_->host_bvhs)
        {
            if (b.num_nodes())
                bounds = combine(bounds, b.node(0).get_bounds());
        }
        auto diagonal = bounds.max - bounds.min;
        auto bounces = impl_->state->num_bounces;
        auto epsilon = max(1E-3f, length(diagonal) * 1E-5f);

        // Kernel params

        int frame = impl_->state->animation_frame + 1; // first BVH contains static data

        for (cur_channel_ = 0; cur_channel_ < impl_->channel_viewing_params.size(); ++cur_channel_)
        {
            // Camera matrices, render target resize

            impl_->update_viewing_params(cur_channel_, get_stereo_mode(info));

            auto renderer = dynamic_cast<osgViewer::Renderer *>(opencover::coVRConfig::instance()->channels[cur_channel_].camera->getRenderer());
            auto scene_view = renderer->getSceneView(0);
            auto stateset = scene_view->getGlobalStateSet();
            auto light_model = dynamic_cast<osg::LightModel *>(stateset->getAttribute(osg::StateAttribute::LIGHTMODEL));
            auto ambient = osg_cast(light_model->getAmbientIntensity());
            auto clear_color = osg_cast(opencover::coVRConfig::instance()->channels[cur_channel_].camera->getClearColor());

            auto &vparams = impl_->channel_viewing_params[cur_channel_];
            if (vparams.need_clear_frame)
            {
                vparams.frame_num = 0;

                impl_->multi_channel_drawer->clearColor(cur_channel_);
                impl_->multi_channel_drawer->clearDepth(cur_channel_);

                vparams.need_clear_frame = false;
            }

            if (impl_->state->device == GPU)
            {
#ifdef __CUDACC__
                thrust::device_vector<device_bvh_type::bvh_ref> primitives;

                if (impl_->device_bvhs.size() > 0 && impl_->device_bvhs[0].num_primitives())
                    primitives.push_back(impl_->device_bvhs[0].ref());

                if (impl_->device_bvhs.size() > frame && impl_->device_bvhs[frame].num_primitives())
                    primitives.push_back(impl_->device_bvhs[frame].ref());

                thrust::device_vector<light_type> device_lights = lights;

                auto has_prims_func = [&](size_t index)
                {
                    return impl_->device_bvhs.size() > index && impl_->device_bvhs[index].num_primitives() != 0;
                };

                two_array_ref<device_normal_list>    normals = make_two_array_ref(impl_->device_normals, 0, frame, has_prims_func);
                two_array_ref<device_tex_coord_list> tex_coords = make_two_array_ref(impl_->device_tex_coords, 0, frame, has_prims_func);
                two_array_ref<device_material_list>  materials = make_two_array_ref(impl_->device_materials, 0, frame, has_prims_func);
                two_array_ref<device_color_list>     colors = make_two_array_ref(impl_->device_colors, 0, frame, has_prims_func);
                two_array_ref<device_texture_list>   texture_refs = make_two_array_ref(impl_->device_texture_refs, 0, frame, has_prims_func);

                auto &vparams = impl_->channel_viewing_params[cur_channel_];
                if (impl_->state->algo == Pathtracing)
                {
                    pathtrace_gpu(
                        thrust::raw_pointer_cast(primitives.data()),
                        thrust::raw_pointer_cast(primitives.data()) + primitives.size(),
                        normals,
                        tex_coords,
                        materials,
                        colors,
                        texture_refs,
                        thrust::raw_pointer_cast(impl_->device_area_lights.data()),
                        thrust::raw_pointer_cast(impl_->device_area_lights.data()) + impl_->device_area_lights.size(),
                        bounces,
                        epsilon,
                        clear_color,
                        vec4(1.0f),
                        impl_->device_sched,
                        vparams.view_matrix,
                        vparams.proj_matrix,
                        this,
                        vparams.frame_num);
                }
                else
                {
                    render_gpu(
                        thrust::raw_pointer_cast(primitives.data()),
                        thrust::raw_pointer_cast(primitives.data()) + primitives.size(),
                        normals,
                        tex_coords,
                        materials,
                        colors,
                        texture_refs,
                        thrust::raw_pointer_cast(device_lights.data()),
                        thrust::raw_pointer_cast(device_lights.data()) + device_lights.size(),
                        bounces,
                        epsilon,
                        clear_color,
                        vec4(1.0f),
                        impl_->device_sched,
                        vparams.view_matrix,
                        vparams.proj_matrix,
                        this,
                        impl_->state,
                        impl_->dev_state);
                }
#endif
            }
            else if (impl_->state->device == CPU)
            {
                /*

                aligned_vector<host_bvh_type::bvh_ref> primitives;

                if (impl_->host_bvhs.size() > 0 && impl_->host_bvhs[0].num_primitives())
                    primitives.push_back(impl_->host_bvhs[0].ref());

                if (impl_->host_bvhs.size() > frame && impl_->host_bvhs[frame].num_primitives())
                    primitives.push_back(impl_->host_bvhs[frame].ref());

                */

                auto bvh_ref = impl_->host_top_level_bvh.ref();

                auto has_prims_func = [&](size_t index)
                {
                    return index == 0; // Breaks dynamic geometry in BVH 1...n
                    return impl_->host_bvhs.size() > index && impl_->host_bvhs[index].num_primitives() != 0;
                };

                two_array_ref<normal_list>    normals = make_two_array_ref(impl_->normals, 0, frame, has_prims_func);
                two_array_ref<normal_list>    shading_normals = make_two_array_ref(impl_->shading_normals, 0, frame, has_prims_func);
                two_array_ref<tex_coord_list> tex_coords = make_two_array_ref(impl_->tex_coords, 0, frame, has_prims_func);
                two_array_ref<material_list>  materials = make_two_array_ref(impl_->materials, 0, frame, has_prims_func);
                two_array_ref<color_list>     colors = make_two_array_ref(impl_->colors, 0, frame, has_prims_func);
                two_array_ref<texture_list>   texture_refs = make_two_array_ref(impl_->texture_refs, 0, frame, has_prims_func);

                auto &vparams = impl_->channel_viewing_params[cur_channel_];
                if (impl_->state->algo == Pathtracing)
                {
                    pathtrace_cpu(
                        &bvh_ref,
                        &bvh_ref + 1,
                        normals,
                        shading_normals,
                        tex_coords,
                        materials,
                        colors,
                        texture_refs,
                        impl_->host_area_lights.data(),
                        impl_->host_area_lights.data() + impl_->host_area_lights.size(),
                        bounces,
                        epsilon,
                        clear_color,
                        vec4(1.0f),
                        impl_->host_sched,
                        vparams.view_matrix,
                        vparams.proj_matrix,
                        this,
                        vparams.frame_num);
                }
                else
                {
                    render_cpu(
                        &bvh_ref,
                        &bvh_ref + 1,
                        normals,
                        shading_normals,
                        tex_coords,
                        materials,
                        colors,
                        texture_refs,
                        lights.data(),
                        lights.data() + lights.size(),
                        bounces,
                        epsilon,
                        clear_color,
                        vec4(1.0f),
                        impl_->host_sched,
                        vparams.view_matrix,
                        vparams.proj_matrix,
                        this,
                        impl_->state,
                        impl_->dev_state);
                }
            }

            if (impl_->dev_state->debug_mode && impl_->dev_state->show_bvh)
            {
                if (impl_->host_bvhs.size() > 0 && impl_->host_bvhs[0].num_primitives())
                {
                    if (impl_->outlines_initialized[0])
                        impl_->outlines[0].frame(vparams.view_matrix, vparams.proj_matrix);
                    else
                    {
                        impl_->outlines[0].init(impl_->host_bvhs[0]);
                        impl_->outlines_initialized[0] = true;
                    }
                }

                if (impl_->host_bvhs.size() > frame && impl_->host_bvhs[frame].num_primitives())
                {
                    if (impl_->outlines_initialized[frame])
                        impl_->outlines[frame].frame(vparams.view_matrix, vparams.proj_matrix);
                    else
                    {
                        impl_->outlines[frame].init(impl_->host_bvhs[frame]);
                        impl_->outlines_initialized[frame] = true;
                    }
                }
            }

            vparams.frame_num++;
        }

        impl_->multi_channel_drawer->update();
        impl_->multi_channel_drawer->swapFrame();

        // Finally update state variables. Call after any other updates!

        impl_->commit_state();

        impl_->total_frame_num++;
    }

    void renderer::begin_frame()
    {

    }

    void renderer::end_frame()
    {

    }

} // namespace visionaray
