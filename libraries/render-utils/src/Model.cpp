//
//  Model.cpp
//  interface/src/renderer
//
//  Created by Andrzej Kapolka on 10/18/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "Model.h"

#include <QMetaType>
#include <QRunnable>
#include <QThreadPool>

#include <glm/gtx/transform.hpp>
#include <glm/gtx/norm.hpp>

#include <shared/QtHelpers.h>
#include <GeometryUtil.h>
#include <PathUtils.h>
#include <PerfStat.h>
#include <ViewFrustum.h>
#include <GLMHelpers.h>
#include <TBBHelpers.h>

#include <model-networking/SimpleMeshProxy.h>
#include <graphics-scripting/Forward.h>
#include <graphics/BufferViewHelpers.h>
#include <DualQuaternion.h>

#include <glm/gtc/packing.hpp>

#include "AbstractViewStateInterface.h"
#include "MeshPartPayload.h"

#include "RenderUtilsLogging.h"
#include <Trace.h>

using namespace std;

int nakedModelPointerTypeId = qRegisterMetaType<ModelPointer>();
int weakGeometryResourceBridgePointerTypeId = qRegisterMetaType<Geometry::WeakPointer>();
int vec3VectorTypeId = qRegisterMetaType<QVector<glm::vec3>>();
int normalTypeVecTypeId = qRegisterMetaType<QVector<NormalType>>("QVector<NormalType>");
float Model::FAKE_DIMENSION_PLACEHOLDER = -1.0f;
#define HTTP_INVALID_COM "http://invalid.com"

Model::Model(QObject* parent, SpatiallyNestable* spatiallyNestableOverride) :
    QObject(parent),
    _renderGeometry(),
    _renderWatcher(_renderGeometry),
    _spatiallyNestableOverride(spatiallyNestableOverride),
    _translation(0.0f),
    _rotation(),
    _scale(1.0f, 1.0f, 1.0f),
    _scaleToFit(false),
    _scaleToFitDimensions(1.0f),
    _scaledToFit(false),
    _snapModelToRegistrationPoint(false),
    _snappedToRegistrationPoint(false),
    _url(HTTP_INVALID_COM),
    _isWireframe(false),
    _renderItemKeyGlobalFlags(render::ItemKey::Builder().withVisible().withTagBits(render::hifi::TAG_ALL_VIEWS).build())
{
    // we may have been created in the network thread, but we live in the main thread
    if (_viewState) {
        moveToThread(_viewState->getMainThread());
    }

    setSnapModelToRegistrationPoint(true, glm::vec3(0.5f));

    connect(&_renderWatcher, &GeometryResourceWatcher::finished, this, &Model::loadURLFinished);
}

Model::~Model() {
    deleteGeometry();
}

AbstractViewStateInterface* Model::_viewState = NULL;

bool Model::needsFixupInScene() const {
    return (_needsFixupInScene || !_addedToScene) && !_needsReload && isLoaded();
}

void Model::setTranslation(const glm::vec3& translation) {
    _translation = translation;
    updateRenderItems();
}

void Model::setRotation(const glm::quat& rotation) {
    _rotation = rotation;
    updateRenderItems();
}

// temporary HACK: set transform while avoiding implicit calls to updateRenderItems()
// TODO: make setRotation() and friends set flag to be used later to decide to updateRenderItems()
void Model::setTransformNoUpdateRenderItems(const Transform& transform) {
    _translation = transform.getTranslation();
    _rotation = transform.getRotation();
    // DO NOT call updateRenderItems() here!
}

Transform Model::getTransform() const {
    if (_overrideModelTransform) {
        Transform transform;
        transform.setTranslation(getOverrideTranslation());
        transform.setRotation(getOverrideRotation());
        transform.setScale(getScale());
        return transform;
    } else if (_spatiallyNestableOverride) {
        bool success;
        Transform transform = _spatiallyNestableOverride->getTransform(success);
        if (success) {
            transform.setScale(getScale());
            return transform;
        }
    }

    Transform transform;
    transform.setScale(getScale());
    transform.setTranslation(getTranslation());
    transform.setRotation(getRotation());
    return transform;
}

void Model::setScale(const glm::vec3& scale) {
    setScaleInternal(scale);
    // if anyone sets scale manually, then we are no longer scaled to fit
    _scaleToFit = false;
    _scaledToFit = false;
}

const float SCALE_CHANGE_EPSILON = 0.0000001f;

void Model::setScaleInternal(const glm::vec3& scale) {
    if (glm::distance(_scale, scale) > SCALE_CHANGE_EPSILON) {
        _scale = scale;
        assert(_scale.x != 0.0f && scale.y != 0.0f && scale.z != 0.0f);
        simulate(0.0f, true);
    }
}

void Model::setOffset(const glm::vec3& offset) {
    _offset = offset;

    // if someone manually sets our offset, then we are no longer snapped to center
    _snapModelToRegistrationPoint = false;
    _snappedToRegistrationPoint = false;
}

void Model::calculateTextureInfo() {
    if (!_hasCalculatedTextureInfo && isLoaded() && getGeometry()->areTexturesLoaded() && !_modelMeshRenderItemsMap.isEmpty()) {
        size_t textureSize = 0;
        int textureCount = 0;
        bool allTexturesLoaded = true;
        foreach(auto renderItem, _modelMeshRenderItems) {
            auto meshPart = renderItem.get();
            textureSize += meshPart->getMaterialTextureSize();
            textureCount += meshPart->getMaterialTextureCount();
            allTexturesLoaded = allTexturesLoaded & meshPart->hasTextureInfo();
        }
        _renderInfoTextureSize = textureSize;
        _renderInfoTextureCount = textureCount;
        _hasCalculatedTextureInfo = allTexturesLoaded; // only do this once
    }
}

size_t Model::getRenderInfoTextureSize() {
    calculateTextureInfo();
    return _renderInfoTextureSize;
}

int Model::getRenderInfoTextureCount() {
    calculateTextureInfo();
    return _renderInfoTextureCount;
}

bool Model::shouldInvalidatePayloadShapeKey(int meshIndex) {
    if (!getGeometry()) {
        return true;
    }

    const HFMModel& hfmModel = getHFMModel();
    const auto& networkMeshes = getGeometry()->getMeshes();
    // if our index is ever out of range for either meshes or networkMeshes, then skip it, and set our _meshGroupsKnown
    // to false to rebuild out mesh groups.
    if (meshIndex < 0 || meshIndex >= (int)networkMeshes.size() || meshIndex >= (int)hfmModel.meshes.size() || meshIndex >= (int)_meshStates.size()) {
        _needsFixupInScene = true;     // trigger remove/add cycle
        invalidCalculatedMeshBoxes();  // if we have to reload, we need to assume our mesh boxes are all invalid
        return true;
    }

    return false;
}

void Model::updateRenderItems() {
    if (!_addedToScene) {
        return;
    }

    _needsUpdateClusterMatrices = true;
    _renderItemsNeedUpdate = false;

    // queue up this work for later processing, at the end of update and just before rendering.
    // the application will ensure only the last lambda is actually invoked.
    void* key = (void*)this;
    std::weak_ptr<Model> weakSelf = shared_from_this();
    AbstractViewStateInterface::instance()->pushPostUpdateLambda(key, [weakSelf]() {

        // do nothing, if the model has already been destroyed.
        auto self = weakSelf.lock();
        if (!self || !self->isLoaded()) {
            return;
        }

        // lazy update of cluster matrices used for rendering.
        // We need to update them here so we can correctly update the bounding box.
        self->updateClusterMatrices();

        Transform modelTransform = self->getTransform();
        modelTransform.setScale(glm::vec3(1.0f));

        bool isWireframe = self->isWireframe();
        auto renderItemKeyGlobalFlags = self->getRenderItemKeyGlobalFlags();

        render::Transaction transaction;
        for (int i = 0; i < (int) self->_modelMeshRenderItemIDs.size(); i++) {

            auto itemID = self->_modelMeshRenderItemIDs[i];
            auto meshIndex = self->_modelMeshRenderItemShapes[i].meshIndex;

            const auto& meshState = self->getMeshState(meshIndex);

            bool invalidatePayloadShapeKey = self->shouldInvalidatePayloadShapeKey(meshIndex);
            bool useDualQuaternionSkinning = self->getUseDualQuaternionSkinning();

            transaction.updateItem<ModelMeshPartPayload>(itemID, [modelTransform, meshState, useDualQuaternionSkinning,
                                                                  invalidatePayloadShapeKey, isWireframe, renderItemKeyGlobalFlags](ModelMeshPartPayload& data) {
                if (useDualQuaternionSkinning) {
                    data.updateClusterBuffer(meshState.clusterDualQuaternions);
                } else {
                    data.updateClusterBuffer(meshState.clusterMatrices);
                }

                Transform renderTransform = modelTransform;

                if (useDualQuaternionSkinning) {
                    if (meshState.clusterDualQuaternions.size() == 1) {
                        const auto& dq = meshState.clusterDualQuaternions[0];
                        Transform transform(dq.getRotation(),
                                            dq.getScale(),
                                            dq.getTranslation());
                        renderTransform = modelTransform.worldTransform(Transform(transform));
                    }
                } else {
                    if (meshState.clusterMatrices.size() == 1) {
                        renderTransform = modelTransform.worldTransform(Transform(meshState.clusterMatrices[0]));
                    }
                }
                data.updateTransformForSkinnedMesh(renderTransform, modelTransform);

                data.updateKey(renderItemKeyGlobalFlags);
                data.setShapeKey(invalidatePayloadShapeKey, isWireframe, useDualQuaternionSkinning);
            });
        }

        AbstractViewStateInterface::instance()->getMain3DScene()->enqueueTransaction(transaction);
    });
}

