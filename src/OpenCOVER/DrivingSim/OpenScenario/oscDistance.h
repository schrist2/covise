/* This file is part of COVISE.

You can use it under the terms of the GNU Lesser General Public License
version 2.1 or later, see lgpl-2.1.txt.

* License: LGPL 2+ */
#ifndef OSC_DISTANCE_H
#define OSC_DISTANCE_H
#include <oscExport.h>
#include <oscObjectBase.h>
#include <oscObjectVariable.h>
#include <oscVariables.h>
#include <oscObject.h>

namespace OpenScenario {

/// \class This class represents a generic OpenScenario Object
class OPENSCENARIOEXPORT oscDistance: public oscObject
{
public:
    oscDistance()
    {	
		OSC_ADD_MEMBER(freespace);
    };
	oscBool freespace;
};

typedef oscObjectVariable<oscDistance *> oscDistanceMember;

}

#endif //OSC_DISTANCE_H
