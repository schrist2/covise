/* This file is part of COVISE.

   You can use it under the terms of the GNU Lesser General Public License
   version 2.1 or later, see lgpl-2.1.txt.

 * License: LGPL 2+ */

#pragma once

#include <memory>
#include <vector>

#include "common.h"

namespace scene
{
    class Observable
    {
    public:
        virtual bool changed() = 0;
        virtual void visit() = 0;
    };

    //-----------------------------------------------------

    class Material : public Observable
    {
    public:
        Material(osg::Material *om, material_list &vm, size_t idx);

        bool changed();
        void visit();
    private:
        // Monitored osg material
        osg::Material *osg_mat_;

        // List of visionaray materials
        material_list &vsnray_mats_;

        // Index to the list of visionaray materials
        size_t index_;

        // Current material attributes
        osg::Vec4 ca_;
        osg::Vec4 cd_;
        osg::Vec4 cs_;
        osg::Vec4 ce_;
        float shininess_;

        // Specular light state
        bool specular_;
    };

    //-----------------------------------------------------

    class Transform : public Observable
    {
    public:
        Transform(std::vector<osg::Transform*> transform_nodes_list, visionaray::index_bvh<host_bvh_type::bvh_inst>& top_level_bvh, size_t bvh_index);

        bool changed();
        void visit();
    private:
        std::vector<osg::Transform*> transform_nodes_list_;
        visionaray::index_bvh<host_bvh_type::bvh_inst>& top_level_bvh_;
        size_t bvh_index_;
        visionaray::mat4 current_transform_ = visionaray::mat4::identity();
    };

    //-----------------------------------------------------

    class Monitor
    {
    public:

        // Flags that indicate what scene data changed
        static const unsigned UpdateGeometry   =  1;
        static const unsigned UpdateNormals    =  2;
        static const unsigned UpdateTexCoords  =  4;
        static const unsigned UpdateColors     =  8;
        static const unsigned UpdateMaterials  = 16;
        static const unsigned UpdateTextures   = 32;
        static const unsigned UpdateAreaLights = 64;
        static const unsigned UpdateTransforms = 128;

    public:

        void add_observable(std::shared_ptr<Observable> obs);
        void update();

        bool need_clear_frame();

        // Return a bit field that indicates what scene data changed
        unsigned update_bits();

    private:

        std::vector<std::shared_ptr<Observable> > observables_;

        bool need_clear_frame_ = true;
        unsigned update_bits_ = 0;
    };
}
