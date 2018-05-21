#ifndef ABSTRACTRESTAPI_P_H
#define ABSTRACTRESTAPI_P_H

#include "proofcore/proofobject_p.h"
#include "proofcore/objectscache.h"
#include "proofnetwork/restclient.h"
#include "proofnetwork/abstractrestapi.h"
#include "proofnetwork/proofnetwork_global.h"

#include <QSslError>
#include <QHash>
#include <QVariant>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonArray>
#include <QNetworkReply>
#include <QMutex>

#include <functional>
#include <atomic>

class QTimer;
class QNetworkReply;

namespace Proof {
using RestAnswerHandler = std::function<void(qulonglong, QNetworkReply *)>;
class AbstractRestApi;
class PROOF_NETWORK_EXPORT AbstractRestApiPrivate : public ProofObjectPrivate
{
    Q_DECLARE_PUBLIC(AbstractRestApi)
public:
    AbstractRestApiPrivate() : ProofObjectPrivate() {}

    QNetworkReply *get(qulonglong &operationId, RestAnswerHandler &&handler, const QString &method, const QUrlQuery &query = QUrlQuery());
    QNetworkReply *post(qulonglong &operationId, RestAnswerHandler &&handler, const QString &method, const QUrlQuery &query = QUrlQuery(),
                        const QByteArray &body = "");
    QNetworkReply *post(qulonglong &operationId, RestAnswerHandler &&handler, const QString &method, const QUrlQuery &query,
                        QHttpMultiPart *multiParts);
    QNetworkReply *put(qulonglong &operationId, RestAnswerHandler &&handler, const QString &method, const QUrlQuery &query = QUrlQuery(),
                        const QByteArray &body = "");
    QNetworkReply *patch(qulonglong &operationId, RestAnswerHandler &&handler, const QString &method, const QUrlQuery &query = QUrlQuery(),
                        const QByteArray &body = "");
    QNetworkReply *deleteResource(qulonglong &operationId, RestAnswerHandler &&handler, const QString &method, const QUrlQuery &query = QUrlQuery());

    virtual void replyFinished(qulonglong operationId, QNetworkReply *reply, bool forceUserFriendly = false);
    virtual void replyErrorOccurred(qulonglong operationId, QNetworkReply *reply, bool forceUserFriendly = false);
    virtual void sslErrorsOccurred(qulonglong operationId, QNetworkReply *reply, const QList<QSslError> &errors, bool forceUserFriendly = false);
    virtual void cleanupReply(qulonglong operationId, QNetworkReply *reply);

    void notifyAboutJsonParseError(qulonglong operationId, QJsonParseError error);

    void clearReplies();

    //TODO: 1.0: add error message transmission from DTO
    template<class EntityKey, class Entity>
    QSharedPointer<Entity> parseEntity(QNetworkReply *reply, ObjectsCache<EntityKey, Entity> &cache,
                                       std::function<EntityKey(Entity *)> &&cacheKey, qulonglong operationId)
    {
        QJsonObject obj = parseEntityObject(reply, operationId);
        if (obj.isEmpty())
            return QSharedPointer<Entity>();
        return parseEntity<EntityKey, Entity>(obj, cache, std::move(cacheKey), operationId);
    }

    template<class Entity>
    QSharedPointer<Entity> parseEntity(QNetworkReply *reply, qulonglong operationId)
    {
        QJsonObject obj = parseEntityObject(reply, operationId);
        if (obj.isEmpty())
            return QSharedPointer<Entity>();
        return parseEntity<Entity>(obj, operationId);
    }

    template<class EntityKey, class Entity>
    QSharedPointer<Entity> parseEntity(const QJsonObject &jsonObject, ObjectsCache<EntityKey, Entity> &cache,
                                       std::function<EntityKey(Entity *)> &&cacheKey, qulonglong operationId)
    {
        QSharedPointer<Entity> entity = parseEntity<Entity>(jsonObject, operationId);
        if (entity) {
            QSharedPointer<Entity> fromCache = cache.add(cacheKey(entity.data()), entity);
            if (entity != fromCache) {
                fromCache->updateFrom(entity);
                entity = fromCache;
            }
        }
        return entity;
    }

