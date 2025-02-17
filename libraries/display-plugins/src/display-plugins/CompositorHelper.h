//
//  Created by Bradley Austin Davis Arnold on 2015/06/13
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_display_plugins_Compositor_h
#define hifi_display_plugins_Compositor_h

#include <atomic>
#include <cstdint>

#include <QtCore/QObject>
#include <QtCore/QMutex>
#include <QtCore/QPropertyAnimation>
#include <QtGui/QCursor>
#include <QtGui/QMouseEvent>

#include <GLMHelpers.h>
#include <Transform.h>
#include <DependencyManager.h>

#include "DisplayPlugin.h"

class ReticleInterface;

const float DEFAULT_RETICLE_DEPTH = 1.0f; // FIXME - probably should be based on UI radius

const float MAGNIFY_WIDTH = 220.0f;
const float MAGNIFY_HEIGHT = 100.0f;
const float MAGNIFY_MULT = 2.0f;

// Handles the drawing of the overlays to the screen
// TODO, move divide up the rendering, displaying and input handling
// facilities of this class
class CompositorHelper : public QObject, public Dependency {
    Q_OBJECT

    Q_PROPERTY(float alpha READ getAlpha WRITE setAlpha NOTIFY alphaChanged)
    Q_PROPERTY(bool reticleOverDesktop READ getReticleOverDesktop WRITE setReticleOverDesktop)
public:
    static const uvec2 VIRTUAL_SCREEN_SIZE;
    static const QRect VIRTUAL_SCREEN_RECOMMENDED_OVERLAY_RECT;
    static const float VIRTUAL_UI_ASPECT_RATIO;
    static const vec2 VIRTUAL_UI_TARGET_FOV;
    static const vec2 MOUSE_EXTENTS_ANGULAR_SIZE;
    static const vec2 MOUSE_EXTENTS_PIXELS;

    CompositorHelper();
    void setRenderingWidget(QWidget* widget) { _renderingWidget = widget; }

    bool calculateRayUICollisionPoint(const glm::vec3& position, const glm::vec3& direction, glm::vec3& result) const;
    bool calculateParabolaUICollisionPoint(const glm::vec3& origin, const glm::vec3& velocity, const glm::vec3& acceleration, glm::vec3& result, float& parabolicDistance) const;

    bool isHMD() const;
    bool fakeEventActive() const { return _fakeMouseEvent; }

    // Converter from one frame of reference to another.
    // Frame of reference:
    // Spherical: Polar coordinates that gives the position on the sphere we project on (yaw,pitch)
    // Overlay: Position on the overlay (x,y)
    glm::vec2 sphericalToOverlay(const glm::vec2 & sphericalPos) const;
    glm::vec2 overlayToSpherical(const glm::vec2 & overlayPos) const;
    void computeHmdPickRay(const glm::vec2& cursorPos, glm::vec3& origin, glm::vec3& direction) const;

    glm::vec2 overlayFromSphereSurface(const glm::vec3& sphereSurfacePoint) const;
    glm::vec3 sphereSurfaceFromOverlay(const glm::vec2& overlay) const;

    void setCameraBaseTransform(const Transform& transform) { _cameraBaseTransform = transform; }
    const Transform& getCameraBaseTransform() const { return _cameraBaseTransform; }

    void setModelTransform(const Transform& transform) { _modelTransform = transform; }
    const Transform& getModelTransform() const { return _modelTransform; }
    glm::mat4 getUiTransform() const;

    float getAlpha() const { return _alpha; }
    void setAlpha(float alpha) { if (alpha != _alpha) { emit alphaChanged();  _alpha = alpha; } }

    bool getReticleVisible() const { return _reticleVisible; }
    void setReticleVisible(bool visible) { _reticleVisible = visible; }

    float getReticleDepth() const { return _reticleDepth; }
    void setReticleDepth(float depth) { _reticleDepth = depth; }
    void resetReticleDepth() { _reticleDepth = DEFAULT_RETICLE_DEPTH; }

    glm::vec2 getReticlePosition() const;
    void setReticlePosition(const glm::vec2& position, bool sendFakeEvent = true);

    glm::vec2 getReticleMaximumPosition() const;

    glm::mat4 getReticleTransform(const glm::mat4& eyePose = glm::mat4(), const glm::vec3& headPosition = glm::vec3()) const;
    glm::mat4 getPoint2DTransform(const glm::vec2& point, float sizeX , float sizeY) const;

    ReticleInterface* getReticleInterface() { return _reticleInterface; }

    /// return value - true means the caller should not process the event further
    bool handleRealMouseMoveEvent(bool sendFakeEvent = true);
    void handleLeaveEvent();
    QPointF getMouseEventPosition(QMouseEvent* event);

    bool shouldCaptureMouse() const;

    bool getAllowMouseCapture() const { return _allowMouseCapture; }
    void setAllowMouseCapture(bool capture);

    /// if the reticle is pointing to a system overlay (a dialog box for example) then the function returns true otherwise false
    bool getReticleOverDesktop() const;
    void setReticleOverDesktop(bool value) { _isOverDesktop = value; }

    void setDisplayPlugin(const DisplayPluginPointer& displayPlugin) { _currentDisplayPlugin = displayPlugin; }
    void setFrameInfo(uint32_t frame, const glm::mat4& camera, const glm::mat4& sensorToWorldMatrix) {
        _currentCamera = camera;
        _sensorToWorldMatrix = sensorToWorldMatrix;
    }

signals:
    void allowMouseCaptureChanged();
    void alphaChanged();

protected slots:
    void sendFakeMouseEvent();

private:
    void updateTooltips();