void Model::setRenderItemsNeedUpdate() {
    _renderItemsNeedUpdate = true;
    emit requestRenderUpdate();
}

void Model::reset() {
    if (isLoaded()) {
        const HFMModel& hfmModel = getHFMModel();
        _rig.reset(hfmModel);
        emit rigReset();
        emit rigReady();
    }
}

bool Model::updateGeometry() {
    bool needFullUpdate = false;

    if (!isLoaded()) {
        return false;
    }

    _needsReload = false;

    // TODO: should all Models have a valid _rig?
    if (_rig.jointStatesEmpty() && getHFMModel().joints.size() > 0) {
        initJointStates();
        assert(_meshStates.empty());

        const HFMModel& hfmModel = getHFMModel();
        int i = 0;
        foreach (const HFMMesh& mesh, hfmModel.meshes) {
            MeshState state;
            state.clusterDualQuaternions.resize(mesh.clusters.size());
            state.clusterMatrices.resize(mesh.clusters.size());
            _meshStates.push_back(state);
            initializeBlendshapes(mesh, i);
            i++;
        }
        _blendshapeOffsetsInitialized = true;
        needFullUpdate = true;
        emit rigReady();
    }

    return needFullUpdate;
}

// virtual
void Model::initJointStates() {
    const HFMModel& hfmModel = getHFMModel();
    glm::mat4 modelOffset = glm::scale(_scale) * glm::translate(_offset);

    _rig.initJointStates(hfmModel, modelOffset);
}

bool Model::findRayIntersectionAgainstSubMeshes(const glm::vec3& origin, const glm::vec3& direction, float& distance,
                                                BoxFace& face, glm::vec3& surfaceNormal, QVariantMap& extraInfo,
                                                bool pickAgainstTriangles, bool allowBackface) {
    bool intersectedSomething = false;

    // if we aren't active, we can't pick yet...
    if (!isActive()) {
        return intersectedSomething;
    }

    // extents is the entity relative, scaled, centered extents of the entity
    glm::mat4 modelToWorldMatrix = createMatFromQuatAndPos(_rotation, _translation);
    glm::mat4 worldToModelMatrix = glm::inverse(modelToWorldMatrix);

    Extents modelExtents = getMeshExtents(); // NOTE: unrotated

    glm::vec3 dimensions = modelExtents.maximum - modelExtents.minimum;
    glm::vec3 corner = -(dimensions * _registrationPoint); // since we're going to do the picking in the model frame of reference
    AABox modelFrameBox(corner, dimensions);

    glm::vec3 modelFrameOrigin = glm::vec3(worldToModelMatrix * glm::vec4(origin, 1.0f));
    glm::vec3 modelFrameDirection = glm::vec3(worldToModelMatrix * glm::vec4(direction, 0.0f));

    // we can use the AABox's intersection by mapping our origin and direction into the model frame
    // and testing intersection there.
    if (modelFrameBox.findRayIntersection(modelFrameOrigin, modelFrameDirection, 1.0f / modelFrameDirection, distance, face, surfaceNormal)) {
        QMutexLocker locker(&_mutex);

        float bestDistance = FLT_MAX;
        BoxFace bestFace;
        Triangle bestModelTriangle;
        Triangle bestWorldTriangle;
        glm::vec3 bestWorldIntersectionPoint;
        glm::vec3 bestMeshIntersectionPoint;
        int bestPartIndex = 0;
        int bestShapeID = 0;
        int bestSubMeshIndex = 0;

        const HFMModel& hfmModel = getHFMModel();
        if (!_triangleSetsValid) {
            calculateTriangleSets(hfmModel);
        }

        glm::mat4 meshToModelMatrix = glm::scale(_scale) * glm::translate(_offset);
        glm::mat4 meshToWorldMatrix = modelToWorldMatrix * meshToModelMatrix;
        glm::mat4 worldToMeshMatrix = glm::inverse(meshToWorldMatrix);

        glm::vec3 meshFrameOrigin = glm::vec3(worldToMeshMatrix * glm::vec4(origin, 1.0f));
        glm::vec3 meshFrameDirection = glm::vec3(worldToMeshMatrix * glm::vec4(direction, 0.0f));
        glm::vec3 meshFrameInvDirection = 1.0f / meshFrameDirection;

        int shapeID = 0;
        int subMeshIndex = 0;

        std::vector<SortedTriangleSet> sortedTriangleSets;
        for (auto& meshTriangleSets : _modelSpaceMeshTriangleSets) {
            int partIndex = 0;
            for (auto& partTriangleSet : meshTriangleSets) {
                float priority = FLT_MAX;
                if (partTriangleSet.getBounds().contains(meshFrameOrigin)) {
                    priority = 0.0f;
                } else {
                    float partBoundDistance = FLT_MAX;
                    BoxFace partBoundFace;
                    glm::vec3 partBoundNormal;
                    if (partTriangleSet.getBounds().findRayIntersection(meshFrameOrigin, meshFrameDirection, meshFrameInvDirection,
                                                                        partBoundDistance, partBoundFace, partBoundNormal)) {
                        priority = partBoundDistance;
                    }
                }

                if (priority < FLT_MAX) {
                    sortedTriangleSets.emplace_back(priority, &partTriangleSet, partIndex, shapeID, subMeshIndex);
                }
                partIndex++;
                shapeID++;
            }
            subMeshIndex++;
        }

        if (sortedTriangleSets.size() > 1) {
            static auto comparator = [](const SortedTriangleSet& left, const SortedTriangleSet& right) { return left.distance < right.distance; };
            std::sort(sortedTriangleSets.begin(), sortedTriangleSets.end(), comparator);
        }

        for (auto it = sortedTriangleSets.begin(); it != sortedTriangleSets.end(); ++it) {
            const SortedTriangleSet& sortedTriangleSet = *it;
            // We can exit once triangleSetDistance > bestDistance
            if (sortedTriangleSet.distance > bestDistance) {
                break;
            }
            float triangleSetDistance = FLT_MAX;
            BoxFace triangleSetFace;
            Triangle triangleSetTriangle;
            if (sortedTriangleSet.triangleSet->findRayIntersection(meshFrameOrigin, meshFrameDirection, meshFrameInvDirection, triangleSetDistance, triangleSetFace,
                                                                   triangleSetTriangle, pickAgainstTriangles, allowBackface)) {
                if (triangleSetDistance < bestDistance) {
                    bestDistance = triangleSetDistance;
                    intersectedSomething = true;
                    bestFace = triangleSetFace;
                    bestModelTriangle = triangleSetTriangle;
                    bestWorldTriangle = triangleSetTriangle * meshToWorldMatrix;
                    glm::vec3 meshIntersectionPoint = meshFrameOrigin + (meshFrameDirection * triangleSetDistance);
                    glm::vec3 worldIntersectionPoint = glm::vec3(meshToWorldMatrix * glm::vec4(meshIntersectionPoint, 1.0f));
                    bestWorldIntersectionPoint = worldIntersectionPoint;
                    bestMeshIntersectionPoint = meshIntersectionPoint;
                    bestPartIndex = sortedTriangleSet.partIndex;
                    bestShapeID = sortedTriangleSet.shapeID;
                    bestSubMeshIndex = sortedTriangleSet.subMeshIndex;
                }
            }
        }

        if (intersectedSomething) {
            distance = bestDistance;
            face = bestFace;
            surfaceNormal = bestWorldTriangle.getNormal();
            extraInfo["worldIntersectionPoint"] = vec3toVariant(bestWorldIntersectionPoint);
            extraInfo["meshIntersectionPoint"] = vec3toVariant(bestMeshIntersectionPoint);
            extraInfo["partIndex"] = bestPartIndex;
            extraInfo["shapeID"] = bestShapeID;
            if (pickAgainstTriangles) {
                extraInfo["subMeshIndex"] = bestSubMeshIndex;
                extraInfo["subMeshName"] = hfmModel.getModelNameOfMesh(bestSubMeshIndex);
                extraInfo["subMeshTriangleWorld"] = QVariantMap{
                    { "v0", vec3toVariant(bestWorldTriangle.v0) },
                    { "v1", vec3toVariant(bestWorldTriangle.v1) },
                    { "v2", vec3toVariant(bestWorldTriangle.v2) },
                };
                extraInfo["subMeshNormal"] = vec3toVariant(bestModelTriangle.getNormal());
                extraInfo["subMeshTriangle"] = QVariantMap{
                    { "v0", vec3toVariant(bestModelTriangle.v0) },
                    { "v1", vec3toVariant(bestModelTriangle.v1) },
                    { "v2", vec3toVariant(bestModelTriangle.v2) },
                };
            }
        }
    }

    return intersectedSomething;
}