    template<class Entity>
    QSharedPointer<Entity> parseEntity(const QJsonObject &jsonObject, qulonglong operationId)
    {
        Q_Q(AbstractRestApi);
        QSharedPointer<Entity> entity = Entity::fromJson(jsonObject);
        if (!entity) {
            for (const QString &attribute : qAsConst(serverErrorAttributes)) {
                if (!jsonObject.value(attribute).isUndefined()) {
                    QString jsonErrorMessage = jsonObject.value(attribute).toString();
                    emit q->apiErrorOccurred(operationId,
                                          RestApiError{RestApiError::Level::JsonServerError, 0,
                                                       NETWORK_MODULE_CODE, NetworkErrorCode::InvalidReply,
                                                       jsonErrorMessage});
                    return entity;
                }
            }

            QString errorString(QStringLiteral("Can't create entity from server response"));
            emit q->apiErrorOccurred(operationId,
                                  RestApiError{RestApiError::Level::JsonDataError, 0,
                                               NETWORK_MODULE_CODE, NetworkErrorCode::InvalidReply,
                                               errorString});
        }
        return entity;
    }

    template<class EntityKey, class Entity>
    bool parseEntitiesList(QList<QSharedPointer<Entity>> &entityList, QNetworkReply *reply,
                           ObjectsCache<EntityKey, Entity> &cache, std::function<EntityKey(Entity *)> &&cacheKey,
                           qulonglong operationId, const QString &attributeName = QString())
    {
        auto jsonParser = [this, &entityList, &cache, &cacheKey, operationId](const QJsonArray &jsonArray) -> bool {
            return parseEntitiesList<EntityKey, Entity>(entityList, jsonArray, cache, std::move(cacheKey), operationId);
        };
        return parseEntitiesListPrivate(reply, jsonParser, operationId, attributeName);
    }

    template<class Entity>
    bool parseEntitiesList(QList<QSharedPointer<Entity>> &entityList, QNetworkReply *reply,
                           qulonglong operationId, const QString &attributeName = QString())
    {
        auto jsonParser = [this, &entityList, operationId](const QJsonArray &jsonArray) -> bool {
            return parseEntitiesList<Entity>(entityList, jsonArray, operationId);
        };
        return parseEntitiesListPrivate(reply, jsonParser, operationId, attributeName);
    }

    bool parseEntitiesList(QStringList &entityList, QNetworkReply *reply,
                           qulonglong operationId, const QString &attributeName = QString())
    {
        auto jsonParser = [this, &entityList, operationId](const QJsonArray &jsonArray) -> bool {
            return parseEntitiesList(entityList, jsonArray, operationId);
        };
        return parseEntitiesListPrivate(reply, jsonParser, operationId, attributeName);
    }

    template<class Entity>
    bool parseEntitiesList(QList<QSharedPointer<Entity>> &entityList, const QJsonArray &jsonArray, qulonglong operationId)
    {
        for (const QJsonValue &value : jsonArray) {
            auto entity = parseEntity<Entity>(value.toObject(), operationId);
            if (entity)
                entityList.append(entity);
        }
        return true;
    }

    template<class EntityKey, class Entity>
    bool parseEntitiesList(QList<QSharedPointer<Entity>> &entityList, const QJsonArray &jsonArray,
                           ObjectsCache<EntityKey, Entity> &cache, std::function<EntityKey(Entity *)> &&cacheKey,
                           qulonglong operationId)
    {
        for (const QJsonValue &value : jsonArray) {
            auto entity = parseEntity<EntityKey, Entity>(value.toObject(), cache, std::move(cacheKey), operationId);
            if (entity)
                entityList.append(entity);
        }
        return true;
    }

    bool parseEntitiesList(QStringList &entityList, const QJsonArray &jsonArray, qulonglong operationId)
    {
        Q_UNUSED(operationId)
        for (const QJsonValue &value : jsonArray) {
            QString string = value.toString();
            if (!string.isEmpty())
                entityList.append(string);
        }
        return true;
    }