    DisplayPluginPointer _currentDisplayPlugin;
    glm::mat4 _currentCamera;
    glm::mat4 _sensorToWorldMatrix;
    QWidget* _renderingWidget{ nullptr };

    //// Support for hovering and tooltips
    //static EntityItemID _noItemId;
    //EntityItemID _hoverItemId { _noItemId };

    //QString _hoverItemTitle;
    //QString _hoverItemDescription;
    //quint64 _hoverItemEnterUsecs { 0 };

    bool _isOverDesktop { true };
    float _textureFov { VIRTUAL_UI_TARGET_FOV.y };
    float _textureAspectRatio { VIRTUAL_UI_ASPECT_RATIO };

    float _alpha { 1.0f };

    int _previousBorderWidth { -1 };
    int _previousBorderHeight { -1 };

    Transform _modelTransform;
    Transform _cameraBaseTransform;

    std::unique_ptr<QPropertyAnimation> _alphaPropertyAnimation;

    std::atomic<bool> _reticleVisible { true };
    std::atomic<float> _reticleDepth { 1.0f };

    // NOTE: when the compositor is running in HMD mode, it will control the reticle position as a custom
    // application specific position, when it's in desktop mode, the reticle position will simply move
    // the system mouse.
    glm::vec2 _reticlePositionInHMD { 0.0f, 0.0f };
    mutable QMutex _reticleLock { QMutex::Recursive };

    QPointF _lastKnownRealMouse;
    bool _ignoreMouseMove { false };

    bool _reticleOverQml { false };

    std::atomic<bool> _allowMouseCapture { true };

    bool _fakeMouseEvent { false };

    ReticleInterface* _reticleInterface { nullptr };
};

/**jsdoc
 * @namespace Reticle
 *
 * @hifi-interface
 * @hifi-client-entity
 *
 * @property {boolean} allowMouseCapture
 * @property {number} depth
 * @property {Vec2} maximumPosition
 * @property {boolean} mouseCaptured
 * @property {boolean} pointingAtSystemOverlay
 * @property {Vec2} position
 * @property {number} scale
 * @property {boolean} visible
 */
// Scripting interface available to control the Reticle
class ReticleInterface : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariant position READ getPosition WRITE setPosition)
    Q_PROPERTY(bool visible READ getVisible WRITE setVisible)
    Q_PROPERTY(float depth READ getDepth WRITE setDepth)
    Q_PROPERTY(float scale READ getScale WRITE setScale)
    Q_PROPERTY(glm::vec2 maximumPosition READ getMaximumPosition)
    Q_PROPERTY(bool mouseCaptured READ isMouseCaptured)
    Q_PROPERTY(bool allowMouseCapture READ getAllowMouseCapture WRITE setAllowMouseCapture)
    Q_PROPERTY(bool pointingAtSystemOverlay READ isPointingAtSystemOverlay)

public:
    ReticleInterface(CompositorHelper* outer) : QObject(outer), _compositor(outer) {}

    /**jsdoc
     * @function Reticle.isMouseCaptured
     * @returns {boolean}
     */
    Q_INVOKABLE bool isMouseCaptured() { return _compositor->shouldCaptureMouse(); }

    /**jsdoc
     * @function Reticle.getAllowMouseCapture
     * @returns {boolean}
     */
    Q_INVOKABLE bool getAllowMouseCapture() { return _compositor->getAllowMouseCapture(); }

    /**jsdoc
     * @function Reticle.setAllowMouseCapture
     * @param {boolean} allowMouseCaptured
     */
    Q_INVOKABLE void setAllowMouseCapture(bool value) { return _compositor->setAllowMouseCapture(value); }

    /**jsdoc
     * @function Reticle.isPointingAtSystemOverlay
     * @returns {boolean}
     */
    Q_INVOKABLE bool isPointingAtSystemOverlay() { return !_compositor->getReticleOverDesktop(); }

    /**jsdoc
     * @function Reticle.getVisible
     * @returns {boolean}
     */
    Q_INVOKABLE bool getVisible() { return _compositor->getReticleVisible(); }

    /**jsdoc
     * @function Reticle.setVisible
     * @param {boolean} visible
     */
    Q_INVOKABLE void setVisible(bool visible) { _compositor->setReticleVisible(visible); }

    /**jsdoc
     * @function Reticle.getDepth
     * @returns {number}
     */
    Q_INVOKABLE float getDepth() { return _compositor->getReticleDepth(); }

    /**jsdoc
     * @function Reticle.setDepth
     * @param {number} depth
     */
    Q_INVOKABLE void setDepth(float depth) { _compositor->setReticleDepth(depth); }

    /**jsdoc
     * @function Reticle.getScale
     * @returns {number}
     */
    Q_INVOKABLE float getScale() const;

    /**jsdoc
     * @function Reticle.setScale
     * @param {number} scale
     */
    Q_INVOKABLE void setScale(float scale);

    /**jsdoc
     * @function Reticle.getPosition
     * @returns {Vec2}
     */
    Q_INVOKABLE QVariant getPosition() const;

    /**jsdoc
     * @function Reticle.setPosition
     * @param {Vec2} position
     */
    Q_INVOKABLE void setPosition(QVariant position);

    /**jsdoc
     * @function Reticle.getMaximumPosition
     * @returns {Vec2}
     */
    Q_INVOKABLE glm::vec2 getMaximumPosition() { return _compositor->getReticleMaximumPosition(); }

private:
    CompositorHelper* _compositor;
};

#endif // hifi_CompositorHelper_h
