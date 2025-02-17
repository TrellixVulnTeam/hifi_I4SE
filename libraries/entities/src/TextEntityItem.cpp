//
//  TextEntityItem.cpp
//  libraries/entities/src
//
//  Created by Brad Hefta-Gaub on 12/4/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "TextEntityItem.h"

#include <glm/gtx/transform.hpp>

#include <QDebug>

#include <ByteCountCoding.h>
#include <GeometryUtil.h>

#include "EntityItemProperties.h"
#include "EntitiesLogging.h"
#include "EntityTree.h"
#include "EntityTreeElement.h"

const QString TextEntityItem::DEFAULT_TEXT("");
const float TextEntityItem::DEFAULT_LINE_HEIGHT = 0.1f;
const glm::u8vec3 TextEntityItem::DEFAULT_TEXT_COLOR = { 255, 255, 255 };
const float TextEntityItem::DEFAULT_TEXT_ALPHA = 1.0f;
const glm::u8vec3 TextEntityItem::DEFAULT_BACKGROUND_COLOR = { 0, 0, 0};
const float TextEntityItem::DEFAULT_MARGIN = 0.0f;

EntityItemPointer TextEntityItem::factory(const EntityItemID& entityID, const EntityItemProperties& properties) {
    EntityItemPointer entity(new TextEntityItem(entityID), [](EntityItem* ptr) { ptr->deleteLater(); });
    entity->setProperties(properties);
    return entity;
}

TextEntityItem::TextEntityItem(const EntityItemID& entityItemID) : EntityItem(entityItemID) {
    _type = EntityTypes::Text;
}

void TextEntityItem::setUnscaledDimensions(const glm::vec3& value) {
    const float TEXT_ENTITY_ITEM_FIXED_DEPTH = 0.01f;
    // NOTE: Text Entities always have a "depth" of 1cm.
    EntityItem::setUnscaledDimensions(glm::vec3(value.x, value.y, TEXT_ENTITY_ITEM_FIXED_DEPTH));
}

EntityItemProperties TextEntityItem::getProperties(const EntityPropertyFlags& desiredProperties, bool allowEmptyDesiredProperties) const {
    EntityItemProperties properties = EntityItem::getProperties(desiredProperties, allowEmptyDesiredProperties); // get the properties from our base class

    COPY_ENTITY_PROPERTY_TO_PROPERTIES(text, getText);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(lineHeight, getLineHeight);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(textColor, getTextColor);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(textAlpha, getTextAlpha);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(backgroundColor, getBackgroundColor);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(backgroundAlpha, getBackgroundAlpha);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(billboardMode, getBillboardMode);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(leftMargin, getLeftMargin);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(rightMargin, getRightMargin);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(topMargin, getTopMargin);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(bottomMargin, getBottomMargin);
    return properties;
}

bool TextEntityItem::setProperties(const EntityItemProperties& properties) {
    bool somethingChanged = false;
    somethingChanged = EntityItem::setProperties(properties); // set the properties in our base class

    SET_ENTITY_PROPERTY_FROM_PROPERTIES(text, setText);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(lineHeight, setLineHeight);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(textColor, setTextColor);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(textAlpha, setTextAlpha);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(backgroundColor, setBackgroundColor);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(backgroundAlpha, setBackgroundAlpha);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(billboardMode, setBillboardMode);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(leftMargin, setLeftMargin);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(rightMargin, setRightMargin);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(topMargin, setTopMargin);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(bottomMargin, setBottomMargin);

    if (somethingChanged) {
        bool wantDebug = false;
        if (wantDebug) {
            uint64_t now = usecTimestampNow();
            int elapsed = now - getLastEdited();
            qCDebug(entities) << "TextEntityItem::setProperties() AFTER update... edited AGO=" << elapsed <<
                    "now=" << now << " getLastEdited()=" << getLastEdited();
        }
        setLastEdited(properties._lastEdited);
    }
    
    return somethingChanged;
}