    QJsonObject parseEntityObject(QNetworkReply *reply, qulonglong operationId)
    {
        Q_Q(AbstractRestApi);
        QByteArray json = reply->readAll();
        QJsonParseError jsonError;
        QJsonDocument doc = QJsonDocument::fromJson(json, &jsonError);
        if (jsonError.error != QJsonParseError::NoError) {
            QString jsonErrorString = QStringLiteral("JSON error: %1").arg(jsonError.errorString());
            emit q->apiErrorOccurred(operationId,
                                  RestApiError{RestApiError::Level::JsonParseError, jsonError.error,
                                               NETWORK_MODULE_CODE, NetworkErrorCode::InvalidReply,
                                               jsonErrorString});
            return QJsonObject();
        } else if (doc.object().isEmpty()) {
            QString jsonErrorString = QStringLiteral("JSON error: empty entity data");
            emit q->apiErrorOccurred(operationId,
                                  RestApiError{RestApiError::Level::JsonParseError, 0,
                                               NETWORK_MODULE_CODE, NetworkErrorCode::InvalidReply,
                                               jsonErrorString});
        }
        return doc.object();
    }

    QMetaObject::Connection replyFinishedConnection;
    QMetaObject::Connection sslErrorsConnection;
    RestClientSP restClient;
    QString vendor;
    QStringList serverErrorAttributes;

protected:
    void runReplyHandler(qulonglong operationId, QNetworkReply *reply);

private:
    void setupReply(qulonglong &operationId, QNetworkReply *reply, RestAnswerHandler &&handler);

    bool parseEntitiesListPrivate(QNetworkReply *reply, std::function<bool(const QJsonArray &)> &&jsonParser,
                                  qulonglong operationId, const QString &attributeName)
    {
        Q_Q(AbstractRestApi);
        bool result = true;
        QByteArray json = reply->readAll();
        QJsonParseError jsonError;
        QJsonDocument doc = QJsonDocument::fromJson(json, &jsonError);
        if (jsonError.error != QJsonParseError::NoError) {
            QString jsonErrorString = QStringLiteral("JSON error: %1").arg(jsonError.errorString());
            emit q->apiErrorOccurred(operationId,
                                  RestApiError{RestApiError::Level::JsonParseError, jsonError.error,
                                               NETWORK_MODULE_CODE, NetworkErrorCode::InvalidReply,
                                               jsonErrorString});
            result = false;
        } else if (!doc.isArray()) {
            if (doc.isObject()) {
                QJsonObject jsonObject = doc.object();
                if (!jsonObject.value(attributeName).isUndefined() && jsonObject.value(attributeName).isArray()) {
                    result = jsonParser(jsonObject.value(attributeName).toArray());
                } else {
                    for (const QString &attribute : qAsConst(serverErrorAttributes)) {
                        if (!jsonObject.value(attribute).isUndefined()) {
                            QString jsonErrorMessage = jsonObject.value(attribute).toString();
                            emit q->apiErrorOccurred(operationId,
                                                  RestApiError{RestApiError::Level::JsonServerError, 0,
                                                               NETWORK_MODULE_CODE, NetworkErrorCode::InvalidReply,
                                                               jsonErrorMessage});
                            return false;
                        }
                    }
                }
            } else {
                QString invalidJson = QStringLiteral("Can't create list of entities from server response");
                emit q->apiErrorOccurred(operationId,
                                      RestApiError{RestApiError::Level::JsonDataError, 0,
                                                   NETWORK_MODULE_CODE, NetworkErrorCode::InvalidReply,
                                                   invalidJson});
                result = false;
            }
        } else {
            result = jsonParser(doc.array());
        }
        return result;
    }

    static std::atomic<qulonglong> lastUsedOperationId;
    QHash<QNetworkReply *, QPair<qulonglong, RestAnswerHandler>> replies;
    QMutex repliesMutex;
};

}
#endif // ABSTRACTRESTAPI_P_H
