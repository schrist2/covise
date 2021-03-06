/* This file is part of COVISE.

   You can use it under the terms of the GNU Lesser General Public License
   version 2.1 or later, see lgpl-2.1.txt.

 * License: LGPL 2+ */

/**************************************************************************
** ODD: OpenDRIVE Designer
**   Frank Naegele (c) 2010
**   <mail@f-naegele.de>
**   27.05.2010
**
**************************************************************************/

#ifndef MOVEHANDLE_HPP
#define MOVEHANDLE_HPP

#include "handle.hpp"

class MoveHandle : public Handle
{

    //################//
    // FUNCTIONS      //
    //################//

public:
    explicit MoveHandle(QGraphicsItem *parent);
    virtual ~MoveHandle();

protected:
private:
    MoveHandle(); /* not allowed */
    MoveHandle(const MoveHandle &); /* not allowed */
    MoveHandle &operator=(const MoveHandle &); /* not allowed */

    //################//
    // EVENTS         //
    //################//

protected:
    virtual void mousePressEvent(QGraphicsSceneMouseEvent *event);
    virtual void mouseReleaseEvent(QGraphicsSceneMouseEvent *event);
    virtual void mouseMoveEvent(QGraphicsSceneMouseEvent *event);

    virtual void hoverEnterEvent(QGraphicsSceneHoverEvent *event);
    virtual void hoverLeaveEvent(QGraphicsSceneHoverEvent *event);
    virtual void hoverMoveEvent(QGraphicsSceneHoverEvent *event);

    //################//
    // PROPERTIES     //
    //################//

protected:
private:
    //################//
    // STATIC         //
    //################//

private:
    // Path Template //
    //
    // this path will be shared by all handles
    static void createPath();
    static QPainterPath *pathTemplate_;
};

#endif // MOVEHANDLE_HPP