bool Model::findParabolaIntersectionAgainstSubMeshes(const glm::vec3& origin, const glm::vec3& velocity, const glm::vec3& acceleration,
                                                     float& parabolicDistance, BoxFace& face, glm::vec3& surfaceNormal, QVariantMap& extraInfo,
                                                     bool pickAgainstTriangles, bool allowBackface) {
    bool intersectedSomething = false;

    // if we aren't active, we can't pick yet...
    if (!isActive()) {
        return intersectedSomething;
    }

    // extents is the entity relative, scaled, centered extents of the entity
    glm::mat4 modelToWorldMatrix = createMatFromQuatAndPos(_rotation, _translation);
    glm::mat4 worldToModelMatrix = glm::inverse(modelToWorldMatrix);

    Extents modelExtents = getMeshExtents(); // NOTE: unrotated

    glm::vec3 dimensions = modelExtents.maximum - modelExtents.minimum;
    glm::vec3 corner = -(dimensions * _registrationPoint); // since we're going to do the picking in the model frame of reference
    AABox modelFrameBox(corner, dimensions);

    glm::vec3 modelFrameOrigin = glm::vec3(worldToModelMatrix * glm::vec4(origin, 1.0f));
    glm::vec3 modelFrameVelocity = glm::vec3(worldToModelMatrix * glm::vec4(velocity, 0.0f));
    glm::vec3 modelFrameAcceleration = glm::vec3(worldToModelMatrix * glm::vec4(acceleration, 0.0f));

    // we can use the AABox's intersection by mapping our origin and direction into the model frame
    // and testing intersection there.
    if (modelFrameBox.findParabolaIntersection(modelFrameOrigin, modelFrameVelocity, modelFrameAcceleration, parabolicDistance, face, surfaceNormal)) {
        QMutexLocker locker(&_mutex);

        float bestDistance = FLT_MAX;
        BoxFace bestFace;
        Triangle bestModelTriangle;
        Triangle bestWorldTriangle;
        glm::vec3 bestWorldIntersectionPoint;
        glm::vec3 bestMeshIntersectionPoint;
        int bestPartIndex = 0;
        int bestShapeID = 0;
        int bestSubMeshIndex = 0;

        const HFMModel& hfmModel = getHFMModel();
        if (!_triangleSetsValid) {
            calculateTriangleSets(hfmModel);
        }

        glm::mat4 meshToModelMatrix = glm::scale(_scale) * glm::translate(_offset);
        glm::mat4 meshToWorldMatrix = modelToWorldMatrix * meshToModelMatrix;
        glm::mat4 worldToMeshMatrix = glm::inverse(meshToWorldMatrix);

        glm::vec3 meshFrameOrigin = glm::vec3(worldToMeshMatrix * glm::vec4(origin, 1.0f));
        glm::vec3 meshFrameVelocity = glm::vec3(worldToMeshMatrix * glm::vec4(velocity, 0.0f));
        glm::vec3 meshFrameAcceleration = glm::vec3(worldToMeshMatrix * glm::vec4(acceleration, 0.0f));

        int shapeID = 0;
        int subMeshIndex = 0;

        std::vector<SortedTriangleSet> sortedTriangleSets;
        for (auto& meshTriangleSets : _modelSpaceMeshTriangleSets) {
            int partIndex = 0;
            for (auto& partTriangleSet : meshTriangleSets) {
                float priority = FLT_MAX;
                if (partTriangleSet.getBounds().contains(meshFrameOrigin)) {
                    priority = 0.0f;
                } else {
                    float partBoundDistance = FLT_MAX;
                    BoxFace partBoundFace;
                    glm::vec3 partBoundNormal;
                    if (partTriangleSet.getBounds().findParabolaIntersection(meshFrameOrigin, meshFrameVelocity, meshFrameAcceleration,
                                                                             partBoundDistance, partBoundFace, partBoundNormal)) {
                        priority = partBoundDistance;
                    }
                }

                if (priority < FLT_MAX) {
                    sortedTriangleSets.emplace_back(priority, &partTriangleSet, partIndex, shapeID, subMeshIndex);
                }
                partIndex++;
                shapeID++;
            }
            subMeshIndex++;
        }

        if (sortedTriangleSets.size() > 1) {
            static auto comparator = [](const SortedTriangleSet& left, const SortedTriangleSet& right) { return left.distance < right.distance; };
            std::sort(sortedTriangleSets.begin(), sortedTriangleSets.end(), comparator);
        }

        for (auto it = sortedTriangleSets.begin(); it != sortedTriangleSets.end(); ++it) {
            const SortedTriangleSet& sortedTriangleSet = *it;
            // We can exit once triangleSetDistance > bestDistance
            if (sortedTriangleSet.distance > bestDistance) {
                break;
            }
            float triangleSetDistance = FLT_MAX;
            BoxFace triangleSetFace;
            Triangle triangleSetTriangle;
            if (sortedTriangleSet.triangleSet->findParabolaIntersection(meshFrameOrigin, meshFrameVelocity, meshFrameAcceleration,
                                                                        triangleSetDistance, triangleSetFace, triangleSetTriangle,
                                                                        pickAgainstTriangles, allowBackface)) {
                if (triangleSetDistance < bestDistance) {
                    bestDistance = triangleSetDistance;
                    intersectedSomething = true;
                    bestFace = triangleSetFace;
                    bestModelTriangle = triangleSetTriangle;
                    bestWorldTriangle = triangleSetTriangle * meshToWorldMatrix;
                    glm::vec3 meshIntersectionPoint = meshFrameOrigin + meshFrameVelocity * triangleSetDistance +
                        0.5f * meshFrameAcceleration * triangleSetDistance * triangleSetDistance;
                    glm::vec3 worldIntersectionPoint = origin + velocity * triangleSetDistance +
                        0.5f * acceleration * triangleSetDistance * triangleSetDistance;
                    bestWorldIntersectionPoint = worldIntersectionPoint;
                    bestMeshIntersectionPoint = meshIntersectionPoint;
                    bestPartIndex = sortedTriangleSet.partIndex;
                    bestShapeID = sortedTriangleSet.shapeID;
                    bestSubMeshIndex = sortedTriangleSet.subMeshIndex;
                    // These sets can overlap, so we can't exit early if we find something
                }
            }
        }

        if (intersectedSomething) {
            parabolicDistance = bestDistance;
            face = bestFace;
            surfaceNormal = bestWorldTriangle.getNormal();
            extraInfo["worldIntersectionPoint"] = vec3toVariant(bestWorldIntersectionPoint);
            extraInfo["meshIntersectionPoint"] = vec3toVariant(bestMeshIntersectionPoint);
            extraInfo["partIndex"] = bestPartIndex;
            extraInfo["shapeID"] = bestShapeID;
            if (pickAgainstTriangles) {
                extraInfo["subMeshIndex"] = bestSubMeshIndex;
                extraInfo["subMeshName"] = hfmModel.getModelNameOfMesh(bestSubMeshIndex);
                extraInfo["subMeshTriangleWorld"] = QVariantMap{
                    { "v0", vec3toVariant(bestWorldTriangle.v0) },
                    { "v1", vec3toVariant(bestWorldTriangle.v1) },
                    { "v2", vec3toVariant(bestWorldTriangle.v2) },
                };
                extraInfo["subMeshNormal"] = vec3toVariant(bestModelTriangle.getNormal());
                extraInfo["subMeshTriangle"] = QVariantMap{
                    { "v0", vec3toVariant(bestModelTriangle.v0) },
                    { "v1", vec3toVariant(bestModelTriangle.v1) },
                    { "v2", vec3toVariant(bestModelTriangle.v2) },
                };
            }
        }
    }

    return intersectedSomething;
}

glm::mat4 Model::getWorldToHFMMatrix() const {
    glm::mat4 hfmToModelMatrix = glm::scale(_scale) * glm::translate(_offset);
    glm::mat4 modelToWorldMatrix = createMatFromQuatAndPos(_rotation, _translation);
    glm::mat4 worldToHFMMatrix = glm::inverse(modelToWorldMatrix * hfmToModelMatrix);
    return worldToHFMMatrix;
}

// TODO: deprecate and remove
MeshProxyList Model::getMeshes() const {
    MeshProxyList result;
    const Geometry::Pointer& renderGeometry = getGeometry();
    const Geometry::GeometryMeshes& meshes = renderGeometry->getMeshes();

    if (!isLoaded()) {
        return result;
    }

    Transform offset;
    offset.setScale(_scale);
    offset.postTranslate(_offset);
    glm::mat4 offsetMat = offset.getMatrix();

    for (std::shared_ptr<const graphics::Mesh> mesh : meshes) {
        if (!mesh) {
            continue;
        }

        MeshProxy* meshProxy = new SimpleMeshProxy(
            mesh->map(
                [=](glm::vec3 position) {
                    return glm::vec3(offsetMat * glm::vec4(position, 1.0f));
                },
                [=](glm::vec3 color) { return color; },
                [=](glm::vec3 normal) {
                    return glm::normalize(glm::vec3(offsetMat * glm::vec4(normal, 0.0f)));
                },
                [&](uint32_t index) { return index; }));
        meshProxy->setObjectName(mesh->displayName.c_str());
        result << meshProxy;
    }

    return result;
}

