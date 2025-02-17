//
//  TextEntityItem.h
//  libraries/entities/src
//
//  Created by Brad Hefta-Gaub on 12/4/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_TextEntityItem_h
#define hifi_TextEntityItem_h

#include "EntityItem.h"

class TextEntityItem : public EntityItem {
public:
    static EntityItemPointer factory(const EntityItemID& entityID, const EntityItemProperties& properties);

    TextEntityItem(const EntityItemID& entityItemID);

    ALLOW_INSTANTIATION // This class can be instantiated

    /// set dimensions in domain scale units (0.0 - 1.0) this will also reset radius appropriately
    virtual void setUnscaledDimensions(const glm::vec3& value) override;
    virtual ShapeType getShapeType() const override { return SHAPE_TYPE_BOX; }

    // methods for getting/setting all properties of an entity
    virtual EntityItemProperties getProperties(const EntityPropertyFlags& desiredProperties, bool allowEmptyDesiredProperties) const override;
    virtual bool setProperties(const EntityItemProperties& properties) override;

    virtual EntityPropertyFlags getEntityProperties(EncodeBitstreamParams& params) const override;

    virtual void appendSubclassData(OctreePacketData* packetData, EncodeBitstreamParams& params,
                                    EntityTreeElementExtraEncodeDataPointer modelTreeElementExtraEncodeData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount,
                                    OctreeElement::AppendState& appendState) const override;

    virtual int readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
                                                ReadBitstreamToTreeParams& args,
                                                EntityPropertyFlags& propertyFlags, bool overwriteLocalData,
                                                bool& somethingChanged) override;

    virtual bool supportsDetailedIntersection() const override { return true; }
    virtual bool findDetailedRayIntersection(const glm::vec3& origin, const glm::vec3& direction,
                         OctreeElementPointer& element, float& distance,
                         BoxFace& face, glm::vec3& surfaceNormal,
                         QVariantMap& extraInfo, bool precisionPicking) const override;
    virtual bool findDetailedParabolaIntersection(const glm::vec3& origin, const glm::vec3& velocity,
                         const glm::vec3& acceleration, OctreeElementPointer& element, float& parabolicDistance,
                         BoxFace& face, glm::vec3& surfaceNormal,
                         QVariantMap& extraInfo, bool precisionPicking) const override;

    static const QString DEFAULT_TEXT;
    void setText(const QString& value);
    QString getText() const;

    static const float DEFAULT_LINE_HEIGHT;
    void setLineHeight(float value);
    float getLineHeight() const;

    static const glm::u8vec3 DEFAULT_TEXT_COLOR;
    glm::u8vec3 getTextColor() const;
    void setTextColor(const glm::u8vec3& value);

    static const float DEFAULT_TEXT_ALPHA;
    float getTextAlpha() const;
    void setTextAlpha(float value);

    static const glm::u8vec3 DEFAULT_BACKGROUND_COLOR;
    glm::u8vec3 getBackgroundColor() const;
    void setBackgroundColor(const glm::u8vec3& value);

    float getBackgroundAlpha() const;
    void setBackgroundAlpha(float value);

    BillboardMode getBillboardMode() const;
    void setBillboardMode(BillboardMode value);

    static const float DEFAULT_MARGIN;
    float getLeftMargin() const;
    void setLeftMargin(float value);

    float getRightMargin() const;
    void setRightMargin(float value);

    float getTopMargin() const;
    void setTopMargin(float value);

    float getBottomMargin() const;
    void setBottomMargin(float value);

private:
    QString _text;
    float _lineHeight;
    glm::u8vec3 _textColor;
    float _textAlpha;
    glm::u8vec3 _backgroundColor;
    float _backgroundAlpha;
    BillboardMode _billboardMode;
    float _leftMargin;
    float _rightMargin;
    float _topMargin;
    float _bottomMargin;
};

#endif // hifi_TextEntityItem_h
