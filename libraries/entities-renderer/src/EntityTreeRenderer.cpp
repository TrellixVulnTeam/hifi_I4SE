//
//  EntityTreeRenderer.cpp
//  interface/src
//
//  Created by Brad Hefta-Gaub on 12/6/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "EntityTreeRenderer.h"

#include <glm/gtx/quaternion.hpp>
#include <queue>

#include <QEventLoop>
#include <QScriptSyntaxCheckResult>
#include <QThreadPool>

#include <shared/QtHelpers.h>
#include <AbstractScriptingServicesInterface.h>
#include <AbstractViewStateInterface.h>
#include <AddressManager.h>
#include <ColorUtils.h>
#include <Model.h>
#include <NetworkAccessManager.h>
#include <PerfStat.h>
#include <PrioritySortUtil.h>
#include <Rig.h>
#include <SceneScriptingInterface.h>
#include <ScriptEngine.h>
#include <EntitySimulation.h>
#include <ZoneRenderer.h>
#include <PhysicalEntitySimulation.h>

#include "EntitiesRendererLogging.h"
#include "RenderableEntityItem.h"

#include "RenderableWebEntityItem.h"

#include <PointerManager.h>

std::function<bool()> EntityTreeRenderer::_entitiesShouldFadeFunction = []() { return true; };
std::function<glm::vec3()> EntityTreeRenderer::_getAvatarUpOperator = []() { return Vectors::UP; };

QString resolveScriptURL(const QString& scriptUrl) {
    auto normalizedScriptUrl = DependencyManager::get<ResourceManager>()->normalizeURL(scriptUrl);
    QUrl url { normalizedScriptUrl };
    if (url.isLocalFile()) {
        // Outside of the ScriptEngine, /~/ resolves to the /resources directory.
        // Inside of the ScriptEngine, /~/ resolves to the /scripts directory.
        // Here we expand local paths in case they are /~/ paths, so they aren't
        // incorrectly recognized as being located in /scripts when utilized in ScriptEngine.
        return PathUtils::expandToLocalDataAbsolutePath(url).toString();
    }
    return normalizedScriptUrl;
}

EntityTreeRenderer::EntityTreeRenderer(bool wantScripts, AbstractViewStateInterface* viewState,
                                            AbstractScriptingServicesInterface* scriptingServices) :
    _wantScripts(wantScripts),
    _lastPointerEventValid(false),
    _viewState(viewState),
    _scriptingServices(scriptingServices),
    _displayModelBounds(false)
{
    setMouseRayPickResultOperator([](unsigned int rayPickID) {
        RayToEntityIntersectionResult entityResult;
        return entityResult;
    });
    setSetPrecisionPickingOperator([](unsigned int rayPickID, bool value) {});
    EntityRenderer::initEntityRenderers();
    _currentHoverOverEntityID = UNKNOWN_ENTITY_ID;
    _currentClickingOnEntityID = UNKNOWN_ENTITY_ID;

    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    auto pointerManager = DependencyManager::get<PointerManager>();
    connect(pointerManager.data(), &PointerManager::hoverBeginEntity, entityScriptingInterface.data(), &EntityScriptingInterface::hoverEnterEntity);
    connect(pointerManager.data(), &PointerManager::hoverContinueEntity, entityScriptingInterface.data(), &EntityScriptingInterface::hoverOverEntity);
    connect(pointerManager.data(), &PointerManager::hoverEndEntity, entityScriptingInterface.data(), &EntityScriptingInterface::hoverLeaveEntity);
    connect(pointerManager.data(), &PointerManager::triggerBeginEntity, entityScriptingInterface.data(), &EntityScriptingInterface::mousePressOnEntity);
    connect(pointerManager.data(), &PointerManager::triggerContinueEntity, entityScriptingInterface.data(), &EntityScriptingInterface::mouseMoveOnEntity);
    connect(pointerManager.data(), &PointerManager::triggerEndEntity, entityScriptingInterface.data(), &EntityScriptingInterface::mouseReleaseOnEntity);

    // Forward mouse events to web entities
    auto handlePointerEvent = [&](const EntityItemID& entityID, const PointerEvent& event) {
        std::shared_ptr<render::entities::WebEntityRenderer> thisEntity;
        auto entity = getEntity(entityID);
        if (entity && entity->getType() == EntityTypes::Web) {
            thisEntity = std::static_pointer_cast<render::entities::WebEntityRenderer>(renderableForEntityId(entityID));
        }
        if (thisEntity) {
            QMetaObject::invokeMethod(thisEntity.get(), "handlePointerEvent", Q_ARG(PointerEvent, event));
        }
    };
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::mousePressOnEntity, this, handlePointerEvent);
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::mouseReleaseOnEntity, this, handlePointerEvent);
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::mouseMoveOnEntity, this, handlePointerEvent);
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::hoverEnterEntity, this, [&](const EntityItemID& entityID, const PointerEvent& event) {
        std::shared_ptr<render::entities::WebEntityRenderer> thisEntity;
        auto entity = getEntity(entityID);
        if (entity && entity->getType() == EntityTypes::Web) {
            thisEntity = std::static_pointer_cast<render::entities::WebEntityRenderer>(renderableForEntityId(entityID));
        }
        if (thisEntity) {
            QMetaObject::invokeMethod(thisEntity.get(), "hoverEnterEntity", Q_ARG(PointerEvent, event));
        }
    });
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::hoverOverEntity, this, handlePointerEvent);
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::hoverLeaveEntity, this, [&](const EntityItemID& entityID, const PointerEvent& event) {
        std::shared_ptr<render::entities::WebEntityRenderer> thisEntity;
        auto entity = getEntity(entityID);
        if (entity && entity->getType() == EntityTypes::Web) {
            thisEntity = std::static_pointer_cast<render::entities::WebEntityRenderer>(renderableForEntityId(entityID));
        }
        if (thisEntity) {
            QMetaObject::invokeMethod(thisEntity.get(), "hoverLeaveEntity", Q_ARG(PointerEvent, event));
        }
    });
}

EntityTreeRenderer::~EntityTreeRenderer() {
    // NOTE: We don't need to delete _entitiesScriptEngine because
    //       it is registered with ScriptEngines, which will call deleteLater for us.
}

EntityRendererPointer EntityTreeRenderer::renderableForEntityId(const EntityItemID& id) const {
    auto itr = _entitiesInScene.find(id);
    if (itr == _entitiesInScene.end()) {
        return EntityRendererPointer();
    }
    return itr->second;
}

render::ItemID EntityTreeRenderer::renderableIdForEntityId(const EntityItemID& id) const {
    auto renderable = renderableForEntityId(id);
    if (renderable) {
        return renderable->getRenderItemID();
    } else {
        return render::Item::INVALID_ITEM_ID;
    }
}