bool Model::replaceScriptableModelMeshPart(scriptable::ScriptableModelBasePointer newModel, int meshIndex, int partIndex) {
    QMutexLocker lock(&_mutex);

    if (!isLoaded()) {
        qDebug() << "!isLoaded" << this;
        return false;
    }

    if (!newModel || !newModel->meshes.size()) {
        qDebug() << "!newModel.meshes.size()" << this;
        return false;
    }

    const auto& meshes = newModel->meshes;
    render::Transaction transaction;
    const render::ScenePointer& scene = AbstractViewStateInterface::instance()->getMain3DScene();

    meshIndex = max(meshIndex, 0);
    partIndex = max(partIndex, 0);

    if (meshIndex >= (int)meshes.size()) {
        qDebug() << meshIndex << "meshIndex >= newModel.meshes.size()" << meshes.size();
        return false;
    }

    auto mesh = meshes[meshIndex].getMeshPointer();

    if (partIndex >= (int)mesh->getNumParts()) {
        qDebug() << partIndex << "partIndex >= mesh->getNumParts()" << mesh->getNumParts();
        return false;
    }
    {
        // update visual geometry
        render::Transaction transaction;
        for (int i = 0; i < (int) _modelMeshRenderItemIDs.size(); i++) {
            auto itemID = _modelMeshRenderItemIDs[i];
            auto shape = _modelMeshRenderItemShapes[i];
            // TODO: check to see if .partIndex matches too
            if (shape.meshIndex == meshIndex) {
                transaction.updateItem<ModelMeshPartPayload>(itemID, [=](ModelMeshPartPayload& data) {
                    data.updateMeshPart(mesh, partIndex);
                });
            }
        }
        scene->enqueueTransaction(transaction);
    }
    // update triangles for picking
    {
        HFMModel hfmModel;
        for (const auto& newMesh : meshes) {
            HFMMesh mesh;
            mesh._mesh = newMesh.getMeshPointer();
            mesh.vertices = buffer_helpers::mesh::attributeToVector<glm::vec3>(mesh._mesh, gpu::Stream::POSITION);
            int numParts = (int)newMesh.getMeshPointer()->getNumParts();
            for (int partID = 0; partID < numParts; partID++) {
                HFMMeshPart part;
                part.triangleIndices = buffer_helpers::bufferToVector<int>(mesh._mesh->getIndexBuffer(), "part.triangleIndices");
                mesh.parts << part;
            }
            {
                foreach (const glm::vec3& vertex, mesh.vertices) {
                    glm::vec3 transformedVertex = glm::vec3(mesh.modelTransform * glm::vec4(vertex, 1.0f));
                    hfmModel.meshExtents.minimum = glm::min(hfmModel.meshExtents.minimum, transformedVertex);
                    hfmModel.meshExtents.maximum = glm::max(hfmModel.meshExtents.maximum, transformedVertex);
                    mesh.meshExtents.minimum = glm::min(mesh.meshExtents.minimum, transformedVertex);
                    mesh.meshExtents.maximum = glm::max(mesh.meshExtents.maximum, transformedVertex);
                }
            }
            hfmModel.meshes << mesh;
        }
        calculateTriangleSets(hfmModel);
    }
    return true;
}

scriptable::ScriptableModelBase Model::getScriptableModel() {
    QMutexLocker lock(&_mutex);
    scriptable::ScriptableModelBase result;

    if (!isLoaded()) {
        qCDebug(renderutils) << "Model::getScriptableModel -- !isLoaded";
        return result;
    }

    const HFMModel& hfmModel = getHFMModel();
    int numberOfMeshes = hfmModel.meshes.size();
    int shapeID = 0;
    for (int i = 0; i < numberOfMeshes; i++) {
        const HFMMesh& hfmMesh = hfmModel.meshes.at(i);
        if (auto mesh = hfmMesh._mesh) {
            result.append(mesh);

            int numParts = (int)mesh->getNumParts();
            for (int partIndex = 0; partIndex < numParts; partIndex++) {
                result.appendMaterial(graphics::MaterialLayer(getGeometry()->getShapeMaterial(shapeID), 0), shapeID, _modelMeshMaterialNames[shapeID]);
                shapeID++;
            }
        }
    }
    result.appendMaterialNames(_modelMeshMaterialNames);
    return result;
}

void Model::calculateTriangleSets(const HFMModel& hfmModel) {
    PROFILE_RANGE(render, __FUNCTION__);

    int numberOfMeshes = hfmModel.meshes.size();

    _triangleSetsValid = true;
    _modelSpaceMeshTriangleSets.clear();
    _modelSpaceMeshTriangleSets.resize(numberOfMeshes);

    for (int i = 0; i < numberOfMeshes; i++) {
        const HFMMesh& mesh = hfmModel.meshes.at(i);

        const int numberOfParts = mesh.parts.size();
        auto& meshTriangleSets = _modelSpaceMeshTriangleSets[i];
        meshTriangleSets.resize(numberOfParts);

        for (int j = 0; j < numberOfParts; j++) {
            const HFMMeshPart& part = mesh.parts.at(j);

            auto& partTriangleSet = meshTriangleSets[j];

            const int INDICES_PER_TRIANGLE = 3;
            const int INDICES_PER_QUAD = 4;
            const int TRIANGLES_PER_QUAD = 2;

            // tell our triangleSet how many triangles to expect.
            int numberOfQuads = part.quadIndices.size() / INDICES_PER_QUAD;
            int numberOfTris = part.triangleIndices.size() / INDICES_PER_TRIANGLE;
            int totalTriangles = (numberOfQuads * TRIANGLES_PER_QUAD) + numberOfTris;
            partTriangleSet.reserve(totalTriangles);

            auto meshTransform = hfmModel.offset * mesh.modelTransform;

            if (part.quadIndices.size() > 0) {
                int vIndex = 0;
                for (int q = 0; q < numberOfQuads; q++) {
                    int i0 = part.quadIndices[vIndex++];
                    int i1 = part.quadIndices[vIndex++];
                    int i2 = part.quadIndices[vIndex++];
                    int i3 = part.quadIndices[vIndex++];

                    // track the model space version... these points will be transformed by the FST's offset, 
                    // which includes the scaling, rotation, and translation specified by the FST/FBX, 
                    // this can't change at runtime, so we can safely store these in our TriangleSet
                    glm::vec3 v0 = glm::vec3(meshTransform * glm::vec4(mesh.vertices[i0], 1.0f));
                    glm::vec3 v1 = glm::vec3(meshTransform * glm::vec4(mesh.vertices[i1], 1.0f));
                    glm::vec3 v2 = glm::vec3(meshTransform * glm::vec4(mesh.vertices[i2], 1.0f));
                    glm::vec3 v3 = glm::vec3(meshTransform * glm::vec4(mesh.vertices[i3], 1.0f));

                    Triangle tri1 = { v0, v1, v3 };
                    Triangle tri2 = { v1, v2, v3 };
                    partTriangleSet.insert(tri1);
                    partTriangleSet.insert(tri2);
                }
            }

            if (part.triangleIndices.size() > 0) {
                int vIndex = 0;
                for (int t = 0; t < numberOfTris; t++) {
                    int i0 = part.triangleIndices[vIndex++];
                    int i1 = part.triangleIndices[vIndex++];
                    int i2 = part.triangleIndices[vIndex++];

                    // track the model space version... these points will be transformed by the FST's offset, 
                    // which includes the scaling, rotation, and translation specified by the FST/FBX, 
                    // this can't change at runtime, so we can safely store these in our TriangleSet
                    glm::vec3 v0 = glm::vec3(meshTransform * glm::vec4(mesh.vertices[i0], 1.0f));
                    glm::vec3 v1 = glm::vec3(meshTransform * glm::vec4(mesh.vertices[i1], 1.0f));
                    glm::vec3 v2 = glm::vec3(meshTransform * glm::vec4(mesh.vertices[i2], 1.0f));

                    Triangle tri = { v0, v1, v2 };
                    partTriangleSet.insert(tri);
                }
            }
        }
    }
}

void Model::updateRenderItemsKey(const render::ScenePointer& scene) {
    if (!scene) {
        _needsFixupInScene = true;
        return;
    }
    auto renderItemsKey = _renderItemKeyGlobalFlags;
    render::Transaction transaction;
    foreach(auto item, _modelMeshRenderItemsMap.keys()) {
        transaction.updateItem<ModelMeshPartPayload>(item, [renderItemsKey](ModelMeshPartPayload& data) {
            data.updateKey(renderItemsKey);
        });
    }
    scene->enqueueTransaction(transaction);
}

void Model::setVisibleInScene(bool visible, const render::ScenePointer& scene) {
    if (Model::isVisible() != visible) {
        auto keyBuilder = render::ItemKey::Builder(_renderItemKeyGlobalFlags);
        _renderItemKeyGlobalFlags = (visible ? keyBuilder.withVisible() : keyBuilder.withInvisible());
        updateRenderItemsKey(scene);
    }
}

bool Model::isVisible() const {
    return _renderItemKeyGlobalFlags.isVisible();
}

void Model::setCanCastShadow(bool castShadow, const render::ScenePointer& scene) {
    if (Model::canCastShadow() != castShadow) {
        auto keyBuilder = render::ItemKey::Builder(_renderItemKeyGlobalFlags);
        _renderItemKeyGlobalFlags = (castShadow ? keyBuilder.withShadowCaster() : keyBuilder.withoutShadowCaster());
        updateRenderItemsKey(scene);
    }
}

bool Model::canCastShadow() const {
    return _renderItemKeyGlobalFlags.isShadowCaster();
}

void Model::setLayeredInFront(bool layeredInFront, const render::ScenePointer& scene) {
    if (Model::isLayeredInFront() != layeredInFront) {
        auto keyBuilder = render::ItemKey::Builder(_renderItemKeyGlobalFlags);
        _renderItemKeyGlobalFlags = (layeredInFront ? keyBuilder.withLayer(render::hifi::LAYER_3D_FRONT) : keyBuilder.withoutLayer());
        updateRenderItemsKey(scene);
    }
}