int TextEntityItem::readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead, 
                                                ReadBitstreamToTreeParams& args,
                                                EntityPropertyFlags& propertyFlags, bool overwriteLocalData,
                                                bool& somethingChanged) {

    int bytesRead = 0;
    const unsigned char* dataAt = data;

    READ_ENTITY_PROPERTY(PROP_TEXT, QString, setText);
    READ_ENTITY_PROPERTY(PROP_LINE_HEIGHT, float, setLineHeight);
    READ_ENTITY_PROPERTY(PROP_TEXT_COLOR, glm::u8vec3, setTextColor);
    READ_ENTITY_PROPERTY(PROP_TEXT_ALPHA, float, setTextAlpha);
    READ_ENTITY_PROPERTY(PROP_BACKGROUND_COLOR, glm::u8vec3, setBackgroundColor);
    READ_ENTITY_PROPERTY(PROP_BACKGROUND_ALPHA, float, setBackgroundAlpha);
    READ_ENTITY_PROPERTY(PROP_BILLBOARD_MODE, BillboardMode, setBillboardMode);
    READ_ENTITY_PROPERTY(PROP_LEFT_MARGIN, float, setLeftMargin);
    READ_ENTITY_PROPERTY(PROP_RIGHT_MARGIN, float, setRightMargin);
    READ_ENTITY_PROPERTY(PROP_TOP_MARGIN, float, setTopMargin);
    READ_ENTITY_PROPERTY(PROP_BOTTOM_MARGIN, float, setBottomMargin);
    
    return bytesRead;
}

EntityPropertyFlags TextEntityItem::getEntityProperties(EncodeBitstreamParams& params) const {
    EntityPropertyFlags requestedProperties = EntityItem::getEntityProperties(params);
    requestedProperties += PROP_TEXT;
    requestedProperties += PROP_LINE_HEIGHT;
    requestedProperties += PROP_TEXT_COLOR;
    requestedProperties += PROP_TEXT_ALPHA;
    requestedProperties += PROP_BACKGROUND_COLOR;
    requestedProperties += PROP_BACKGROUND_ALPHA;
    requestedProperties += PROP_BILLBOARD_MODE;
    requestedProperties += PROP_LEFT_MARGIN;
    requestedProperties += PROP_RIGHT_MARGIN;
    requestedProperties += PROP_TOP_MARGIN;
    requestedProperties += PROP_BOTTOM_MARGIN;
    return requestedProperties;
}

void TextEntityItem::appendSubclassData(OctreePacketData* packetData, EncodeBitstreamParams& params, 
                                    EntityTreeElementExtraEncodeDataPointer modelTreeElementExtraEncodeData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount, 
                                    OctreeElement::AppendState& appendState) const { 

    bool successPropertyFits = true;

    APPEND_ENTITY_PROPERTY(PROP_TEXT, getText());
    APPEND_ENTITY_PROPERTY(PROP_LINE_HEIGHT, getLineHeight());
    APPEND_ENTITY_PROPERTY(PROP_TEXT_COLOR, getTextColor());
    APPEND_ENTITY_PROPERTY(PROP_TEXT_ALPHA, getTextAlpha());
    APPEND_ENTITY_PROPERTY(PROP_BACKGROUND_COLOR, getBackgroundColor());
    APPEND_ENTITY_PROPERTY(PROP_BACKGROUND_ALPHA, getBackgroundAlpha());
    APPEND_ENTITY_PROPERTY(PROP_BILLBOARD_MODE, (uint32_t)getBillboardMode());
    APPEND_ENTITY_PROPERTY(PROP_LEFT_MARGIN, getLeftMargin());
    APPEND_ENTITY_PROPERTY(PROP_RIGHT_MARGIN, getRightMargin());
    APPEND_ENTITY_PROPERTY(PROP_TOP_MARGIN, getTopMargin());
    APPEND_ENTITY_PROPERTY(PROP_BOTTOM_MARGIN, getBottomMargin());
    
}

bool TextEntityItem::findDetailedRayIntersection(const glm::vec3& origin, const glm::vec3& direction,
                                                 OctreeElementPointer& element, float& distance,
                                                 BoxFace& face, glm::vec3& surfaceNormal,
                                                 QVariantMap& extraInfo, bool precisionPicking) const {
    glm::vec3 dimensions = getScaledDimensions();
    glm::vec2 xyDimensions(dimensions.x, dimensions.y);
    glm::quat rotation = getWorldOrientation();
    glm::vec3 position = getWorldPosition() + rotation * (dimensions * (ENTITY_ITEM_DEFAULT_REGISTRATION_POINT - getRegistrationPoint()));

    if (findRayRectangleIntersection(origin, direction, rotation, position, xyDimensions, distance)) {
        glm::vec3 forward = rotation * Vectors::FRONT;
        if (glm::dot(forward, direction) > 0.0f) {
            face = MAX_Z_FACE;
            surfaceNormal = -forward;
        } else {
            face = MIN_Z_FACE;
            surfaceNormal = forward;
        }
        return true;
    }
    return false;
}