int EntityTreeRenderer::_entitiesScriptEngineCount = 0;

void EntityTreeRenderer::resetEntitiesScriptEngine() {
    _entitiesScriptEngine = scriptEngineFactory(ScriptEngine::ENTITY_CLIENT_SCRIPT, NO_SCRIPT,
                                                QString("about:Entities %1").arg(++_entitiesScriptEngineCount));
    _scriptingServices->registerScriptEngineWithApplicationServices(_entitiesScriptEngine);
    _entitiesScriptEngine->runInThread();
    auto entitiesScriptEngineProvider = qSharedPointerCast<EntitiesScriptEngineProvider>(_entitiesScriptEngine);
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    entityScriptingInterface->setEntitiesScriptEngine(entitiesScriptEngineProvider);

    // Connect mouse events to entity script callbacks
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::mousePressOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
        _entitiesScriptEngine->callEntityScriptMethod(entityID, "mousePressOnEntity", event);
    });
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::mouseDoublePressOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
        _entitiesScriptEngine->callEntityScriptMethod(entityID, "mouseDoublePressOnEntity", event);
    });
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::mouseMoveOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
        _entitiesScriptEngine->callEntityScriptMethod(entityID, "mouseMoveOnEntity", event);
        // FIXME: this is a duplicate of mouseMoveOnEntity, but it seems like some scripts might use this naming
        _entitiesScriptEngine->callEntityScriptMethod(entityID, "mouseMoveEvent", event);
    });
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::mouseReleaseOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
        _entitiesScriptEngine->callEntityScriptMethod(entityID, "mouseReleaseOnEntity", event);
    });

    connect(entityScriptingInterface.data(), &EntityScriptingInterface::clickDownOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
        _entitiesScriptEngine->callEntityScriptMethod(entityID, "clickDownOnEntity", event);
    });
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::holdingClickOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
        _entitiesScriptEngine->callEntityScriptMethod(entityID, "holdingClickOnEntity", event);
    });
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::clickReleaseOnEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
        _entitiesScriptEngine->callEntityScriptMethod(entityID, "clickReleaseOnEntity", event);
    });

    connect(entityScriptingInterface.data(), &EntityScriptingInterface::hoverEnterEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
        _entitiesScriptEngine->callEntityScriptMethod(entityID, "hoverEnterEntity", event);
    });
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::hoverOverEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
        _entitiesScriptEngine->callEntityScriptMethod(entityID, "hoverOverEntity", event);
    });
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::hoverLeaveEntity, _entitiesScriptEngine.data(), [&](const EntityItemID& entityID, const PointerEvent& event) {
        _entitiesScriptEngine->callEntityScriptMethod(entityID, "hoverLeaveEntity", event);
    });

    connect(_entitiesScriptEngine.data(), &ScriptEngine::entityScriptPreloadFinished, [&](const EntityItemID& entityID) {
        EntityItemPointer entity = getTree()->findEntityByID(entityID);
        if (entity) {
            entity->setScriptHasFinishedPreload(true);
        }
    });
}

void EntityTreeRenderer::clear() {
    leaveAllEntities();

    // unload and stop the engine
    if (_entitiesScriptEngine) {
        // do this here (instead of in deleter) to avoid marshalling unload signals back to this thread
        _entitiesScriptEngine->unloadAllEntityScripts();
        _entitiesScriptEngine->stop();
    }

    // reset the engine
    if (_wantScripts && !_shuttingDown) {
        resetEntitiesScriptEngine();
    }

    // remove all entities from the scene
    _space->clear();
    auto scene = _viewState->getMain3DScene();
    if (scene) {
        render::Transaction transaction;
        for (const auto& entry :  _entitiesInScene) {
            const auto& renderer = entry.second;
            renderer->removeFromScene(scene, transaction);
        }
        scene->enqueueTransaction(transaction);
    } else {
        qCWarning(entitiesrenderer) << "EntitityTreeRenderer::clear(), Unexpected null scene, possibly during application shutdown";
    }
    _entitiesInScene.clear();
    _renderablesToUpdate.clear();

    // reset the zone to the default (while we load the next scene)
    _layeredZones.clear();

    OctreeProcessor::clear();
}

void EntityTreeRenderer::reloadEntityScripts() {
    _entitiesScriptEngine->unloadAllEntityScripts();
    _entitiesScriptEngine->resetModuleCache();
    for (const auto& entry : _entitiesInScene) {
        const auto& renderer = entry.second;
        const auto& entity = renderer->getEntity();
        if (!entity->getScript().isEmpty()) {
            _entitiesScriptEngine->loadEntityScript(entity->getEntityItemID(), resolveScriptURL(entity->getScript()), true);
        }
    }
}

void EntityTreeRenderer::init() {
    OctreeProcessor::init();
    EntityTreePointer entityTree = std::static_pointer_cast<EntityTree>(_tree);

    if (_wantScripts) {
        resetEntitiesScriptEngine();
    }

    forceRecheckEntities(); // setup our state to force checking our inside/outsideness of entities

    connect(entityTree.get(), &EntityTree::deletingEntity, this, &EntityTreeRenderer::deletingEntity, Qt::QueuedConnection);
    connect(entityTree.get(), &EntityTree::addingEntity, this, &EntityTreeRenderer::addingEntity, Qt::QueuedConnection);
    connect(entityTree.get(), &EntityTree::entityScriptChanging,
            this, &EntityTreeRenderer::entityScriptChanging, Qt::QueuedConnection);
}

void EntityTreeRenderer::shutdown() {
    if (_entitiesScriptEngine) {
        _entitiesScriptEngine->disconnectNonEssentialSignals(); // disconnect all slots/signals from the script engine, except essential
    }
    _shuttingDown = true;

    clear(); // always clear() on shutdown
}

