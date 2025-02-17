//
//  Created by Sam Gondelman on 11/29/18
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "GridEntityItem.h"

#include "EntityItemProperties.h"

const uint32_t GridEntityItem::DEFAULT_MAJOR_GRID_EVERY = 5;
const float GridEntityItem::DEFAULT_MINOR_GRID_EVERY = 1.0f;

EntityItemPointer GridEntityItem::factory(const EntityItemID& entityID, const EntityItemProperties& properties) {
    Pointer entity(new GridEntityItem(entityID), [](EntityItem* ptr) { ptr->deleteLater(); });
    entity->setProperties(properties);
    return entity;
}

// our non-pure virtual subclass for now...
GridEntityItem::GridEntityItem(const EntityItemID& entityItemID) : EntityItem(entityItemID) {
    _type = EntityTypes::Grid;
}

void GridEntityItem::setUnscaledDimensions(const glm::vec3& value) {
    const float GRID_ENTITY_ITEM_FIXED_DEPTH = 0.01f;
    // NOTE: Grid Entities always have a "depth" of 1cm.
    EntityItem::setUnscaledDimensions(glm::vec3(value.x, value.y, GRID_ENTITY_ITEM_FIXED_DEPTH));
}

EntityItemProperties GridEntityItem::getProperties(const EntityPropertyFlags& desiredProperties, bool allowEmptyDesiredProperties) const {
    EntityItemProperties properties = EntityItem::getProperties(desiredProperties, allowEmptyDesiredProperties); // get the properties from our base class

    COPY_ENTITY_PROPERTY_TO_PROPERTIES(color, getColor);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(alpha, getAlpha);

    COPY_ENTITY_PROPERTY_TO_PROPERTIES(followCamera, getFollowCamera);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(majorGridEvery, getMajorGridEvery);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(minorGridEvery, getMinorGridEvery);

    return properties;
}

bool GridEntityItem::setProperties(const EntityItemProperties& properties) {
    bool somethingChanged = EntityItem::setProperties(properties); // set the properties in our base class

    SET_ENTITY_PROPERTY_FROM_PROPERTIES(color, setColor);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(alpha, setAlpha);

    SET_ENTITY_PROPERTY_FROM_PROPERTIES(followCamera, setFollowCamera);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(majorGridEvery, setMajorGridEvery);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(minorGridEvery, setMinorGridEvery);

    if (somethingChanged) {
        bool wantDebug = false;
        if (wantDebug) {
            uint64_t now = usecTimestampNow();
            int elapsed = now - getLastEdited();
            qCDebug(entities) << "GridEntityItem::setProperties() AFTER update... edited AGO=" << elapsed <<
                    "now=" << now << " getLastEdited()=" << getLastEdited();
        }
        setLastEdited(properties.getLastEdited());
    }
    return somethingChanged;
}

int GridEntityItem::readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
                                                ReadBitstreamToTreeParams& args,
                                                EntityPropertyFlags& propertyFlags, bool overwriteLocalData,
                                                bool& somethingChanged) {

    int bytesRead = 0;
    const unsigned char* dataAt = data;

    READ_ENTITY_PROPERTY(PROP_COLOR, u8vec3Color, setColor);
    READ_ENTITY_PROPERTY(PROP_ALPHA, float, setAlpha);

    READ_ENTITY_PROPERTY(PROP_GRID_FOLLOW_CAMERA, bool, setFollowCamera);
    READ_ENTITY_PROPERTY(PROP_MAJOR_GRID_EVERY, uint32_t, setMajorGridEvery);
    READ_ENTITY_PROPERTY(PROP_MINOR_GRID_EVERY, float, setMinorGridEvery);

    return bytesRead;
}

EntityPropertyFlags GridEntityItem::getEntityProperties(EncodeBitstreamParams& params) const {
    EntityPropertyFlags requestedProperties = EntityItem::getEntityProperties(params);

    requestedProperties += PROP_COLOR;
    requestedProperties += PROP_ALPHA;

    requestedProperties += PROP_GRID_FOLLOW_CAMERA;
    requestedProperties += PROP_MAJOR_GRID_EVERY;
    requestedProperties += PROP_MINOR_GRID_EVERY;

    return requestedProperties;
}

void GridEntityItem::appendSubclassData(OctreePacketData* packetData, EncodeBitstreamParams& params,
                                    EntityTreeElementExtraEncodeDataPointer modelTreeElementExtraEncodeData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount,
                                    OctreeElement::AppendState& appendState) const {

    bool successPropertyFits = true;

    APPEND_ENTITY_PROPERTY(PROP_COLOR, getColor());
    APPEND_ENTITY_PROPERTY(PROP_ALPHA, getAlpha());

    APPEND_ENTITY_PROPERTY(PROP_GRID_FOLLOW_CAMERA, getFollowCamera());
    APPEND_ENTITY_PROPERTY(PROP_MAJOR_GRID_EVERY, getMajorGridEvery());
    APPEND_ENTITY_PROPERTY(PROP_MINOR_GRID_EVERY, getMinorGridEvery());
}

void GridEntityItem::setColor(const glm::u8vec3& color) {
    withWriteLock([&] {
        _color = color;
    });
}

glm::u8vec3 GridEntityItem::getColor() const {
    return resultWithReadLock<glm::u8vec3>([&] {
        return _color;
    });
}

void GridEntityItem::setAlpha(float alpha) {
    withWriteLock([&] {
        _alpha = alpha;
    });
}

float GridEntityItem::getAlpha() const {
    return resultWithReadLock<float>([&] {
        return _alpha;
    });
}

void GridEntityItem::setFollowCamera(bool followCamera) {
    withWriteLock([&] {
        _followCamera = followCamera;
    });
}

bool GridEntityItem::getFollowCamera() const {
    return resultWithReadLock<bool>([&] {
        return _followCamera;
    });
}

void GridEntityItem::setMajorGridEvery(uint32_t majorGridEvery) {
    withWriteLock([&] {
        const uint32_t MAJOR_GRID_EVERY_MIN = 1;
        _majorGridEvery = std::max(majorGridEvery, MAJOR_GRID_EVERY_MIN);
    });
}

uint32_t GridEntityItem::getMajorGridEvery() const {
    return resultWithReadLock<uint32_t>([&] {
        return _majorGridEvery;
    });
}

void GridEntityItem::setMinorGridEvery(float minorGridEvery) {
    withWriteLock([&] {
        const float MINOR_GRID_EVERY_MIN = 0.01f;
        _minorGridEvery = std::max(minorGridEvery, MINOR_GRID_EVERY_MIN);
    });
}

float GridEntityItem::getMinorGridEvery() const {
    return resultWithReadLock<float>([&] {
        return _minorGridEvery;
    });
}