bool Model::isLayeredInFront() const {
    return _renderItemKeyGlobalFlags.isLayer(render::hifi::LAYER_3D_FRONT);
}

void Model::setLayeredInHUD(bool layeredInHUD, const render::ScenePointer& scene) {
    if (Model::isLayeredInHUD() != layeredInHUD) {
        auto keyBuilder = render::ItemKey::Builder(_renderItemKeyGlobalFlags);
        _renderItemKeyGlobalFlags = (layeredInHUD ? keyBuilder.withLayer(render::hifi::LAYER_3D_HUD) : keyBuilder.withoutLayer());
        updateRenderItemsKey(scene);
    }
}

bool Model::isLayeredInHUD() const {
    return _renderItemKeyGlobalFlags.isLayer(render::hifi::LAYER_3D_HUD);
}

void Model::setTagMask(uint8_t mask, const render::ScenePointer& scene) {
    if (Model::getTagMask() != mask) {
        auto keyBuilder = render::ItemKey::Builder(_renderItemKeyGlobalFlags);
        _renderItemKeyGlobalFlags = keyBuilder.withTagBits(mask);
        updateRenderItemsKey(scene);
    }
}
render::hifi::Tag Model::getTagMask() const {
    return (render::hifi::Tag) _renderItemKeyGlobalFlags.getTagBits();
}

void Model::setGroupCulled(bool groupCulled, const render::ScenePointer& scene) {
    if (Model::isGroupCulled() != groupCulled) {
        auto keyBuilder = render::ItemKey::Builder(_renderItemKeyGlobalFlags);
        _renderItemKeyGlobalFlags = (groupCulled ? keyBuilder.withSubMetaCulled() : keyBuilder.withoutSubMetaCulled());
        updateRenderItemsKey(scene);
    }
}
bool Model::isGroupCulled() const {
    return _renderItemKeyGlobalFlags.isSubMetaCulled();
}

const render::ItemKey Model::getRenderItemKeyGlobalFlags() const {
    return _renderItemKeyGlobalFlags;
}

bool Model::addToScene(const render::ScenePointer& scene,
                       render::Transaction& transaction,
                       render::Item::Status::Getters& statusGetters,
                       BlendShapeOperator modelBlendshapeOperator) {
    if (!_addedToScene && isLoaded()) {
        updateClusterMatrices();
        if (_modelMeshRenderItems.empty()) {
            createRenderItemSet();
        }
    }

    _modelBlendshapeOperator = modelBlendshapeOperator;

    bool somethingAdded = false;

    if (_modelMeshRenderItemsMap.empty()) {
        bool hasTransparent = false;
        size_t verticesCount = 0;
        foreach(auto renderItem, _modelMeshRenderItems) {
            auto item = scene->allocateID();
            auto renderPayload = std::make_shared<ModelMeshPartPayload::Payload>(renderItem);
            if (_modelMeshRenderItemsMap.empty() && statusGetters.size()) {
                renderPayload->addStatusGetters(statusGetters);
            }
            transaction.resetItem(item, renderPayload);

            hasTransparent = hasTransparent || renderItem.get()->getShapeKey().isTranslucent();
            verticesCount += renderItem.get()->getVerticesCount();
            _modelMeshRenderItemsMap.insert(item, renderPayload);
            _modelMeshRenderItemIDs.emplace_back(item);
        }
        somethingAdded = !_modelMeshRenderItemsMap.empty();

        _renderInfoVertexCount = verticesCount;
        _renderInfoDrawCalls = _modelMeshRenderItemsMap.count();
        _renderInfoHasTransparent = hasTransparent;
    }

    if (somethingAdded) {
        _addedToScene = true;
        updateRenderItems();
        _needsFixupInScene = false;
    }

    return somethingAdded;
}

void Model::removeFromScene(const render::ScenePointer& scene, render::Transaction& transaction) {
    foreach (auto item, _modelMeshRenderItemsMap.keys()) {
        transaction.removeItem(item);
    }
    _modelMeshRenderItemIDs.clear();
    _modelMeshRenderItemsMap.clear();
    _modelMeshRenderItems.clear();
    _modelMeshMaterialNames.clear();
    _modelMeshRenderItemShapes.clear();

    _blendshapeOffsets.clear();
    _blendshapeOffsetsInitialized = false;

    _addedToScene = false;

    _renderInfoVertexCount = 0;
    _renderInfoDrawCalls = 0;
    _renderInfoTextureSize = 0;
    _renderInfoHasTransparent = false;
}

void Model::renderDebugMeshBoxes(gpu::Batch& batch) {
    int colorNdx = 0;
    _mutex.lock();

    glm::mat4 meshToModelMatrix = glm::scale(_scale) * glm::translate(_offset);
    glm::mat4 meshToWorldMatrix = createMatFromQuatAndPos(_rotation, _translation) * meshToModelMatrix;
    Transform meshToWorld(meshToWorldMatrix);
    batch.setModelTransform(meshToWorld);

    DependencyManager::get<GeometryCache>()->bindSimpleProgram(batch, false, false, false, true, true);

    for (auto& meshTriangleSets : _modelSpaceMeshTriangleSets) {
        for (auto &partTriangleSet : meshTriangleSets) {
            auto box = partTriangleSet.getBounds();

            if (_debugMeshBoxesID == GeometryCache::UNKNOWN_ID) {
                _debugMeshBoxesID = DependencyManager::get<GeometryCache>()->allocateID();
            }
            QVector<glm::vec3> points;

            glm::vec3 brn = box.getCorner();
            glm::vec3 bln = brn + glm::vec3(box.getDimensions().x, 0, 0);
            glm::vec3 brf = brn + glm::vec3(0, 0, box.getDimensions().z);
            glm::vec3 blf = brn + glm::vec3(box.getDimensions().x, 0, box.getDimensions().z);

            glm::vec3 trn = brn + glm::vec3(0, box.getDimensions().y, 0);
            glm::vec3 tln = bln + glm::vec3(0, box.getDimensions().y, 0);
            glm::vec3 trf = brf + glm::vec3(0, box.getDimensions().y, 0);
            glm::vec3 tlf = blf + glm::vec3(0, box.getDimensions().y, 0);

            points << brn << bln;
            points << brf << blf;
            points << brn << brf;
            points << bln << blf;

            points << trn << tln;
            points << trf << tlf;
            points << trn << trf;
            points << tln << tlf;

            points << brn << trn;
            points << brf << trf;
            points << bln << tln;
            points << blf << tlf;

            glm::vec4 color[] = {
                { 0.0f, 1.0f, 0.0f, 1.0f }, // green
                { 1.0f, 0.0f, 0.0f, 1.0f }, // red
                { 0.0f, 0.0f, 1.0f, 1.0f }, // blue
                { 1.0f, 0.0f, 1.0f, 1.0f }, // purple
                { 1.0f, 1.0f, 0.0f, 1.0f }, // yellow
                { 0.0f, 1.0f, 1.0f, 1.0f }, // cyan
                { 1.0f, 1.0f, 1.0f, 1.0f }, // white
                { 0.0f, 0.5f, 0.0f, 1.0f },
                { 0.0f, 0.0f, 0.5f, 1.0f },
                { 0.5f, 0.0f, 0.5f, 1.0f },
                { 0.5f, 0.5f, 0.0f, 1.0f },
                { 0.0f, 0.5f, 0.5f, 1.0f } };

            DependencyManager::get<GeometryCache>()->updateVertices(_debugMeshBoxesID, points, color[colorNdx]);
            DependencyManager::get<GeometryCache>()->renderVertices(batch, gpu::LINES, _debugMeshBoxesID);
            colorNdx++;
        }
    }
    _mutex.unlock();
}

Extents Model::getBindExtents() const {
    if (!isActive()) {
        return Extents();
    }
    const Extents& bindExtents = getHFMModel().bindExtents;
    Extents scaledExtents = { bindExtents.minimum * _scale, bindExtents.maximum * _scale };
    return scaledExtents;
}

glm::vec3 Model::getNaturalDimensions() const {
    Extents modelMeshExtents = getUnscaledMeshExtents();
    return modelMeshExtents.maximum - modelMeshExtents.minimum;
}

Extents Model::getMeshExtents() const {
    if (!isActive()) {
        return Extents();
    }
    const Extents& extents = getHFMModel().meshExtents;

    // even though our caller asked for "unscaled" we need to include any fst scaling, translation, and rotation, which
    // is captured in the offset matrix
    glm::vec3 minimum = glm::vec3(getHFMModel().offset * glm::vec4(extents.minimum, 1.0f));
    glm::vec3 maximum = glm::vec3(getHFMModel().offset * glm::vec4(extents.maximum, 1.0f));
    Extents scaledExtents = { minimum * _scale, maximum * _scale };
    return scaledExtents;
}

Extents Model::getUnscaledMeshExtents() const {
    if (!isActive()) {
        return Extents();
    }

    const Extents& extents = getHFMModel().meshExtents;

    // even though our caller asked for "unscaled" we need to include any fst scaling, translation, and rotation, which
    // is captured in the offset matrix
    glm::vec3 minimum = glm::vec3(getHFMModel().offset * glm::vec4(extents.minimum, 1.0f));
    glm::vec3 maximum = glm::vec3(getHFMModel().offset * glm::vec4(extents.maximum, 1.0f));
    Extents scaledExtents = { minimum, maximum };

    return scaledExtents;
}