void EntityTreeRenderer::addPendingEntities(const render::ScenePointer& scene, render::Transaction& transaction) {
    PROFILE_RANGE_EX(simulation_physics, "AddToScene", 0xffff00ff, (uint64_t)_entitiesToAdd.size());
    PerformanceTimer pt("add");
    // Clear any expired entities
    // FIXME should be able to use std::remove_if, but it fails due to some
    // weird compilation error related to EntityItemID assignment operators
    for (auto itr = _entitiesToAdd.begin(); _entitiesToAdd.end() != itr; ) {
        if (itr->second.expired()) {
            _entitiesToAdd.erase(itr++);
        } else {
            ++itr;
        }
    }

    if (!_entitiesToAdd.empty()) {
        std::unordered_set<EntityItemID> processedIds;
        for (const auto& entry : _entitiesToAdd) {
            auto entity = entry.second.lock();
            if (!entity) {
                continue;
            }

            // Path to the parent transforms is not valid,
            // don't add to the scene graph yet
            if (!entity->isParentPathComplete()) {
                continue;
            }
            if (entity->getSpaceIndex() == -1) {
                std::unique_lock<std::mutex> lock(_spaceLock);
                auto spaceIndex = _space->allocateID();
                workload::Sphere sphere(entity->getWorldPosition(), entity->getBoundingRadius());
                workload::Transaction transaction;
                SpatiallyNestablePointer nestable = std::static_pointer_cast<SpatiallyNestable>(entity);
                transaction.reset(spaceIndex, sphere, workload::Owner(nestable));
                _space->enqueueTransaction(transaction);
                entity->setSpaceIndex(spaceIndex);
                connect(entity.get(), &EntityItem::spaceUpdate, this, &EntityTreeRenderer::handleSpaceUpdate, Qt::QueuedConnection);
            }

            auto entityID = entity->getEntityItemID();
            auto renderable = EntityRenderer::addToScene(*this, entity, scene, transaction);
            if (renderable) {
                _entitiesInScene.insert({ entityID, renderable });
                processedIds.insert(entityID);
            }
        }

        if (!processedIds.empty()) {
            for (const auto& processedId : processedIds) {
                _entitiesToAdd.erase(processedId);
            }
        }
    }
}

void EntityTreeRenderer::updateChangedEntities(const render::ScenePointer& scene, render::Transaction& transaction) {
    PROFILE_RANGE_EX(simulation_physics, "ChangeInScene", 0xffff00ff, (uint64_t)_changedEntities.size());
    PerformanceTimer pt("change");
    std::unordered_set<EntityItemID> changedEntities;
    _changedEntitiesGuard.withWriteLock([&] {
#if 0
        // FIXME Weird build failure in latest VC update that fails to compile when using std::swap
        changedEntities.swap(_changedEntities);
#else
        changedEntities.insert(_changedEntities.begin(), _changedEntities.end());
        _changedEntities.clear();
#endif
    });

    {
        PROFILE_RANGE_EX(simulation_physics, "CopyRenderables", 0xffff00ff, (uint64_t)changedEntities.size());
        for (const auto& entityId : changedEntities) {
            auto renderable = renderableForEntityId(entityId);
            if (renderable) {
                // only add valid renderables _renderablesToUpdate
                _renderablesToUpdate.insert({ entityId, renderable });
            }
        }
    }

    float expectedUpdateCost = _avgRenderableUpdateCost * _renderablesToUpdate.size();
    if (expectedUpdateCost < MAX_UPDATE_RENDERABLES_TIME_BUDGET) {
        // we expect to update all renderables within available time budget
        PROFILE_RANGE_EX(simulation_physics, "UpdateRenderables", 0xffff00ff, (uint64_t)_renderablesToUpdate.size());
        uint64_t updateStart = usecTimestampNow();
        for (const auto& entry : _renderablesToUpdate) {
            const auto& renderable = entry.second;
            assert(renderable); // only valid renderables are added to _renderablesToUpdate
            renderable->updateInScene(scene, transaction);
        }
        size_t numRenderables = _renderablesToUpdate.size() + 1; // add one to avoid divide by zero
        _renderablesToUpdate.clear();

        // compute average per-renderable update cost
        float cost = (float)(usecTimestampNow() - updateStart) / (float)(numRenderables);
        const float blend = 0.1f;
        _avgRenderableUpdateCost = (1.0f - blend) * _avgRenderableUpdateCost + blend * cost;
    } else {
        // we expect the cost to updating all renderables to exceed available time budget
        // so we first sort by priority and update in order until out of time

        class SortableRenderer: public PrioritySortUtil::Sortable {
        public:
            SortableRenderer(const EntityRendererPointer& renderer) : _renderer(renderer) { }

            glm::vec3 getPosition() const override { return _renderer->getEntity()->getWorldPosition(); }
            float getRadius() const override { return 0.5f * _renderer->getEntity()->getQueryAACube().getScale(); }
            uint64_t getTimestamp() const override { return _renderer->getUpdateTime(); }

            EntityRendererPointer getRenderer() const { return _renderer; }
        private:
            EntityRendererPointer _renderer;
        };

        // prioritize and sort the renderables
        uint64_t sortStart = usecTimestampNow();

        const auto& views = _viewState->getConicalViews();
        PrioritySortUtil::PriorityQueue<SortableRenderer> sortedRenderables(views);
        sortedRenderables.reserve(_renderablesToUpdate.size());
        {
            PROFILE_RANGE_EX(simulation_physics, "SortRenderables", 0xffff00ff, (uint64_t)_renderablesToUpdate.size());
            std::unordered_map<EntityItemID, EntityRendererPointer>::iterator itr = _renderablesToUpdate.begin();
            while (itr != _renderablesToUpdate.end()) {
                assert(itr->second); // only valid renderables are added to _renderablesToUpdate
                sortedRenderables.push(SortableRenderer(itr->second));
                ++itr;
            }
        }
        {
            PROFILE_RANGE_EX(simulation_physics, "UpdateRenderables", 0xffff00ff, sortedRenderables.size());

            // compute remaining time budget
            uint64_t updateStart = usecTimestampNow();
            uint64_t timeBudget = MIN_SORTED_UPDATE_RENDERABLES_TIME_BUDGET;
            uint64_t sortCost = updateStart - sortStart;
            if (sortCost < MAX_UPDATE_RENDERABLES_TIME_BUDGET - MIN_SORTED_UPDATE_RENDERABLES_TIME_BUDGET) {
                timeBudget = MAX_UPDATE_RENDERABLES_TIME_BUDGET - sortCost;
            }
            uint64_t expiry = updateStart + timeBudget;

            // process the sorted renderables
            size_t numSorted = sortedRenderables.size();
            const auto& sortedRenderablesVector = sortedRenderables.getSortedVector();
            for (const auto& sortedRenderable : sortedRenderablesVector) {
                if (usecTimestampNow() > expiry) {
                    break;
                }
                const auto& renderable = sortedRenderable.getRenderer();
                renderable->updateInScene(scene, transaction);
                _renderablesToUpdate.erase(renderable->getEntity()->getID());
            }

            // compute average per-renderable update cost
            size_t numUpdated = numSorted - sortedRenderables.size() + 1; // add one to avoid divide by zero
            float cost = (float)(usecTimestampNow() - updateStart) / (float)(numUpdated);
            const float blend = 0.1f;
            _avgRenderableUpdateCost = (1.0f - blend) * _avgRenderableUpdateCost + blend * cost;
        }
    }
}

