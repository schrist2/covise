/* This file is part of COVISE.

   You can use it under the terms of the GNU Lesser General Public License
   version 2.1 or later, see lgpl-2.1.txt.

 * License: LGPL 2+ */

#include <cover/coVRLighting.h>

#include "scene_monitor.h"

namespace scene
{

//-------------------------------------------------------------------------------------------------
// Material observable
//

Material::Material(osg::Material *om, material_list &vm, size_t idx)
    : osg_mat_(om)
    , vsnray_mats_(vm)
    , index_(idx)
    , ca_(om->getAmbient(osg::Material::Face::FRONT))
    , cd_(om->getDiffuse(osg::Material::Face::FRONT))
    , cs_(om->getSpecular(osg::Material::Face::FRONT))
    , ce_(om->getEmission(osg::Material::Face::FRONT))
    , shininess_(om->getShininess(osg::Material::Face::FRONT))
    , specular_(opencover::coVRLighting::instance()->specularlightState)
{
}

bool Material::changed()
{
    auto ca = osg_mat_->getAmbient(osg::Material::Face::FRONT);
    auto cd = osg_mat_->getDiffuse(osg::Material::Face::FRONT);
    auto cs = osg_mat_->getSpecular(osg::Material::Face::FRONT);
    auto ce = osg_mat_->getEmission(osg::Material::Face::FRONT);
    auto sh = osg_mat_->getShininess(osg::Material::Face::FRONT);
    bool sp = opencover::coVRLighting::instance()->specularlightState;

    return ca_ != ca || cd_ != cd || cs_ != cs || ce_ != ce || shininess_ != sh || specular_ != sp;
}

void Material::visit()
{
    auto ca = osg_mat_->getAmbient(osg::Material::Face::FRONT);
    auto cd = osg_mat_->getDiffuse(osg::Material::Face::FRONT);
    auto cs = osg_mat_->getSpecular(osg::Material::Face::FRONT);
    auto ce = osg_mat_->getEmission(osg::Material::Face::FRONT);
    auto sh = osg_mat_->getShininess(osg::Material::Face::FRONT);
    bool sp = opencover::coVRLighting::instance()->specularlightState;

    vsnray_mats_[index_] = osg_cast(osg_mat_, sp);
    ca_ = ca;
    cd_ = cd;
    cs_ = cs;
    ce_ = ce;
    shininess_ = sh;
    specular_ = sp;
}

//-------------------------------------------------------------------------------------------------
// Geometry observable
//

Transform::Transform(std::vector<osg::Transform*> transform_nodes_list, visionaray::index_bvh<host_bvh_type::bvh_inst>& top_level_bvh, size_t bvh_index)
    : transform_nodes_list_(transform_nodes_list),
    top_level_bvh_(top_level_bvh),
    bvh_index_(bvh_index)
{
}

bool Transform::changed()
{
    if (top_level_bvh_.num_primitives() <= bvh_index_) {
        return false;
    }

    current_transform_ = visionaray::mat4::identity();
    for (auto it = transform_nodes_list_.begin(); it != transform_nodes_list_.end(); it++) {
        current_transform_ = current_transform_ * osg_cast((*it)->asMatrixTransform()->getMatrix());
    }

    return top_level_bvh_.primitive(bvh_index_).transform_inv() != visionaray::inverse(current_transform_);
}

void Transform::visit()
{
    if (top_level_bvh_.num_primitives() <= bvh_index_) {
        return;
    }
    
    top_level_bvh_.primitive(bvh_index_).set_transform_inv(visionaray::inverse(current_transform_));
}

//-------------------------------------------------------------------------------------------------
// Monitor
//

void Monitor::add_observable(std::shared_ptr<Observable> obs)
{
    observables_.push_back(obs);
}

void Monitor::update()
{
    need_clear_frame_ = false;
    update_bits_ = 0;

    for (auto &o: observables_)
    {
        if (o->changed())
        {
            // TODO: merge into bits!?
            need_clear_frame_ = true;

            // TODO: nicer implementation
            if (std::dynamic_pointer_cast<Material>(o) != nullptr)
                update_bits_ |= UpdateMaterials;
            else if (std::dynamic_pointer_cast<Transform>(o) != nullptr)
                update_bits_ |= UpdateTransforms;

            o->visit();
        }
    }
}

bool Monitor::need_clear_frame()
{
    return need_clear_frame_;
}

unsigned Monitor::update_bits()
{
    return update_bits_;
}

}