void Model::clearJointState(int index) {
    _rig.clearJointState(index);
}

void Model::setJointState(int index, bool valid, const glm::quat& rotation, const glm::vec3& translation, float priority) {
    _rig.setJointState(index, valid, rotation, translation, priority);
}

void Model::setJointRotation(int index, bool valid, const glm::quat& rotation, float priority) {
    _rig.setJointRotation(index, valid, rotation, priority);
}

void Model::setJointTranslation(int index, bool valid, const glm::vec3& translation, float priority) {
    _rig.setJointTranslation(index, valid, translation, priority);
}

int Model::getParentJointIndex(int jointIndex) const {
    return (isActive() && jointIndex != -1) ? getHFMModel().joints.at(jointIndex).parentIndex : -1;
}

int Model::getLastFreeJointIndex(int jointIndex) const {
    return (isActive() && jointIndex != -1) ? getHFMModel().joints.at(jointIndex).freeLineage.last() : -1;
}

void Model::setTextures(const QVariantMap& textures) {
    if (isLoaded()) {
        _needsFixupInScene = true;
        _renderGeometry->setTextures(textures);
        _pendingTextures.clear();
    } else {
        _pendingTextures = textures;
    }
}

void Model::setURL(const QUrl& url) {
    // don't recreate the geometry if it's the same URL
    if (_url == url && _renderWatcher.getURL() == url) {
        return;
    }

    _url = url;

    {
        render::Transaction transaction;
        const render::ScenePointer& scene = AbstractViewStateInterface::instance()->getMain3DScene();
        if (scene) {
            removeFromScene(scene, transaction);
            scene->enqueueTransaction(transaction);
        } else {
            qCWarning(renderutils) << "Model::setURL(), Unexpected null scene, possibly during application shutdown";
        }
    }

    _needsReload = true;
    // One might be tempted to _pendingTextures.clear(), thinking that a new URL means an old texture doesn't apply.
    // But sometimes, particularly when first setting the values, the texture might be set first. So let's not clear here.
    _visualGeometryRequestFailed = false;
    _needsFixupInScene = true;
    invalidCalculatedMeshBoxes();
    deleteGeometry();

    auto resource = DependencyManager::get<ModelCache>()->getGeometryResource(url);
    if (resource) {
        resource->setLoadPriority(this, _loadingPriority);
        _renderWatcher.setResource(resource);
    }
    onInvalidate();
}

void Model::loadURLFinished(bool success) {
    if (!success) {
        _visualGeometryRequestFailed = true;
    } else if (!_pendingTextures.empty()) {
        setTextures(_pendingTextures);
    }
    emit setURLFinished(success);
}

bool Model::getJointPositionInWorldFrame(int jointIndex, glm::vec3& position) const {
    return _rig.getJointPositionInWorldFrame(jointIndex, position, _translation, _rotation);
}

bool Model::getJointPosition(int jointIndex, glm::vec3& position) const {
    return _rig.getJointPosition(jointIndex, position);
}

bool Model::getJointRotationInWorldFrame(int jointIndex, glm::quat& rotation) const {
    return _rig.getJointRotationInWorldFrame(jointIndex, rotation, _rotation);
}

bool Model::getJointRotation(int jointIndex, glm::quat& rotation) const {
    return _rig.getJointRotation(jointIndex, rotation);
}

bool Model::getJointTranslation(int jointIndex, glm::vec3& translation) const {
    return _rig.getJointTranslation(jointIndex, translation);
}

bool Model::getAbsoluteJointRotationInRigFrame(int jointIndex, glm::quat& rotationOut) const {
    return _rig.getAbsoluteJointRotationInRigFrame(jointIndex, rotationOut);
}

bool Model::getAbsoluteJointTranslationInRigFrame(int jointIndex, glm::vec3& translationOut) const {
    return _rig.getAbsoluteJointTranslationInRigFrame(jointIndex, translationOut);
}

bool Model::getRelativeDefaultJointRotation(int jointIndex, glm::quat& rotationOut) const {
    return _rig.getRelativeDefaultJointRotation(jointIndex, rotationOut);
}

bool Model::getRelativeDefaultJointTranslation(int jointIndex, glm::vec3& translationOut) const {
    return _rig.getRelativeDefaultJointTranslation(jointIndex, translationOut);
}

QStringList Model::getJointNames() const {
    if (QThread::currentThread() != thread()) {
        QStringList result;
        BLOCKING_INVOKE_METHOD(const_cast<Model*>(this), "getJointNames",
            Q_RETURN_ARG(QStringList, result));
        return result;
    }
    return isActive() ? getHFMModel().getJointNames() : QStringList();
}

void Model::setScaleToFit(bool scaleToFit, const glm::vec3& dimensions, bool forceRescale) {
    if (forceRescale || _scaleToFit != scaleToFit || _scaleToFitDimensions != dimensions) {
        _scaleToFit = scaleToFit;
        _scaleToFitDimensions = dimensions;
        _scaledToFit = false; // force rescaling
    }
}

void Model::setScaleToFit(bool scaleToFit, float largestDimension, bool forceRescale) {
    // NOTE: if the model is not active, then it means we don't actually know the true/natural dimensions of the
    // mesh, and so we can't do the needed calculations for scaling to fit to a single largest dimension. In this
    // case we will record that we do want to do this, but we will stick our desired single dimension into the
    // first element of the vec3 for the non-fixed aspect ration dimensions
    if (!isActive()) {
        _scaleToFit = scaleToFit;
        if (scaleToFit) {
            _scaleToFitDimensions = glm::vec3(largestDimension, FAKE_DIMENSION_PLACEHOLDER, FAKE_DIMENSION_PLACEHOLDER);
        }
        return;
    }

    if (forceRescale || _scaleToFit != scaleToFit || glm::length(_scaleToFitDimensions) != largestDimension) {
        _scaleToFit = scaleToFit;

        // we only need to do this work if we're "turning on" scale to fit.
        if (scaleToFit) {
            Extents modelMeshExtents = getUnscaledMeshExtents();
            float maxDimension = glm::distance(modelMeshExtents.maximum, modelMeshExtents.minimum);
            float maxScale = largestDimension / maxDimension;
            glm::vec3 modelMeshDimensions = modelMeshExtents.maximum - modelMeshExtents.minimum;
            glm::vec3 dimensions = modelMeshDimensions * maxScale;

            _scaleToFitDimensions = dimensions;
            _scaledToFit = false; // force rescaling
        }
    }
}

glm::vec3 Model::getScaleToFitDimensions() const {
    if (_scaleToFitDimensions.y == FAKE_DIMENSION_PLACEHOLDER &&
        _scaleToFitDimensions.z == FAKE_DIMENSION_PLACEHOLDER) {
        return glm::vec3(_scaleToFitDimensions.x);
    }
    return _scaleToFitDimensions;
}

void Model::scaleToFit() {
    // If our _scaleToFitDimensions.y/z are FAKE_DIMENSION_PLACEHOLDER then it means our
    // user asked to scale us in a fixed aspect ratio to a single largest dimension, but
    // we didn't yet have an active mesh. We can only enter this scaleToFit() in this state
    // if we now do have an active mesh, so we take this opportunity to actually determine
    // the correct scale.
    if (_scaleToFit && _scaleToFitDimensions.y == FAKE_DIMENSION_PLACEHOLDER
            && _scaleToFitDimensions.z == FAKE_DIMENSION_PLACEHOLDER) {
        setScaleToFit(_scaleToFit, _scaleToFitDimensions.x);
    }
    Extents modelMeshExtents = getUnscaledMeshExtents();

    // size is our "target size in world space"
    // we need to set our model scale so that the extents of the mesh, fit in a box that size...
    glm::vec3 meshDimensions = modelMeshExtents.maximum - modelMeshExtents.minimum;
    glm::vec3 rescaleDimensions = _scaleToFitDimensions / meshDimensions;
    setScaleInternal(rescaleDimensions);
    _scaledToFit = true;
}

void Model::setSnapModelToRegistrationPoint(bool snapModelToRegistrationPoint, const glm::vec3& registrationPoint) {
    glm::vec3 clampedRegistrationPoint = glm::clamp(registrationPoint, 0.0f, 1.0f);
    if (_snapModelToRegistrationPoint != snapModelToRegistrationPoint || _registrationPoint != clampedRegistrationPoint) {
        _snapModelToRegistrationPoint = snapModelToRegistrationPoint;
        _registrationPoint = clampedRegistrationPoint;
        _snappedToRegistrationPoint = false; // force re-centering
    }
}

void Model::snapToRegistrationPoint() {
    Extents modelMeshExtents = getUnscaledMeshExtents();
    glm::vec3 dimensions = (modelMeshExtents.maximum - modelMeshExtents.minimum);
    glm::vec3 offset = -modelMeshExtents.minimum - (dimensions * _registrationPoint);
    _offset = offset;
    _snappedToRegistrationPoint = true;
}

void Model::setUseDualQuaternionSkinning(bool value) {
    _useDualQuaternionSkinning = value;
}

void Model::simulate(float deltaTime, bool fullUpdate) {
    DETAILED_PROFILE_RANGE(simulation_detail, __FUNCTION__);
    fullUpdate = updateGeometry() || fullUpdate || (_scaleToFit && !_scaledToFit)
                    || (_snapModelToRegistrationPoint && !_snappedToRegistrationPoint);

    if (isActive() && fullUpdate) {
        onInvalidate();

        // check for scale to fit
        if (_scaleToFit && !_scaledToFit) {
            scaleToFit();
        }
        if (_snapModelToRegistrationPoint && !_snappedToRegistrationPoint) {
            snapToRegistrationPoint();
        }
        // update the world space transforms for all joints
        glm::mat4 parentTransform = glm::scale(_scale) * glm::translate(_offset);
        updateRig(deltaTime, parentTransform);

        computeMeshPartLocalBounds();
    }
}