void EntityTreeRenderer::update(bool simulate) {
    PROFILE_RANGE(simulation_physics, "ETR::update");
    PerformanceTimer perfTimer("ETRupdate");
    if (_tree && !_shuttingDown) {
        EntityTreePointer tree = std::static_pointer_cast<EntityTree>(_tree);

        // here we update _currentFrame and _lastAnimated and sync with the server properties.
        {
            PerformanceTimer perfTimer("tree::update");
            tree->update(simulate);
        }

        {   // Update the rendereable entities as needed
            PROFILE_RANGE(simulation_physics, "Scene");
            PerformanceTimer sceneTimer("scene");
            auto scene = _viewState->getMain3DScene();
            if (scene) {
                render::Transaction transaction;
                addPendingEntities(scene, transaction);

                updateChangedEntities(scene, transaction);
                scene->enqueueTransaction(transaction);
            }
        }
        {
            PerformanceTimer perfTimer("workload::transaction");
            workload::Transaction spaceTransaction;
            {   // update proxies in the workload::Space
                std::unique_lock<std::mutex> lock(_spaceLock);
                spaceTransaction.update(_spaceUpdates);
                _spaceUpdates.clear();
            }
            {
                std::vector<int32_t> staleProxies;
                tree->swapStaleProxies(staleProxies);
                spaceTransaction.remove(staleProxies);
                {
                    std::unique_lock<std::mutex> lock(_spaceLock);
                    _space->enqueueTransaction(spaceTransaction);
                }
            }
        }

        if (simulate) {
            // Handle enter/leave entity logic
            checkEnterLeaveEntities();

            // Even if we're not moving the mouse, if we started clicking on an entity and we have
            // not yet released the hold then this is still considered a holdingClickOnEntity event
            // and we want to simulate this message here as well as in mouse move
            if (_lastPointerEventValid && !_currentClickingOnEntityID.isInvalidID()) {
                emit DependencyManager::get<EntityScriptingInterface>()->holdingClickOnEntity(_currentClickingOnEntityID, _lastPointerEvent);
            }
        }

    }
}

void EntityTreeRenderer::handleSpaceUpdate(std::pair<int32_t, glm::vec4> proxyUpdate) {
    std::unique_lock<std::mutex> lock(_spaceLock);
    _spaceUpdates.emplace_back(proxyUpdate.first, proxyUpdate.second);
}

bool EntityTreeRenderer::findBestZoneAndMaybeContainingEntities(QVector<EntityItemID>* entitiesContainingAvatar) {
    bool didUpdate = false;
    float radius = 0.01f; // for now, assume 0.01 meter radius, because we actually check the point inside later
    QVector<QUuid> entityIDs;

    // find the entities near us
    // don't let someone else change our tree while we search
    _tree->withReadLock([&] {
        auto entityTree = std::static_pointer_cast<EntityTree>(_tree);

        // FIXME - if EntityTree had a findEntitiesContainingPoint() this could theoretically be a little faster
        entityTree->evalEntitiesInSphere(_avatarPosition, radius,
            PickFilter(PickFilter::getBitMask(PickFilter::FlagBit::DOMAIN_ENTITIES) | PickFilter::getBitMask(PickFilter::FlagBit::AVATAR_ENTITIES)), entityIDs);

        LayeredZones oldLayeredZones(std::move(_layeredZones));
        _layeredZones.clear();

        // create a list of entities that actually contain the avatar's position
        for (auto& entityID : entityIDs) {
            auto entity = entityTree->findEntityByID(entityID);
            if (!entity) {
                continue;
            }

            auto isZone = entity->getType() == EntityTypes::Zone;
            auto hasScript = !entity->getScript().isEmpty();

            // only consider entities that are zones or have scripts, all other entities can
            // be ignored because they can't have events fired on them.
            // FIXME - this could be optimized further by determining if the script is loaded
            // and if it has either an enterEntity or leaveEntity method
            //
            // also, don't flag a scripted entity as containing the avatar until the script is loaded,
            // so that the script is awake in time to receive the "entityEntity" call (even if the entity is a zone).
            bool contains = false;
            bool scriptHasLoaded = hasScript && entity->isScriptPreloadFinished();
            if (isZone || scriptHasLoaded) {
                contains = entity->contains(_avatarPosition);
            }

            if (contains) {
                // if this entity is a zone and visible, add it to our layered zones
                if (isZone && entity->getVisible() && renderableForEntity(entity)) {
                    _layeredZones.insert(std::dynamic_pointer_cast<ZoneEntityItem>(entity));
                }

                if ((!hasScript && isZone) || scriptHasLoaded) {
                    if (entitiesContainingAvatar) {
                        *entitiesContainingAvatar << entity->getEntityItemID();
                    }
                }
            }
        }

        // check if our layered zones have changed
        if ((_layeredZones.empty() && oldLayeredZones.empty()) || (!oldLayeredZones.empty() && _layeredZones.contains(oldLayeredZones))) {
            return;
        }

        applyLayeredZones();

        didUpdate = true;
    });

    return didUpdate;
}

bool EntityTreeRenderer::checkEnterLeaveEntities() {
    PROFILE_RANGE(simulation_physics, "EnterLeave");
    PerformanceTimer perfTimer("enterLeave");
    auto now = usecTimestampNow();
    bool didUpdate = false;

    if (_tree && !_shuttingDown) {
        glm::vec3 avatarPosition = _viewState->getAvatarPosition();

        // we want to check our enter/leave state if we've moved a significant amount, or
        // if some amount of time has elapsed since we last checked. We check the time
        // elapsed because zones or entities might have been created "around us" while we've
        // been stationary
        auto movedEnough = glm::distance(avatarPosition, _avatarPosition) > ZONE_CHECK_DISTANCE;
        auto enoughTimeElapsed = (now - _lastZoneCheck) > ZONE_CHECK_INTERVAL;
        
        if (movedEnough || enoughTimeElapsed) {
            _avatarPosition = avatarPosition;
            _lastZoneCheck = now;
            QVector<EntityItemID> entitiesContainingAvatar;
            didUpdate = findBestZoneAndMaybeContainingEntities(&entitiesContainingAvatar);
            
            // Note: at this point we don't need to worry about the tree being locked, because we only deal with
            // EntityItemIDs from here. The callEntityScriptMethod() method is robust against attempting to call scripts
            // for entity IDs that no longer exist.

            // for all of our previous containing entities, if they are no longer containing then send them a leave event
            foreach(const EntityItemID& entityID, _currentEntitiesInside) {
                if (!entitiesContainingAvatar.contains(entityID)) {
                    emit leaveEntity(entityID);
                    if (_entitiesScriptEngine) {
                        _entitiesScriptEngine->callEntityScriptMethod(entityID, "leaveEntity");
                    }
                }
            }

            // for all of our new containing entities, if they weren't previously containing then send them an enter event
            foreach(const EntityItemID& entityID, entitiesContainingAvatar) {
                if (!_currentEntitiesInside.contains(entityID)) {
                    emit enterEntity(entityID);
                    if (_entitiesScriptEngine) {
                        _entitiesScriptEngine->callEntityScriptMethod(entityID, "enterEntity");
                    }
                }
            }
            _currentEntitiesInside = entitiesContainingAvatar;
        }
    }
    return didUpdate;
}

