//
//  Created by Sam Gondelman on 11/29/18
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ImageEntityItem.h"

#include "EntityItemProperties.h"

EntityItemPointer ImageEntityItem::factory(const EntityItemID& entityID, const EntityItemProperties& properties) {
    Pointer entity(new ImageEntityItem(entityID), [](EntityItem* ptr) { ptr->deleteLater(); });
    entity->setProperties(properties);
    return entity;
}

// our non-pure virtual subclass for now...
ImageEntityItem::ImageEntityItem(const EntityItemID& entityItemID) : EntityItem(entityItemID) {
    _type = EntityTypes::Image;
}

void ImageEntityItem::setUnscaledDimensions(const glm::vec3& value) {
    const float IMAGE_ENTITY_ITEM_FIXED_DEPTH = 0.01f;
    // NOTE: Image Entities always have a "depth" of 1cm.
    EntityItem::setUnscaledDimensions(glm::vec3(value.x, value.y, IMAGE_ENTITY_ITEM_FIXED_DEPTH));
}

EntityItemProperties ImageEntityItem::getProperties(const EntityPropertyFlags& desiredProperties, bool allowEmptyDesiredProperties) const {
    EntityItemProperties properties = EntityItem::getProperties(desiredProperties, allowEmptyDesiredProperties); // get the properties from our base class

    COPY_ENTITY_PROPERTY_TO_PROPERTIES(color, getColor);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(alpha, getAlpha);

    COPY_ENTITY_PROPERTY_TO_PROPERTIES(imageURL, getImageURL);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(emissive, getEmissive);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(keepAspectRatio, getKeepAspectRatio);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(billboardMode, getBillboardMode);
    COPY_ENTITY_PROPERTY_TO_PROPERTIES(subImage, getSubImage);

    return properties;
}

bool ImageEntityItem::setProperties(const EntityItemProperties& properties) {
    bool somethingChanged = EntityItem::setProperties(properties); // set the properties in our base class

    SET_ENTITY_PROPERTY_FROM_PROPERTIES(color, setColor);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(alpha, setAlpha);

    SET_ENTITY_PROPERTY_FROM_PROPERTIES(imageURL, setImageURL);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(emissive, setEmissive);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(keepAspectRatio, setKeepAspectRatio);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(billboardMode, setBillboardMode);
    SET_ENTITY_PROPERTY_FROM_PROPERTIES(subImage, setSubImage);

    if (somethingChanged) {
        bool wantDebug = false;
        if (wantDebug) {
            uint64_t now = usecTimestampNow();
            int elapsed = now - getLastEdited();
            qCDebug(entities) << "ImageEntityItem::setProperties() AFTER update... edited AGO=" << elapsed <<
                    "now=" << now << " getLastEdited()=" << getLastEdited();
        }
        setLastEdited(properties.getLastEdited());
    }
    return somethingChanged;
}

int ImageEntityItem::readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
                                                ReadBitstreamToTreeParams& args,
                                                EntityPropertyFlags& propertyFlags, bool overwriteLocalData,
                                                bool& somethingChanged) {

    int bytesRead = 0;
    const unsigned char* dataAt = data;

    READ_ENTITY_PROPERTY(PROP_COLOR, u8vec3Color, setColor);
    READ_ENTITY_PROPERTY(PROP_ALPHA, float, setAlpha);

    READ_ENTITY_PROPERTY(PROP_IMAGE_URL, QString, setImageURL);
    READ_ENTITY_PROPERTY(PROP_EMISSIVE, bool, setEmissive);
    READ_ENTITY_PROPERTY(PROP_KEEP_ASPECT_RATIO, bool, setKeepAspectRatio);
    READ_ENTITY_PROPERTY(PROP_BILLBOARD_MODE, BillboardMode, setBillboardMode);
    READ_ENTITY_PROPERTY(PROP_SUB_IMAGE, QRect, setSubImage);

    return bytesRead;
}