bool TextEntityItem::findDetailedParabolaIntersection(const glm::vec3& origin, const glm::vec3& velocity, const glm::vec3& acceleration,
                                                      OctreeElementPointer& element, float& parabolicDistance,
                                                      BoxFace& face, glm::vec3& surfaceNormal,
                                                      QVariantMap& extraInfo, bool precisionPicking) const {
    glm::vec3 dimensions = getScaledDimensions();
    glm::vec2 xyDimensions(dimensions.x, dimensions.y);
    glm::quat rotation = getWorldOrientation();
    glm::vec3 position = getWorldPosition() + rotation * (dimensions * (ENTITY_ITEM_DEFAULT_REGISTRATION_POINT - getRegistrationPoint()));

    glm::quat inverseRot = glm::inverse(rotation);
    glm::vec3 localOrigin = inverseRot * (origin - position);
    glm::vec3 localVelocity = inverseRot * velocity;
    glm::vec3 localAcceleration = inverseRot * acceleration;

    if (findParabolaRectangleIntersection(localOrigin, localVelocity, localAcceleration, xyDimensions, parabolicDistance)) {
        float localIntersectionVelocityZ = localVelocity.z + localAcceleration.z * parabolicDistance;
        glm::vec3 forward = rotation * Vectors::FRONT;
        if (localIntersectionVelocityZ > 0.0f) {
            face = MIN_Z_FACE;
            surfaceNormal = forward;
        } else {
            face = MAX_Z_FACE;
            surfaceNormal = -forward;
        }
        return true;
    }
    return false;
}

void TextEntityItem::setText(const QString& value) {
    withWriteLock([&] {
        _text = value;
    });
}

QString TextEntityItem::getText() const { 
    QString result;
    withReadLock([&] {
        result = _text;
    });
    return result;
}

void TextEntityItem::setLineHeight(float value) { 
    withWriteLock([&] {
        _lineHeight = value;
    });
}

float TextEntityItem::getLineHeight() const { 
    float result;
    withReadLock([&] {
        result = _lineHeight;
    });
    return result;
}

void TextEntityItem::setTextColor(const glm::u8vec3& value) {
    withWriteLock([&] {
        _textColor = value;
    });
}

glm::u8vec3 TextEntityItem::getTextColor() const {
    return resultWithReadLock<glm::u8vec3>([&] {
        return _textColor;
    });
}

void TextEntityItem::setTextAlpha(float value) {
    withWriteLock([&] {
        _textAlpha = value;
    });
}

float TextEntityItem::getTextAlpha() const {
    return resultWithReadLock<float>([&] {
        return _textAlpha;
    });
}

void TextEntityItem::setBackgroundColor(const glm::u8vec3& value) {
    withWriteLock([&] {
        _backgroundColor = value;
    });
}

glm::u8vec3 TextEntityItem::getBackgroundColor() const {
    return resultWithReadLock<glm::u8vec3>([&] {
        return _backgroundColor;
    });
}

void TextEntityItem::setBackgroundAlpha(float value) {
    withWriteLock([&] {
        _backgroundAlpha = value;
    });
}

float TextEntityItem::getBackgroundAlpha() const {
    return resultWithReadLock<float>([&] {
        return _backgroundAlpha;
    });
}

BillboardMode TextEntityItem::getBillboardMode() const {
    BillboardMode result;
    withReadLock([&] {
        result = _billboardMode;
    });
    return result;
}

void TextEntityItem::setBillboardMode(BillboardMode value) {
    withWriteLock([&] {
        _billboardMode = value;
    });
}

void TextEntityItem::setLeftMargin(float value) {
    withWriteLock([&] {
        _leftMargin = value;
    });
}

float TextEntityItem::getLeftMargin() const {
    return resultWithReadLock<float>([&] {
        return _leftMargin;
    });
}

void TextEntityItem::setRightMargin(float value) {
    withWriteLock([&] {
        _rightMargin = value;
    });
}

float TextEntityItem::getRightMargin() const {
    return resultWithReadLock<float>([&] {
        return _rightMargin;
    });
}

void TextEntityItem::setTopMargin(float value) {
    withWriteLock([&] {
        _topMargin = value;
    });
}

float TextEntityItem::getTopMargin() const {
    return resultWithReadLock<float>([&] {
        return _topMargin;
    });
}

void TextEntityItem::setBottomMargin(float value) {
    withWriteLock([&] {
        _bottomMargin = value;
    });
}

float TextEntityItem::getBottomMargin() const {
    return resultWithReadLock<float>([&] {
        return _bottomMargin;
    });
}