void EntityTreeRenderer::leaveAllEntities() {
    if (_tree && !_shuttingDown) {

        // for all of our previous containing entities, if they are no longer containing then send them a leave event
        foreach(const EntityItemID& entityID, _currentEntitiesInside) {
            emit leaveEntity(entityID);
            if (_entitiesScriptEngine) {
                _entitiesScriptEngine->callEntityScriptMethod(entityID, "leaveEntity");
            }
        }
        _currentEntitiesInside.clear();
        forceRecheckEntities();
    }
}

void EntityTreeRenderer::forceRecheckEntities() {
    // make sure our "last avatar position" is something other than our current position, 
    // so that on our next chance, we'll check for enter/leave entity events.
    _avatarPosition = _viewState->getAvatarPosition() + glm::vec3((float)TREE_SCALE);
}

bool EntityTreeRenderer::applyLayeredZones() {
    // from the list of zones we are going to build a selection list the Render Item corresponding to the zones
    // in the expected layered order and update the scene with it
    auto scene = _viewState->getMain3DScene();
    if (scene) {
        render::Transaction transaction;
        render::ItemIDs list;
        for (auto& zone : _layeredZones) {
            auto id = renderableIdForEntity(zone.zone);
            // The zone may not have been rendered yet.
            if (id != render::Item::INVALID_ITEM_ID) {
                list.push_back(id);
            }
        }
        render::Selection selection("RankedZones", list);
        transaction.resetSelection(selection);

        scene->enqueueTransaction(transaction);
    } else {
        qCWarning(entitiesrenderer) << "EntityTreeRenderer::applyLayeredZones(), Unexpected null scene, possibly during application shutdown";
    }

    return true;
}

void EntityTreeRenderer::processEraseMessage(ReceivedMessage& message, const SharedNodePointer& sourceNode) {
    OCTREE_PACKET_FLAGS flags;
    message.readPrimitive(&flags);

    OCTREE_PACKET_SEQUENCE sequence;
    message.readPrimitive(&sequence);

    _lastOctreeMessageSequence = sequence;
    message.seek(0);
    std::static_pointer_cast<EntityTree>(_tree)->processEraseMessage(message, sourceNode);
}

void EntityTreeRenderer::connectSignalsToSlots(EntityScriptingInterface* entityScriptingInterface) {
    connect(this, &EntityTreeRenderer::enterEntity, entityScriptingInterface, &EntityScriptingInterface::enterEntity);
    connect(this, &EntityTreeRenderer::leaveEntity, entityScriptingInterface, &EntityScriptingInterface::leaveEntity);
    connect(this, &EntityTreeRenderer::collisionWithEntity, entityScriptingInterface, &EntityScriptingInterface::collisionWithEntity);

    connect(DependencyManager::get<SceneScriptingInterface>().data(), &SceneScriptingInterface::shouldRenderEntitiesChanged, this, &EntityTreeRenderer::updateEntityRenderStatus, Qt::QueuedConnection);
}

static glm::vec2 projectOntoEntityXYPlane(EntityItemPointer entity, const PickRay& pickRay, const RayToEntityIntersectionResult& rayPickResult) {

    if (entity) {

        glm::vec3 entityPosition = entity->getWorldPosition();
        glm::quat entityRotation = entity->getWorldOrientation();
        glm::vec3 entityDimensions = entity->getScaledDimensions();
        glm::vec3 entityRegistrationPoint = entity->getRegistrationPoint();

        // project the intersection point onto the local xy plane of the object.
        float distance;
        glm::vec3 planePosition = entityPosition;
        glm::vec3 planeNormal = entityRotation * Vectors::UNIT_Z;
        glm::vec3 rayDirection = pickRay.direction;
        glm::vec3 rayStart = pickRay.origin;
        glm::vec3 p;
        if (rayPlaneIntersection(planePosition, planeNormal, rayStart, rayDirection, distance)) {
            p = rayStart + rayDirection * distance;
        } else {
            p = rayPickResult.intersection;
        }
        glm::vec3 localP = glm::inverse(entityRotation) * (p - entityPosition);
        glm::vec3 normalizedP = (localP / entityDimensions) + entityRegistrationPoint;
        return glm::vec2(normalizedP.x * entityDimensions.x,
                         (1.0f - normalizedP.y) * entityDimensions.y);  // flip y-axis
    } else {
        return glm::vec2();
    }
}

static uint32_t toPointerButtons(const QMouseEvent& event) {
    uint32_t buttons = 0;
    buttons |= event.buttons().testFlag(Qt::LeftButton) ? PointerEvent::PrimaryButton : 0;
    buttons |= event.buttons().testFlag(Qt::RightButton) ? PointerEvent::SecondaryButton : 0;
    buttons |= event.buttons().testFlag(Qt::MiddleButton) ? PointerEvent::TertiaryButton : 0;
    return buttons;
}

static PointerEvent::Button toPointerButton(const QMouseEvent& event) {
    switch (event.button()) {
    case Qt::LeftButton:
        return PointerEvent::PrimaryButton;
    case Qt::RightButton:
        return PointerEvent::SecondaryButton;
    case Qt::MiddleButton:
        return PointerEvent::TertiaryButton;
    default:
        return PointerEvent::NoButtons;
    }
}

void EntityTreeRenderer::mousePressEvent(QMouseEvent* event) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }

    PerformanceTimer perfTimer("EntityTreeRenderer::mousePressEvent");
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    PickRay ray = _viewState->computePickRay(event->x(), event->y());
    RayToEntityIntersectionResult rayPickResult = _getPrevRayPickResultOperator(_mouseRayPickID);
    EntityItemPointer entity;
    if (rayPickResult.intersects && (entity = getTree()->findEntityByID(rayPickResult.entityID))) {
        auto properties = entity->getProperties();
        QString urlString = properties.getHref();
        QUrl url = QUrl(urlString, QUrl::StrictMode);
        if (url.isValid() && !url.isEmpty()){
            DependencyManager::get<AddressManager>()->handleLookupString(urlString);
        }

        glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Press, PointerManager::MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit entityScriptingInterface->mousePressOnEntity(rayPickResult.entityID, pointerEvent);

        _currentClickingOnEntityID = rayPickResult.entityID;
        emit entityScriptingInterface->clickDownOnEntity(_currentClickingOnEntityID, pointerEvent);

        _lastPointerEvent = pointerEvent;
        _lastPointerEventValid = true;

    } else {
        emit entityScriptingInterface->mousePressOffEntity();
    }
}