//virtual
void Model::updateRig(float deltaTime, glm::mat4 parentTransform) {
    _needsUpdateClusterMatrices = true;
    glm::mat4 rigToWorldTransform = createMatFromQuatAndPos(getRotation(), getTranslation());
    _rig.updateAnimations(deltaTime, parentTransform, rigToWorldTransform);
}

void Model::computeMeshPartLocalBounds() {
    for (auto& part : _modelMeshRenderItems) {
        const Model::MeshState& state = _meshStates.at(part->_meshIndex);
        if (_useDualQuaternionSkinning) {
            part->computeAdjustedLocalBound(state.clusterDualQuaternions);
        } else {
            part->computeAdjustedLocalBound(state.clusterMatrices);
        }
    }
}

// virtual
void Model::updateClusterMatrices() {
    DETAILED_PERFORMANCE_TIMER("Model::updateClusterMatrices");

    if (!_needsUpdateClusterMatrices || !isLoaded()) {
        return;
    }

    _needsUpdateClusterMatrices = false;
    const HFMModel& hfmModel = getHFMModel();
    for (int i = 0; i < (int) _meshStates.size(); i++) {
        MeshState& state = _meshStates[i];
        int meshIndex = i;
        const HFMMesh& mesh = hfmModel.meshes.at(i);
        for (int j = 0; j < mesh.clusters.size(); j++) {
            const HFMCluster& cluster = mesh.clusters.at(j);
            int clusterIndex = j;

            if (_useDualQuaternionSkinning) {
                auto jointPose = _rig.getJointPose(cluster.jointIndex);
                Transform jointTransform(jointPose.rot(), jointPose.scale(), jointPose.trans());
                Transform clusterTransform;
                Transform::mult(clusterTransform, jointTransform, _rig.getAnimSkeleton()->getClusterBindMatricesOriginalValues(meshIndex, clusterIndex).inverseBindTransform);
                state.clusterDualQuaternions[j] = Model::TransformDualQuaternion(clusterTransform);
            } else {
                auto jointMatrix = _rig.getJointTransform(cluster.jointIndex);
                glm_mat4u_mul(jointMatrix, _rig.getAnimSkeleton()->getClusterBindMatricesOriginalValues(meshIndex, clusterIndex).inverseBindMatrix, state.clusterMatrices[j]);
            }
        }
    }

    // post the blender if we're not currently waiting for one to finish
    auto modelBlender = DependencyManager::get<ModelBlender>();
    if (_blendshapeOffsetsInitialized && modelBlender->shouldComputeBlendshapes() && hfmModel.hasBlendedMeshes() && _blendshapeCoefficients != _blendedBlendshapeCoefficients) {
        _blendedBlendshapeCoefficients = _blendshapeCoefficients;
        modelBlender->noteRequiresBlend(getThisPointer());
    }
}

void Model::deleteGeometry() {
    _deleteGeometryCounter++;
    _blendshapeOffsets.clear();
    _blendshapeOffsetsInitialized = false;
    _meshStates.clear();
    _rig.destroyAnimGraph();
    _blendedBlendshapeCoefficients.clear();
    _renderGeometry.reset();
}

void Model::overrideModelTransformAndOffset(const Transform& transform, const glm::vec3& offset) {
    _overrideTranslation = transform.getTranslation();
    _overrideRotation = transform.getRotation();
    _overrideModelTransform = true;
    setScale(transform.getScale());
    setOffset(offset);
}

AABox Model::getRenderableMeshBound() const {
    if (!isLoaded()) {
        return AABox();
    } else {
        // Build a bound using the last known bound from all the renderItems.
        AABox totalBound;
        for (auto& renderItem : _modelMeshRenderItems) {
            totalBound += renderItem->getBound();
        }
        return totalBound;
    }
}

const render::ItemIDs& Model::fetchRenderItemIDs() const {
    return _modelMeshRenderItemIDs;
}

void Model::createRenderItemSet() {
    assert(isLoaded());
    const auto& meshes = _renderGeometry->getMeshes();

    // all of our mesh vectors must match in size
    if (meshes.size() != _meshStates.size()) {
        qCDebug(renderutils) << "WARNING!!!! Mesh Sizes don't match! " << meshes.size() << _meshStates.size() << " We will not segregate mesh groups yet.";
        return;
    }

    // We should not have any existing renderItems if we enter this section of code
    Q_ASSERT(_modelMeshRenderItems.isEmpty());

    _modelMeshRenderItems.clear();
    _modelMeshMaterialNames.clear();
    _modelMeshRenderItemShapes.clear();

    Transform transform;
    transform.setTranslation(_translation);
    transform.setRotation(_rotation);

    Transform offset;
    offset.setScale(_scale);
    offset.postTranslate(_offset);

    // Run through all of the meshes, and place them into their segregated, but unsorted buckets
    int shapeID = 0;
    uint32_t numMeshes = (uint32_t)meshes.size();
    auto& hfmModel = getHFMModel();
    for (uint32_t i = 0; i < numMeshes; i++) {
        const auto& mesh = meshes.at(i);
        if (!mesh) {
            continue;
        }

        // Create the render payloads
        int numParts = (int)mesh->getNumParts();
        for (int partIndex = 0; partIndex < numParts; partIndex++) {
            initializeBlendshapes(hfmModel.meshes[i], i);
            _modelMeshRenderItems << std::make_shared<ModelMeshPartPayload>(shared_from_this(), i, partIndex, shapeID, transform, offset);
            auto material = getGeometry()->getShapeMaterial(shapeID);
            _modelMeshMaterialNames.push_back(material ? material->getName() : "");
            _modelMeshRenderItemShapes.emplace_back(ShapeInfo{ (int)i });
            shapeID++;
        }
    }
    _blendshapeOffsetsInitialized = true;
}

bool Model::isRenderable() const {
    return !_meshStates.empty() || (isLoaded() && _renderGeometry->getMeshes().empty());
}

std::vector<unsigned int> Model::getMeshIDsFromMaterialID(QString parentMaterialName) {
    // try to find all meshes with materials that match parentMaterialName as a string
    // if none, return parentMaterialName as a uint
    std::vector<unsigned int> toReturn;
    const QString MATERIAL_NAME_PREFIX = "mat::";
    if (parentMaterialName.startsWith(MATERIAL_NAME_PREFIX)) {
        parentMaterialName.replace(0, MATERIAL_NAME_PREFIX.size(), QString(""));
        for (unsigned int i = 0; i < (unsigned int)_modelMeshMaterialNames.size(); i++) {
            if (_modelMeshMaterialNames[i] == parentMaterialName.toStdString()) {
                toReturn.push_back(i);
            }
        }
    }

    if (toReturn.empty()) {
        toReturn.push_back(parentMaterialName.toUInt());
    }

    return toReturn;
}

void Model::addMaterial(graphics::MaterialLayer material, const std::string& parentMaterialName) {
    std::vector<unsigned int> shapeIDs = getMeshIDsFromMaterialID(QString(parentMaterialName.c_str()));
    render::Transaction transaction;
    for (auto shapeID : shapeIDs) {
        if (shapeID < _modelMeshRenderItemIDs.size()) {
            auto itemID = _modelMeshRenderItemIDs[shapeID];
            auto renderItemsKey = _renderItemKeyGlobalFlags;
            bool wireframe = isWireframe();
            auto meshIndex = _modelMeshRenderItemShapes[shapeID].meshIndex;
            bool invalidatePayloadShapeKey = shouldInvalidatePayloadShapeKey(meshIndex);
            bool useDualQuaternionSkinning = _useDualQuaternionSkinning;
            transaction.updateItem<ModelMeshPartPayload>(itemID, [material, renderItemsKey,
                invalidatePayloadShapeKey, wireframe, useDualQuaternionSkinning](ModelMeshPartPayload& data) {
                data.addMaterial(material);
                // if the material changed, we might need to update our item key or shape key
                data.updateKey(renderItemsKey);
                data.setShapeKey(invalidatePayloadShapeKey, wireframe, useDualQuaternionSkinning);
            });
        }
    }
    AbstractViewStateInterface::instance()->getMain3DScene()->enqueueTransaction(transaction);
}

void Model::removeMaterial(graphics::MaterialPointer material, const std::string& parentMaterialName) {
    std::vector<unsigned int> shapeIDs = getMeshIDsFromMaterialID(QString(parentMaterialName.c_str()));
    render::Transaction transaction;
    for (auto shapeID : shapeIDs) {
        if (shapeID < _modelMeshRenderItemIDs.size()) {
            auto itemID = _modelMeshRenderItemIDs[shapeID];
            auto renderItemsKey = _renderItemKeyGlobalFlags;
            bool wireframe = isWireframe();
            auto meshIndex = _modelMeshRenderItemShapes[shapeID].meshIndex;
            bool invalidatePayloadShapeKey = shouldInvalidatePayloadShapeKey(meshIndex);
            bool useDualQuaternionSkinning = _useDualQuaternionSkinning;
            transaction.updateItem<ModelMeshPartPayload>(itemID, [material, renderItemsKey,
                invalidatePayloadShapeKey, wireframe, useDualQuaternionSkinning](ModelMeshPartPayload& data) {
                data.removeMaterial(material);
                // if the material changed, we might need to update our item key or shape key
                data.updateKey(renderItemsKey);
                data.setShapeKey(invalidatePayloadShapeKey, wireframe, useDualQuaternionSkinning);
            });
        }
    }
    AbstractViewStateInterface::instance()->getMain3DScene()->enqueueTransaction(transaction);
}

