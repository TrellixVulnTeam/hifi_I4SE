"use strict";

//  grab.js
//  examples
//
//  Created by Eric Levin on May 1, 2015
//  Copyright 2015 High Fidelity, Inc.
//
//  Grab's physically moveable entities with the mouse, by applying a spring force.
//
//  Updated November 22, 2016 by Philip Rosedale:  Add distance attenuation of grab effect
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

/* global MyAvatar, Entities, Script, HMD, Camera, Vec3, Reticle, Overlays, getEntityCustomData, Messages, Quat, Controller,
   isInEditMode, entityIsGrabbable, Picks, PickType, Pointers, unhighlightTargetEntity, DISPATCHER_PROPERTIES,
   entityIsGrabbable, entityIsEquipped, getMainTabletIDs
*/
/* jslint bitwise: true */

(function() { // BEGIN LOCAL_SCOPE

    Script.include("/~/system/libraries/utils.js");
    Script.include("/~/system/libraries/controllerDispatcherUtils.js");
var MAX_SOLID_ANGLE = 0.01; // objects that appear smaller than this can't be grabbed

var DELAY_FOR_30HZ = 33; // milliseconds

var ZERO_VEC3 = {
    x: 0,
    y: 0,
    z: 0
};
var IDENTITY_QUAT = {
    x: 0,
    y: 0,
    z: 0,
    w: 0
};

var DEFAULT_GRABBABLE_DATA = {
    grabbable: true,
    invertSolidWhileHeld: false
};


var ACTION_TTL = 10; // seconds

function getTag() {
    return "grab-" + MyAvatar.sessionUUID;
}

var DISTANCE_HOLDING_ACTION_TIMEFRAME = 0.1; // how quickly objects move to their new position
var DISTANCE_HOLDING_UNITY_MASS = 1200; //  The mass at which the distance holding action timeframe is unmodified
var DISTANCE_HOLDING_UNITY_DISTANCE = 6; //  The distance at which the distance holding action timeframe is unmodified

function distanceGrabTimescale(mass, distance) {
    var timeScale = DISTANCE_HOLDING_ACTION_TIMEFRAME * mass /
        DISTANCE_HOLDING_UNITY_MASS * distance /
        DISTANCE_HOLDING_UNITY_DISTANCE;
    if (timeScale < DISTANCE_HOLDING_ACTION_TIMEFRAME) {
        timeScale = DISTANCE_HOLDING_ACTION_TIMEFRAME;
    }
    return timeScale;
}
function getMass(dimensions, density) {
    return (dimensions.x * dimensions.y * dimensions.z) * density;
}

function entityIsGrabbedByOther(entityID) {
    // by convention, a distance grab sets the tag of its action to be grab-*owner-session-id*.
    var actionIDs = Entities.getActionIDs(entityID);
    for (var actionIndex = 0; actionIndex < actionIDs.length; actionIndex++) {
        var actionID = actionIDs[actionIndex];
        var actionArguments = Entities.getActionArguments(entityID, actionID);
        var tag = actionArguments.tag;
        if (tag == getTag()) {
            // we see a grab-*uuid* shaped tag, but it's our tag, so that's okay.
            continue;
        }
        if (tag.slice(0, 5) == "grab-") {
            // we see a grab-*uuid* shaped tag and it's not ours, so someone else is grabbing it.
            return true;
        }
    }
    return false;
}

// helper function
function mouseIntersectionWithPlane(pointOnPlane, planeNormal, event, maxDistance) {
    var cameraPosition = Camera.getPosition();
    var localPointOnPlane = Vec3.subtract(pointOnPlane, cameraPosition);
    var distanceFromPlane = Vec3.dot(localPointOnPlane, planeNormal);
    var MIN_DISTANCE_FROM_PLANE = 0.001;
    if (Math.abs(distanceFromPlane) < MIN_DISTANCE_FROM_PLANE) {
        // camera is touching the plane
        return pointOnPlane;
    }
    var pickRay = Camera.computePickRay(event.x, event.y);
    var dirDotNorm = Vec3.dot(pickRay.direction, planeNormal);
    var MIN_RAY_PLANE_DOT = 0.00001;

    var localIntersection;
    var useMaxForwardGrab = false;
    if (Math.abs(dirDotNorm) > MIN_RAY_PLANE_DOT) {
        var distanceToIntersection = distanceFromPlane / dirDotNorm;
        if (distanceToIntersection > 0 && distanceToIntersection < maxDistance) {
            // ray points into the plane
            localIntersection = Vec3.multiply(pickRay.direction, distanceFromPlane / dirDotNorm);
        } else {
            // ray intersects BEHIND the camera or else very far away
            // so we clamp the grab point to be the maximum forward position
            useMaxForwardGrab = true;
        }
    } else {
        // ray points perpendicular to grab plane
        // so we map the grab point to the maximum forward position
        useMaxForwardGrab = true;
    }
    if (useMaxForwardGrab) {
        // we re-route the intersection to be in front at max distance.
        var rayDirection = Vec3.subtract(pickRay.direction, Vec3.multiply(planeNormal, dirDotNorm));
        rayDirection = Vec3.normalize(rayDirection);
        localIntersection = Vec3.multiply(rayDirection, maxDistance);
        localIntersection = Vec3.sum(localIntersection, Vec3.multiply(planeNormal, distanceFromPlane));
    }
    var worldIntersection = Vec3.sum(cameraPosition, localIntersection);
    return worldIntersection;
}

// Mouse class stores mouse click and drag info
function Mouse() {
    this.current = {
        x: 0,
        y: 0
    };
    this.previous = {
        x: 0,
        y: 0
    };
    this.rotateStart = {
        x: 0,
        y: 0
    };
    this.cursorRestore = {
        x: 0,
        y: 0
    };
}

Mouse.prototype.startDrag = function(position) {
    this.current = {
        x: position.x,
        y: position.y
    };
    this.startRotateDrag();
};

Mouse.prototype.updateDrag = function(position) {
    this.current = {
        x: position.x,
        y: position.y
    };
};

Mouse.prototype.startRotateDrag = function() {
    this.previous = {
        x: this.current.x,
        y: this.current.y
    };
    this.rotateStart = {
        x: this.current.x,
        y: this.current.y
    };
    this.cursorRestore = Reticle.getPosition();
};

Mouse.prototype.getDrag = function() {
    var delta = {
        x: this.current.x - this.previous.x,
        y: this.current.y - this.previous.y
    };
    this.previous = {
        x: this.current.x,
        y: this.current.y
    };
    return delta;
};

Mouse.prototype.restoreRotateCursor = function() {
    Reticle.setPosition(this.cursorRestore);
    this.current = {
        x: this.rotateStart.x,
        y: this.rotateStart.y
    };
};

var mouse = new Mouse();

var beacon = {
    type: "cube",
    dimensions: {
        x: 0.01,
        y: 0,
        z: 0.01
    },
    color: {
        red: 200,
        green: 200,
        blue: 200
    },
    alpha: 1,
    solid: true,
    ignoreRayIntersection: true,
    visible: true
};

// TODO: play sounds again when we aren't leaking AudioInjector threads
// var grabSound = SoundCache.getSound("https://hifi-public.s3.amazonaws.com/eric/sounds/CloseClamp.wav");
// var releaseSound = SoundCache.getSound("https://hifi-public.s3.amazonaws.com/eric/sounds/ReleaseClamp.wav");
// var VOLUME = 0.0;


// Grabber class stores and computes info for grab behavior
function Grabber() {
    this.isGrabbing = false;
    this.entityID = null;
    this.actionID = null;
    this.startPosition = ZERO_VEC3;
    this.lastRotation = IDENTITY_QUAT;
    this.currentPosition = ZERO_VEC3;
    this.planeNormal = ZERO_VEC3;

    // maxDistance is a function of the size of the object.
    this.maxDistance = 0;

    // mode defines the degrees of freedom of the grab target positions
    // relative to startPosition options include:
    //     xzPlane  (default)
    //     verticalCylinder  (SHIFT)
    //     rotate  (CONTROL)
    this.mode = "xzplane";

    // offset allows the user to grab an object off-center.  It points from the object's center
    // to the point where the ray intersects the grab plane (at the moment the grab is initiated).
    // Future target positions of the ray intersection are on the same plane, and the offset is subtracted
    // to compute the target position of the object's center.
    this.offset = {
        x: 0,
        y: 0,
        z: 0
    };

    this.targetPosition = null;
    this.targetRotation = null;

    this.liftKey = false; // SHIFT
    this.rotateKey = false; // CONTROL

    this.mouseRayOverlays = Picks.createPick(PickType.Ray, {
        joint: "Mouse",
        filter: Picks.PICK_OVERLAYS | Picks.PICK_INCLUDE_NONCOLLIDABLE,
        enabled: true
    });
    var tabletItems = getMainTabletIDs();
    if (tabletItems.length > 0) {
        Picks.setIncludeItems(this.mouseRayOverlays, tabletItems);
    }
    var renderStates = [{name: "grabbed", end: beacon}];
    this.mouseRayEntities = Pointers.createPointer(PickType.Ray, {
        joint: "Mouse",
        filter: Picks.PICK_ENTITIES | Picks.PICK_INCLUDE_NONCOLLIDABLE,
        faceAvatar: true,
        scaleWithParent: true,
        enabled: true,
        renderStates: renderStates
    });
}

Grabber.prototype.computeNewGrabPlane = function() {
    if (!this.isGrabbing) {
        return;
    }

    var modeWasRotate = (this.mode == "rotate");
    this.mode = "xzPlane";
    this.planeNormal = {
        x: 0,
        y: 1,
        z: 0
    };
    if (this.rotateKey) {
        this.mode = "rotate";
        mouse.startRotateDrag();
    } else {
        if (modeWasRotate) {
            // we reset the mouse screen position whenever we stop rotating
            mouse.restoreRotateCursor();
        }
        if (this.liftKey) {
            this.mode = "verticalCylinder";
            // NOTE: during verticalCylinder mode a new planeNormal will be computed each move
        }
    }

    this.pointOnPlane = Vec3.sum(this.currentPosition, this.offset);
    var xzOffset = Vec3.subtract(this.pointOnPlane, Camera.getPosition());
    xzOffset.y = 0;
    this.xzDistanceToGrab = Vec3.length(xzOffset);
};

Grabber.prototype.pressEvent = function(event) {
    if (isInEditMode() || HMD.active) {
        return;
    }

    if (event.button !== "LEFT") {
        return;
    }

    if (event.isAlt || event.isMeta) {
        return;
    }

    if (Overlays.getOverlayAtPoint(Reticle.position) > 0) {
        // the mouse is pointing at an overlay; don't look for entities underneath the overlay.
        return;
    }

    var overlayResult = Picks.getPrevPickResult(this.mouseRayOverlays);
    if (overlayResult.type != Picks.INTERSECTED_NONE) {
        return;
    }

    var pickResults = Pointers.getPrevPickResult(this.mouseRayEntities);
    if (pickResults.type == Picks.INTERSECTED_NONE) {
        Pointers.setRenderState(this.mouseRayEntities, "");
        return;
    }

    var props = Entities.getEntityProperties(pickResults.objectID, DISPATCHER_PROPERTIES);
    var isDynamic = props.dynamic;
    if (!entityIsGrabbable(props)) {
        // only grab grabbable objects
        return;
    }

    if (!props.grab.grabbable) {
        return;
    }

    Pointers.setRenderState(this.mouseRayEntities, "grabbed");
    Pointers.setLockEndUUID(this.mouseRayEntities, pickResults.objectID, false);
    unhighlightTargetEntity(pickResults.objectID);

    mouse.startDrag(event);

    var clickedEntity = pickResults.objectID;
    var entityProperties = Entities.getEntityProperties(clickedEntity, DISPATCHER_PROPERTIES);
    this.startPosition = entityProperties.position;
    this.lastRotation = entityProperties.rotation;
    this.madeDynamic = false;
    var cameraPosition = Camera.getPosition();

    var objectBoundingDiameter = Vec3.length(entityProperties.dimensions);
    beacon.dimensions.y = objectBoundingDiameter;
    Pointers.editRenderState(this.mouseRayEntities, "grabbed", {end: beacon});
    this.maxDistance = objectBoundingDiameter / MAX_SOLID_ANGLE;
    if (Vec3.distance(this.startPosition, cameraPosition) > this.maxDistance) {
        // don't allow grabs of things far away
        return;
    }

    if (entityIsGrabbable(props) && !isDynamic) {
        entityProperties.dynamic = true;
        Entities.editEntity(clickedEntity, entityProperties);
        this.madeDynamic = true;
    }
    // this.activateEntity(clickedEntity, entityProperties);
    this.isGrabbing = true;

    this.entityID = clickedEntity;
    this.currentPosition = entityProperties.position;
    this.targetPosition = {
        x: this.startPosition.x,
        y: this.startPosition.y,
        z: this.startPosition.z
    };

    // compute the grab point
    var pickRay = Camera.computePickRay(event.x, event.y);
    var nearestPoint = Vec3.subtract(this.startPosition, cameraPosition);
    var distanceToGrab = Vec3.dot(nearestPoint, pickRay.direction);
    nearestPoint = Vec3.multiply(distanceToGrab, pickRay.direction);
    this.pointOnPlane = Vec3.sum(cameraPosition, nearestPoint);

    // compute the grab offset (points from object center to point of grab)
    this.offset = Vec3.subtract(this.pointOnPlane, this.startPosition);

    this.computeNewGrabPlane();

    if (!entityIsGrabbedByOther(this.entityID)) {
      this.moveEvent(event);
    }

    var args = "mouse";
    Entities.callEntityMethod(this.entityID, "startDistanceGrab", args);

    Messages.sendLocalMessage('Hifi-Object-Manipulation', JSON.stringify({
        action: 'grab',
        grabbedEntity: this.entityID
    }));

    // TODO: play sounds again when we aren't leaking AudioInjector threads
    //Audio.playSound(grabSound, { position: entityProperties.position, volume: VOLUME });
};

Grabber.prototype.releaseEvent = function(event) {
    if (event.button !== "LEFT" && !HMD.active) {
        return;
    }

    if (this.moveEventTimer) {
        Script.clearTimeout(this.moveEventTimer);
        this.moveEventTimer = null;
    }

    if (this.isGrabbing) {
        // this.deactivateEntity(this.entityID);
        this.isGrabbing = false;
        if (this.actionID) {
            Entities.deleteAction(this.entityID, this.actionID);
        }

        if (this.madeDynamic) {
            var entityProps = {};
            entityProps.dynamic = false;
            entityProps.localVelocity = {x: 0, y: 0, z: 0};
            Entities.editEntity(this.entityID, entityProps);
        }

        this.actionID = null;

        Pointers.setRenderState(this.mouseRayEntities, "");
        Pointers.setLockEndUUID(this.mouseRayEntities, null, false);

        var args = "mouse";
        Entities.callEntityMethod(this.entityID, "releaseGrab", args);

        Messages.sendLocalMessage('Hifi-Object-Manipulation', JSON.stringify({
            action: 'release',
            grabbedEntity: this.entityID,
            joint: "mouse"
        }));

        // TODO: play sounds again when we aren't leaking AudioInjector threads
        //Audio.playSound(releaseSound, { position: entityProperties.position, volume: VOLUME });
    }
};

Grabber.prototype.scheduleMouseMoveProcessor = function(event) {
    var _this = this;
    if (!this.moveEventTimer) {
        this.moveEventTimer = Script.setTimeout(function() {
            _this.moveEventProcess();
        }, DELAY_FOR_30HZ);
    }
};

Grabber.prototype.moveEvent = function(event) {
    // during the handling of the event, do as little as possible.  We save the updated mouse position,
    // and start a timer to react to the change.  If more changes arrive before the timer fires, only
    // the last update will be considered.  This is done to avoid backing-up Qt's event queue.
    if (!this.isGrabbing || HMD.active) {
        return;
    }
    mouse.updateDrag(event);
    this.scheduleMouseMoveProcessor();
};

Grabber.prototype.moveEventProcess = function() {
    this.moveEventTimer = null;
    // see if something added/restored gravity
    var entityProperties = Entities.getEntityProperties(this.entityID, DISPATCHER_PROPERTIES);
    if (!entityProperties || !entityProperties.gravity || HMD.active) {
        return;
    }

    if (Vec3.length(entityProperties.gravity) !== 0.0) {
        this.originalGravity = entityProperties.gravity;
    }
    this.currentPosition = entityProperties.position;
    this.mass = getMass(entityProperties.dimensions, entityProperties.density);
    var cameraPosition = Camera.getPosition();

    var actionArgs = {
        tag: getTag(),
        ttl: ACTION_TTL
    };

    if (this.mode === "rotate") {
        var drag = mouse.getDrag();
        var orientation = Camera.getOrientation();
        var dragOffset = Vec3.multiply(drag.x, Quat.getRight(orientation));
        dragOffset = Vec3.sum(dragOffset, Vec3.multiply(-drag.y, Quat.getUp(orientation)));
        var axis = Vec3.cross(dragOffset, Quat.getForward(orientation));
        axis = Vec3.normalize(axis);
        var ROTATE_STRENGTH = 0.4; // magic number tuned by hand
        var angle = ROTATE_STRENGTH * Math.sqrt((drag.x * drag.x) + (drag.y * drag.y));
        var deltaQ = Quat.angleAxis(angle, axis);
        // var qZero = entityProperties.rotation;
        //var qZero = this.lastRotation;
        this.lastRotation = Quat.multiply(deltaQ, this.lastRotation);

        var distanceToCameraR = Vec3.length(Vec3.subtract(this.currentPosition, cameraPosition));
        var angularTimeScale = distanceGrabTimescale(this.mass, distanceToCameraR);

        actionArgs = {
            targetRotation: this.lastRotation,
            angularTimeScale: angularTimeScale,
            tag: getTag(),
            ttl: ACTION_TTL
        };

    } else {
        var newPointOnPlane;

        if (this.mode === "verticalCylinder") {
            // for this mode we recompute the plane based on current Camera
            var planeNormal = Quat.getForward(Camera.getOrientation());
            planeNormal.y = 0;
            planeNormal = Vec3.normalize(planeNormal);
            var pointOnCylinder = Vec3.multiply(planeNormal, this.xzDistanceToGrab);
            pointOnCylinder = Vec3.sum(Camera.getPosition(), pointOnCylinder);
            this.pointOnPlane = mouseIntersectionWithPlane(pointOnCylinder, planeNormal, mouse.current, this.maxDistance);
            newPointOnPlane = {
                x: this.pointOnPlane.x,
                y: this.pointOnPlane.y,
                z: this.pointOnPlane.z
            };

        } else {

            newPointOnPlane = mouseIntersectionWithPlane(
                    this.pointOnPlane, this.planeNormal, mouse.current, this.maxDistance);
            var relativePosition = Vec3.subtract(newPointOnPlane, cameraPosition);
            var distance = Vec3.length(relativePosition);
            if (distance > this.maxDistance) {
                // clamp distance
                relativePosition = Vec3.multiply(relativePosition, this.maxDistance / distance);
                newPointOnPlane = Vec3.sum(relativePosition, cameraPosition);
            }
        }
        this.targetPosition = Vec3.subtract(newPointOnPlane, this.offset);

        var distanceToCameraL = Vec3.length(Vec3.subtract(this.targetPosition, cameraPosition));
        var linearTimeScale = distanceGrabTimescale(this.mass, distanceToCameraL);

        actionArgs = {
            targetPosition: this.targetPosition,
            linearTimeScale: linearTimeScale,
            tag: getTag(),
            ttl: ACTION_TTL
        };

    }

    if (!this.actionID) {
        if (!entityIsGrabbedByOther(this.entityID) && !entityIsEquipped(this.entityID)) {
            this.actionID = Entities.addAction("far-grab", this.entityID, actionArgs);
        }
    } else {
        Entities.updateAction(this.entityID, this.actionID, actionArgs);
    }

    this.scheduleMouseMoveProcessor();
};

Grabber.prototype.keyReleaseEvent = function(event) {
    if (event.text === "SHIFT") {
        this.liftKey = false;
    }
    if (event.text === "CONTROL") {
        this.rotateKey = false;
    }
    this.computeNewGrabPlane();
};

Grabber.prototype.keyPressEvent = function(event) {
    if (event.text === "SHIFT") {
        this.liftKey = true;
    }
    if (event.text === "CONTROL") {
        this.rotateKey = true;
    }
    this.computeNewGrabPlane();
};

Grabber.prototype.cleanup = function() {
    Pointers.removePointer(this.mouseRayEntities);
    Picks.removePick(this.mouseRayOverlays);
};

var grabber = new Grabber();

function pressEvent(event) {
    grabber.pressEvent(event);
}

function moveEvent(event) {
    grabber.moveEvent(event);
}

function releaseEvent(event) {
    grabber.releaseEvent(event);
}

function keyPressEvent(event) {
    grabber.keyPressEvent(event);
}

function keyReleaseEvent(event) {
    grabber.keyReleaseEvent(event);
}

function cleanup() {
    grabber.cleanup();
}

Controller.mousePressEvent.connect(pressEvent);
Controller.mouseMoveEvent.connect(moveEvent);
Controller.mouseReleaseEvent.connect(releaseEvent);
Controller.keyPressEvent.connect(keyPressEvent);
Controller.keyReleaseEvent.connect(keyReleaseEvent);
Script.scriptEnding.connect(cleanup);

}()); // END LOCAL_SCOPE