void EntityTreeRenderer::mouseDoublePressEvent(QMouseEvent* event) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }

    PerformanceTimer perfTimer("EntityTreeRenderer::mouseDoublePressEvent");
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    PickRay ray = _viewState->computePickRay(event->x(), event->y());
    RayToEntityIntersectionResult rayPickResult = _getPrevRayPickResultOperator(_mouseRayPickID);
    EntityItemPointer entity;
    if (rayPickResult.intersects && (entity = getTree()->findEntityByID(rayPickResult.entityID))) {
        glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Press, PointerManager::MOUSE_POINTER_ID,
            pos2D, rayPickResult.intersection,
            rayPickResult.surfaceNormal, ray.direction,
            toPointerButton(*event), toPointerButtons(*event), Qt::NoModifier);

        emit entityScriptingInterface->mouseDoublePressOnEntity(rayPickResult.entityID, pointerEvent);

        _currentClickingOnEntityID = rayPickResult.entityID;
        emit entityScriptingInterface->clickDownOnEntity(_currentClickingOnEntityID, pointerEvent);

        _lastPointerEvent = pointerEvent;
        _lastPointerEventValid = true;
    } else {
        emit entityScriptingInterface->mouseDoublePressOffEntity();
    }
}

void EntityTreeRenderer::mouseReleaseEvent(QMouseEvent* event) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }

    PerformanceTimer perfTimer("EntityTreeRenderer::mouseReleaseEvent");
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    PickRay ray = _viewState->computePickRay(event->x(), event->y());
    RayToEntityIntersectionResult rayPickResult = _getPrevRayPickResultOperator(_mouseRayPickID);
    EntityItemPointer entity;
    if (rayPickResult.intersects && (entity = getTree()->findEntityByID(rayPickResult.entityID))) {
        // qCDebug(entitiesrenderer) << "mouseReleaseEvent over entity:" << rayPickResult.entityID;

        glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Release, PointerManager::MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit entityScriptingInterface->mouseReleaseOnEntity(rayPickResult.entityID, pointerEvent);

        _lastPointerEvent = pointerEvent;
        _lastPointerEventValid = true;
    }

    // Even if we're no longer intersecting with an entity, if we started clicking on it, and now
    // we're releasing the button, then this is considered a clickReleaseOn event
    if (!_currentClickingOnEntityID.isInvalidID()) {
        glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Release, PointerManager::MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit entityScriptingInterface->clickReleaseOnEntity(_currentClickingOnEntityID, pointerEvent);
    }

    // makes it the unknown ID, we just released so we can't be clicking on anything
    _currentClickingOnEntityID = UNKNOWN_ENTITY_ID;
}

void EntityTreeRenderer::mouseMoveEvent(QMouseEvent* event) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }

    PerformanceTimer perfTimer("EntityTreeRenderer::mouseMoveEvent");
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    PickRay ray = _viewState->computePickRay(event->x(), event->y());
    RayToEntityIntersectionResult rayPickResult = _getPrevRayPickResultOperator(_mouseRayPickID);
    EntityItemPointer entity;
    if (rayPickResult.intersects && (entity = getTree()->findEntityByID(rayPickResult.entityID))) {
        glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Move, PointerManager::MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit entityScriptingInterface->mouseMoveOnEntity(rayPickResult.entityID, pointerEvent);

        // handle the hover logic...

        // if we were previously hovering over an entity, and this new entity is not the same as our previous entity
        // then we need to send the hover leave.
        if (!_currentHoverOverEntityID.isInvalidID() && rayPickResult.entityID != _currentHoverOverEntityID) {
            glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
            PointerEvent pointerEvent(PointerEvent::Move, PointerManager::MOUSE_POINTER_ID,
                                      pos2D, rayPickResult.intersection,
                                      rayPickResult.surfaceNormal, ray.direction,
                                      toPointerButton(*event), toPointerButtons(*event),
                                      Qt::NoModifier); // TODO -- check for modifier keys?

            emit entityScriptingInterface->hoverLeaveEntity(_currentHoverOverEntityID, pointerEvent);
        }

        // If the new hover entity does not match the previous hover entity then we are entering the new one
        // this is true if the _currentHoverOverEntityID is known or unknown
        if (rayPickResult.entityID != _currentHoverOverEntityID) {
            emit entityScriptingInterface->hoverEnterEntity(rayPickResult.entityID, pointerEvent);
        }

        // and finally, no matter what, if we're intersecting an entity then we're definitely hovering over it, and
        // we should send our hover over event
        emit entityScriptingInterface->hoverOverEntity(rayPickResult.entityID, pointerEvent);

        // remember what we're hovering over
        _currentHoverOverEntityID = rayPickResult.entityID;

        _lastPointerEvent = pointerEvent;
        _lastPointerEventValid = true;

    } else {
        // handle the hover logic...
        // if we were previously hovering over an entity, and we're no longer hovering over any entity then we need to
        // send the hover leave for our previous entity
        if (!_currentHoverOverEntityID.isInvalidID()) {
            glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
            PointerEvent pointerEvent(PointerEvent::Move, PointerManager::MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                      toPointerButton(*event), toPointerButtons(*event),
                                      Qt::NoModifier); // TODO -- check for modifier keys?

            emit entityScriptingInterface->hoverLeaveEntity(_currentHoverOverEntityID, pointerEvent);
            _currentHoverOverEntityID = UNKNOWN_ENTITY_ID; // makes it the unknown ID

            _lastPointerEvent = pointerEvent;
            _lastPointerEventValid = true;
        }
    }
}