EntityPropertyFlags ImageEntityItem::getEntityProperties(EncodeBitstreamParams& params) const {
    EntityPropertyFlags requestedProperties = EntityItem::getEntityProperties(params);

    requestedProperties += PROP_COLOR;
    requestedProperties += PROP_ALPHA;

    requestedProperties += PROP_IMAGE_URL;
    requestedProperties += PROP_EMISSIVE;
    requestedProperties += PROP_KEEP_ASPECT_RATIO;
    requestedProperties += PROP_BILLBOARD_MODE;
    requestedProperties += PROP_SUB_IMAGE;

    return requestedProperties;
}

void ImageEntityItem::appendSubclassData(OctreePacketData* packetData, EncodeBitstreamParams& params,
                                    EntityTreeElementExtraEncodeDataPointer modelTreeElementExtraEncodeData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount,
                                    OctreeElement::AppendState& appendState) const {

    bool successPropertyFits = true;

    APPEND_ENTITY_PROPERTY(PROP_COLOR, getColor());
    APPEND_ENTITY_PROPERTY(PROP_ALPHA, getAlpha());

    APPEND_ENTITY_PROPERTY(PROP_IMAGE_URL, getImageURL());
    APPEND_ENTITY_PROPERTY(PROP_EMISSIVE, getEmissive());
    APPEND_ENTITY_PROPERTY(PROP_KEEP_ASPECT_RATIO, getKeepAspectRatio());
    APPEND_ENTITY_PROPERTY(PROP_BILLBOARD_MODE, (uint32_t)getBillboardMode());
    APPEND_ENTITY_PROPERTY(PROP_SUB_IMAGE, getSubImage());
}

bool ImageEntityItem::findDetailedRayIntersection(const glm::vec3& origin, const glm::vec3& direction,
                                                   OctreeElementPointer& element,
                                                   float& distance, BoxFace& face, glm::vec3& surfaceNormal,
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

bool ImageEntityItem::findDetailedParabolaIntersection(const glm::vec3& origin, const glm::vec3& velocity, const glm::vec3& acceleration,
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

QString ImageEntityItem::getImageURL() const {
    QString result;
    withReadLock([&] {
        result = _imageURL;
    });
    return result;
}

void ImageEntityItem::setImageURL(const QString& url) {
    withWriteLock([&] {
        _imageURL = url;
    });
}

bool ImageEntityItem::getEmissive() const {
    bool result;
    withReadLock([&] {
        result = _emissive;
    });
    return result;
}

void ImageEntityItem::setEmissive(bool emissive) {
    withWriteLock([&] {
        _emissive = emissive;
    });
}

bool ImageEntityItem::getKeepAspectRatio() const {
    bool result;
    withReadLock([&] {
        result = _keepAspectRatio;
    });
    return result;
}

void ImageEntityItem::setKeepAspectRatio(bool keepAspectRatio) {
    withWriteLock([&] {
        _keepAspectRatio = keepAspectRatio;
    });
}

BillboardMode ImageEntityItem::getBillboardMode() const {
    BillboardMode result;
    withReadLock([&] {
        result = _billboardMode;
    });
    return result;
}

void ImageEntityItem::setBillboardMode(BillboardMode value) {
    withWriteLock([&] {
        _billboardMode = value;
    });
}

QRect ImageEntityItem::getSubImage() const {
    QRect result;
    withReadLock([&] {
        result = _subImage;
    });
    return result;
}

void ImageEntityItem::setSubImage(const QRect& subImage) {
    withWriteLock([&] {
        _subImage = subImage;
    });
}

void ImageEntityItem::setColor(const glm::u8vec3& color) {
    withWriteLock([&] {
        _color = color;
    });
}

glm::u8vec3 ImageEntityItem::getColor() const {
    return resultWithReadLock<glm::u8vec3>([&] {
        return _color;
    });
}

void ImageEntityItem::setAlpha(float alpha) {
    withWriteLock([&] {
        _alpha = alpha;
    });
}

float ImageEntityItem::getAlpha() const {
    return resultWithReadLock<float>([&] {
        return _alpha;
    });
}