class CollisionRenderGeometry : public Geometry {
public:
    CollisionRenderGeometry(graphics::MeshPointer mesh) {
        _hfmModel = std::make_shared<HFMModel>();
        std::shared_ptr<GeometryMeshes> meshes = std::make_shared<GeometryMeshes>();
        meshes->push_back(mesh);
        _meshes = meshes;
        _meshParts = std::shared_ptr<const GeometryMeshParts>();
    }
};


using packBlendshapeOffsetTo = void(glm::uvec4& packed, const BlendshapeOffsetUnpacked& unpacked);

void packBlendshapeOffsetTo_Pos_F32_3xSN10_Nor_3xSN10_Tan_3xSN10(glm::uvec4& packed, const BlendshapeOffsetUnpacked& unpacked) {
    float len = glm::compMax(glm::abs(unpacked.positionOffset));
    glm::vec3 normalizedPos(unpacked.positionOffset);
    if (len > 1.0f) {
        normalizedPos /= len;
    } else {
        len = 1.0f;
    }

    packed = glm::uvec4(
        glm::floatBitsToUint(len),
        glm::packSnorm3x10_1x2(glm::vec4(normalizedPos, 0.0f)),
        glm::packSnorm3x10_1x2(glm::vec4(unpacked.normalOffset, 0.0f)),
        glm::packSnorm3x10_1x2(glm::vec4(unpacked.tangentOffset, 0.0f))
    );
}

class Blender : public QRunnable {
public:

    Blender(ModelPointer model, int blendNumber, const Geometry::WeakPointer& geometry, const QVector<float>& blendshapeCoefficients);

    virtual void run() override;

private:

    ModelPointer _model;
    int _blendNumber;
    Geometry::WeakPointer _geometry;
    QVector<float> _blendshapeCoefficients;
};

Blender::Blender(ModelPointer model, int blendNumber, const Geometry::WeakPointer& geometry, const QVector<float>& blendshapeCoefficients) :
    _model(model),
    _blendNumber(blendNumber),
    _geometry(geometry),
    _blendshapeCoefficients(blendshapeCoefficients) {
}

void Blender::run() {
    QVector<BlendshapeOffset> blendshapeOffsets;
    QVector<int> blendedMeshSizes;
    if (_model && _model->isLoaded()) {
        DETAILED_PROFILE_RANGE_EX(simulation_animation, __FUNCTION__, 0xFFFF0000, 0, { { "url", _model->getURL().toString() } });
        int offset = 0;
        auto meshes = _model->getHFMModel().meshes;
        int meshIndex = 0;
        foreach(const HFMMesh& mesh, meshes) {
            auto modelMeshBlendshapeOffsets = _model->_blendshapeOffsets.find(meshIndex++);
            if (mesh.blendshapes.isEmpty() || modelMeshBlendshapeOffsets == _model->_blendshapeOffsets.end()) {
                // Not blendshaped or not initialized
                blendedMeshSizes.push_back(0);
                continue;
            }

            if (mesh.vertices.size() != modelMeshBlendshapeOffsets->second.size()) {
                // Mesh sizes don't match.  Something has gone wrong
                blendedMeshSizes.push_back(0);
                continue;
            }

            blendshapeOffsets += modelMeshBlendshapeOffsets->second;
            BlendshapeOffset* meshBlendshapeOffsets = blendshapeOffsets.data() + offset;
            int numVertices = modelMeshBlendshapeOffsets->second.size();
            blendedMeshSizes.push_back(numVertices);
            offset += numVertices;
            std::vector<BlendshapeOffsetUnpacked> unpackedBlendshapeOffsets(modelMeshBlendshapeOffsets->second.size());

            const float NORMAL_COEFFICIENT_SCALE = 0.01f;
            for (int i = 0, n = qMin(_blendshapeCoefficients.size(), mesh.blendshapes.size()); i < n; i++) {
                float vertexCoefficient = _blendshapeCoefficients.at(i);
                const float EPSILON = 0.0001f;
                if (vertexCoefficient < EPSILON) {
                    continue;
                }

                float normalCoefficient = vertexCoefficient * NORMAL_COEFFICIENT_SCALE;
                const HFMBlendshape& blendshape = mesh.blendshapes.at(i);

                tbb::parallel_for(tbb::blocked_range<int>(0, blendshape.indices.size()), [&](const tbb::blocked_range<int>& range) {
                    for (auto j = range.begin(); j < range.end(); j++) {
                        int index = blendshape.indices.at(j);

                        auto& currentBlendshapeOffset = unpackedBlendshapeOffsets[index];
                        currentBlendshapeOffset.positionOffset += blendshape.vertices.at(j) * vertexCoefficient;

                        currentBlendshapeOffset.normalOffset += blendshape.normals.at(j) * normalCoefficient;
                        if (j < blendshape.tangents.size()) {
                            currentBlendshapeOffset.tangentOffset += blendshape.tangents.at(j) * normalCoefficient;
                        }
                    }
                });
            }

            // Blendshape offsets are generrated, now let's pack it on its way to gpu
            tbb::parallel_for(tbb::blocked_range<int>(0, (int) unpackedBlendshapeOffsets.size()), [&](const tbb::blocked_range<int>& range) {
                auto unpacked = unpackedBlendshapeOffsets.data() + range.begin();
                auto packed = meshBlendshapeOffsets + range.begin();
                for (auto j = range.begin(); j < range.end(); j++) {
                    packBlendshapeOffsetTo_Pos_F32_3xSN10_Nor_3xSN10_Tan_3xSN10((*packed).packedPosNorTan, (*unpacked));

                    unpacked++;
                    packed++;
                }
            });
        }
    }
    // post the result to the ModelBlender, which will dispatch to the model if still alive
    QMetaObject::invokeMethod(DependencyManager::get<ModelBlender>().data(), "setBlendedVertices",
        Q_ARG(ModelPointer, _model), Q_ARG(int, _blendNumber), Q_ARG(QVector<BlendshapeOffset>, blendshapeOffsets), Q_ARG(QVector<int>, blendedMeshSizes));
}

bool Model::maybeStartBlender() {
    if (isLoaded()) {
        QThreadPool::globalInstance()->start(new Blender(getThisPointer(), ++_blendNumber, _renderGeometry, _blendshapeCoefficients));
        return true;
    }
    return false;
}

void Model::initializeBlendshapes(const HFMMesh& mesh, int index) {
    if (mesh.blendshapes.empty()) {
        // mesh doesn't have blendshape, did we allocate one though ?
        if (_blendshapeOffsets.find(index) != _blendshapeOffsets.end()) {
            qWarning() << "Mesh does not have Blendshape yet the blendshapeOffsets are allocated ?";
        }
        return;
    }
    // Mesh has blendshape, let s allocate the local buffer if not done yet
    if (_blendshapeOffsets.find(index) == _blendshapeOffsets.end()) {
        QVector<BlendshapeOffset> blendshapeOffset;
        blendshapeOffset.fill(BlendshapeOffset(), mesh.vertices.size());
        _blendshapeOffsets[index] = blendshapeOffset;
    }
}

ModelBlender::ModelBlender() :
    _pendingBlenders(0) {
}

ModelBlender::~ModelBlender() {
}

void ModelBlender::noteRequiresBlend(ModelPointer model) {
    Lock lock(_mutex);
    if (_modelsRequiringBlendsSet.find(model) == _modelsRequiringBlendsSet.end()) {
        _modelsRequiringBlendsQueue.push(model);
        _modelsRequiringBlendsSet.insert(model);
    }

    if (_pendingBlenders < QThread::idealThreadCount()) {
        while (!_modelsRequiringBlendsQueue.empty()) {
            auto weakPtr = _modelsRequiringBlendsQueue.front();
            _modelsRequiringBlendsQueue.pop();
            _modelsRequiringBlendsSet.erase(weakPtr);
            ModelPointer nextModel = weakPtr.lock();
            if (nextModel && nextModel->maybeStartBlender()) {
                _pendingBlenders++;
                return;
            }
        }
    }
}

void ModelBlender::setBlendedVertices(ModelPointer model, int blendNumber, QVector<BlendshapeOffset> blendshapeOffsets, QVector<int> blendedMeshSizes) {
    if (model) {
        auto blendshapeOperator = model->getModelBlendshapeOperator();
        if (blendshapeOperator) {
            blendshapeOperator(blendNumber, blendshapeOffsets, blendedMeshSizes, model->fetchRenderItemIDs());
        }
    }

    {
        Lock lock(_mutex);
        _pendingBlenders--;
        while (!_modelsRequiringBlendsQueue.empty()) {
            auto weakPtr = _modelsRequiringBlendsQueue.front();
            _modelsRequiringBlendsQueue.pop();
            _modelsRequiringBlendsSet.erase(weakPtr);
            ModelPointer nextModel = weakPtr.lock();
            if (nextModel && nextModel->maybeStartBlender()) {
                _pendingBlenders++;
                break;
            }
        }
    }
}