void EntityTreeRenderer::deletingEntity(const EntityItemID& entityID) {
    // If it's in a pending queue, remove it
    _renderablesToUpdate.erase(entityID);
    _entitiesToAdd.erase(entityID);

    auto itr = _entitiesInScene.find(entityID);
    if (_entitiesInScene.end() == itr) {
        // Not in the scene, and no longer potentially in the pending queue, we're done
        return;
    }

    if (_tree && !_shuttingDown && _entitiesScriptEngine) {
        _entitiesScriptEngine->unloadEntityScript(entityID, true);
    }

    auto scene = _viewState->getMain3DScene();
    if (!scene) {
        qCWarning(entitiesrenderer) << "EntityTreeRenderer::deletingEntity(), Unexpected null scene, possibly during application shutdown";
        return;
    }

    auto renderable = itr->second;
    _entitiesInScene.erase(itr);

    if (!renderable) {
        qCWarning(entitiesrenderer) << "EntityTreeRenderer::deletingEntity(), trying to remove non-renderable entity";
        return;
    }

    forceRecheckEntities(); // reset our state to force checking our inside/outsideness of entities

    // here's where we remove the entity payload from the scene
    render::Transaction transaction;
    renderable->removeFromScene(scene, transaction);
    scene->enqueueTransaction(transaction);
}

void EntityTreeRenderer::addingEntity(const EntityItemID& entityID) {
    forceRecheckEntities(); // reset our state to force checking our inside/outsideness of entities
    checkAndCallPreload(entityID);
    auto entity = std::static_pointer_cast<EntityTree>(_tree)->findEntityByID(entityID);
    if (entity) {
        _entitiesToAdd.insert({ entity->getEntityItemID(),  entity });
    }
}

void EntityTreeRenderer::entityScriptChanging(const EntityItemID& entityID, bool reload) {
    checkAndCallPreload(entityID, reload, true);
}

void EntityTreeRenderer::checkAndCallPreload(const EntityItemID& entityID, bool reload, bool unloadFirst) {
    if (_tree && !_shuttingDown) {
        EntityItemPointer entity = getTree()->findEntityByEntityItemID(entityID);
        if (!entity) {
            return;
        }
        bool shouldLoad = entity->shouldPreloadScript() && _entitiesScriptEngine;
        QString scriptUrl = entity->getScript();
        if ((shouldLoad && unloadFirst) || scriptUrl.isEmpty()) {
            if (_entitiesScriptEngine) {
            _entitiesScriptEngine->unloadEntityScript(entityID);
            }
            entity->scriptHasUnloaded();
        }
        if (shouldLoad) {
            entity->setScriptHasFinishedPreload(false);
            _entitiesScriptEngine->loadEntityScript(entityID, resolveScriptURL(scriptUrl), reload);
            entity->scriptHasPreloaded();
        }
    }
}

void EntityTreeRenderer::playEntityCollisionSound(const EntityItemPointer& entity, const Collision& collision) {
    assert((bool)entity);
    auto renderable = renderableForEntity(entity);
    if (!renderable) { 
        return; 
    }
    
    SharedSoundPointer collisionSound = renderable->getCollisionSound();
    if (!collisionSound) {
        return;
    }
    bool success = false;
    AACube minAACube = entity->getMinimumAACube(success);
    if (!success) {
        return;
    }
    float mass = entity->computeMass();

    const float COLLISION_PENETRATION_TO_VELOCITY = 50.0f; // as a subsitute for RELATIVE entity->getVelocity()
    // The collision.penetration is a pretty good indicator of changed velocity AFTER the initial contact,
    // but that first contact depends on exactly where we hit in the physics step.
    // We can get a more consistent initial-contact energy reading by using the changed velocity.
    // Note that velocityChange is not a good indicator for continuing collisions, because it does not distinguish
    // between bounce and sliding along a surface.
    const float speedSquared = (collision.type == CONTACT_EVENT_TYPE_START) ?
        glm::length2(collision.velocityChange) :
        glm::length2(collision.penetration) * COLLISION_PENETRATION_TO_VELOCITY;
    const float energy = mass * speedSquared / 2.0f;
    const float COLLISION_ENERGY_AT_FULL_VOLUME = (collision.type == CONTACT_EVENT_TYPE_START) ? 150.0f : 5.0f;
    const float COLLISION_MINIMUM_VOLUME = 0.005f;
    const float energyFactorOfFull = fmin(1.0f, energy / COLLISION_ENERGY_AT_FULL_VOLUME);
    if (energyFactorOfFull < COLLISION_MINIMUM_VOLUME) {
        return;
    }
    // Quiet sound aren't really heard at all, so we can compress everything to the range [1-c, 1], if we play it all.
    const float COLLISION_SOUND_COMPRESSION_RANGE = 1.0f; // This section could be removed when the value is 1, but let's see how it goes.
    const float volume = (energyFactorOfFull * COLLISION_SOUND_COMPRESSION_RANGE) + (1.0f - COLLISION_SOUND_COMPRESSION_RANGE);

    // Shift the pitch down by ln(1 + (size / COLLISION_SIZE_FOR_STANDARD_PITCH)) / ln(2)
    const float COLLISION_SIZE_FOR_STANDARD_PITCH = 0.2f;
    const float stretchFactor = logf(1.0f + (minAACube.getLargestDimension() / COLLISION_SIZE_FOR_STANDARD_PITCH)) / logf(2.0f);

    AudioInjectorOptions options;
    options.stereo = collisionSound->isStereo();
    options.position = collision.contactPoint;
    options.volume = volume;
    options.pitch = 1.0f / stretchFactor;

    AudioInjector::playSoundAndDelete(collisionSound, options);
}

void EntityTreeRenderer::entityCollisionWithEntity(const EntityItemID& idA, const EntityItemID& idB,
                                                    const Collision& collision) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }

    EntityTreePointer entityTree = std::static_pointer_cast<EntityTree>(_tree);
    const QUuid& myNodeID = DependencyManager::get<NodeList>()->getSessionUUID();

    // trigger scripted collision sounds and events for locally owned objects
    EntityItemPointer entityA = entityTree->findEntityByEntityItemID(idA);
    EntityItemPointer entityB = entityTree->findEntityByEntityItemID(idB);
    if ((bool)entityA && (bool)entityB) {
        QUuid entityASimulatorID = entityA->getSimulatorID();
        QUuid entityBSimulatorID = entityB->getSimulatorID();
        bool entityAIsDynamic = entityA->getDynamic();
        bool entityBIsDynamic = entityB->getDynamic();

#ifdef WANT_DEBUG
        bool bothEntitiesStatic = !entityAIsDynamic && !entityBIsDynamic;
        if (bothEntitiesStatic) {
            qCDebug(entities) << "A collision has occurred between two static entities!";
            qCDebug(entities) << "Entity A ID:" << entityA->getID();
            qCDebug(entities) << "Entity B ID:" << entityB->getID();
        }
        assert(!bothEntitiesStatic);
#endif

        if ((myNodeID == entityASimulatorID && entityAIsDynamic) || (myNodeID == entityBSimulatorID && (!entityAIsDynamic || entityASimulatorID.isNull()))) {
            playEntityCollisionSound(entityA, collision);
            emit collisionWithEntity(idA, idB, collision);
            if (_entitiesScriptEngine) {
                _entitiesScriptEngine->callEntityScriptMethod(idA, "collisionWithEntity", idB, collision);
            }
        }

        if ((myNodeID == entityBSimulatorID && entityBIsDynamic) || (myNodeID == entityASimulatorID && (!entityBIsDynamic || entityBSimulatorID.isNull()))) {
            playEntityCollisionSound(entityB, collision);
            // since we're swapping A and B we need to send the inverted collision
            Collision invertedCollision(collision);
            invertedCollision.invert();
            emit collisionWithEntity(idB, idA, invertedCollision);
            if (_entitiesScriptEngine) {
                _entitiesScriptEngine->callEntityScriptMethod(idB, "collisionWithEntity", idA, invertedCollision);
            }
        }
    }
}

void EntityTreeRenderer::updateEntityRenderStatus(bool shouldRenderEntities) {
    if (DependencyManager::get<SceneScriptingInterface>()->shouldRenderEntities()) {
        for (auto entityID : _entityIDsLastInScene) {
            addingEntity(entityID);
        }
        _entityIDsLastInScene.clear();
    } else {
        _entityIDsLastInScene.clear();
        using MapEntry = std::pair<EntityItemID, EntityRendererPointer>;
        std::transform(_entitiesInScene.begin(), _entitiesInScene.end(), 
            std::back_inserter(_entityIDsLastInScene), [](const MapEntry& entry) { return entry.first; });

        for (auto entityID : _entityIDsLastInScene) {
            // FIXME - is this really right? do we want to do the deletingEntity() code or just remove from the scene.
            deletingEntity(entityID);
        }
    }
}

void EntityTreeRenderer::updateZone(const EntityItemID& id) {
    // Get in the zone!
    auto zone = std::dynamic_pointer_cast<ZoneEntityItem>(getTree()->findEntityByEntityItemID(id));
    if (zone && zone->contains(_avatarPosition)) {
        _layeredZones.update(zone);
    }
}

EntityTreeRenderer::LayeredZones::LayeredZones(LayeredZones&& other) {
    // In a swap:
    // > All iterators and references remain valid. The past-the-end iterator is invalidated.
    bool isSkyboxLayerValid = (other._skyboxLayer != other.end());

    swap(other);
    _map.swap(other._map);
    _skyboxLayer = other._skyboxLayer;

    if (!isSkyboxLayerValid) {
        _skyboxLayer = end();
    }
}

void EntityTreeRenderer::LayeredZones::clear() {
    std::set<LayeredZone>::clear();
    _map.clear();
    _skyboxLayer = end();
}

std::pair<EntityTreeRenderer::LayeredZones::iterator, bool> EntityTreeRenderer::LayeredZones::insert(const LayeredZone& layer) {
    iterator it;
    bool success;
    std::tie(it, success) = std::set<LayeredZone>::insert(layer);

    if (success) {
        _map.emplace(it->id, it);
    }

    return { it, success };
}

void EntityTreeRenderer::LayeredZones::update(std::shared_ptr<ZoneEntityItem> zone) {
    bool isVisible = zone->isVisible();

    if (empty() && isVisible) {
        // there are no zones: set this one
        insert(zone);
        return;
    } else {
        LayeredZone zoneLayer(zone);

        // find this zone's layer, if it exists
        iterator layer = end();
        auto it = _map.find(zoneLayer.id);
        if (it != _map.end()) {
            layer = it->second;
            // if the volume changed, we need to resort the layer (reinsertion)
            // if the visibility changed, we need to erase the layer
            if (zoneLayer.volume != layer->volume || !isVisible) {
                erase(layer);
                _map.erase(it);
                layer = end();
            }
        }

        // (re)insert this zone's layer if necessary
        if (layer == end() && isVisible) {
            std::tie(layer, std::ignore) = insert(zoneLayer);
            _map.emplace(layer->id, layer);
        }
    }
}

bool EntityTreeRenderer::LayeredZones::contains(const LayeredZones& other) {
    bool result = std::equal(other.begin(), other._skyboxLayer, begin());
    if (result) {
        // if valid, set the _skyboxLayer from the other LayeredZones
        _skyboxLayer = std::next(begin(), std::distance(other.begin(), other._skyboxLayer));
    }
    return result;
}

CalculateEntityLoadingPriority EntityTreeRenderer::_calculateEntityLoadingPriorityFunc = [](const EntityItem& item) -> float {
    return 0.0f;
};

bool EntityTreeRenderer::wantsKeyboardFocus(const EntityItemID& id) const {
    auto renderable = renderableForEntityId(id);
    if (!renderable) {
        return false;
    }
    return renderable->wantsKeyboardFocus();
}

QObject* EntityTreeRenderer::getEventHandler(const EntityItemID& id) {
    auto renderable = renderableForEntityId(id);
    if (!renderable) {
        return nullptr;
    }
    return renderable->getEventHandler();

}

bool EntityTreeRenderer::wantsHandControllerPointerEvents(const EntityItemID& id) const {
    auto renderable = renderableForEntityId(id);
    if (!renderable) {
        return false;
    }
    return renderable->wantsHandControllerPointerEvents();
}

void EntityTreeRenderer::setProxyWindow(const EntityItemID& id, QWindow* proxyWindow) {
    auto renderable = renderableForEntityId(id);
    if (renderable) {
        renderable->setProxyWindow(proxyWindow);
    }
}

void EntityTreeRenderer::setCollisionSound(const EntityItemID& id, const SharedSoundPointer& sound) {
    auto renderable = renderableForEntityId(id);
    if (renderable) {
        renderable->setCollisionSound(sound);
    }
}
EntityItemPointer EntityTreeRenderer::getEntity(const EntityItemID& id) {
    EntityItemPointer result;
    auto renderable = renderableForEntityId(id);
    if (renderable) {
        result = renderable->getEntity();
    }
    return result;
}

void EntityTreeRenderer::onEntityChanged(const EntityItemID& id) {
    _changedEntitiesGuard.withWriteLock([&] {
        _changedEntities.insert(id);
    });
}

EntityEditPacketSender* EntityTreeRenderer::getPacketSender() {
    EntityTreePointer tree = getTree();
    EntitySimulationPointer simulation = tree ? tree->getSimulation() : nullptr;
    PhysicalEntitySimulationPointer peSimulation = std::static_pointer_cast<PhysicalEntitySimulation>(simulation);
    EntityEditPacketSender* packetSender = peSimulation ? peSimulation->getPacketSender() : nullptr;
    return packetSender;
}
