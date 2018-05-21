#include "abstractrestapi.h"
#include "abstractrestapi_p.h"

#include "restclient.h"

#include <QNetworkReply>
#include <QThread>

static const int NETWORK_SSL_ERROR_OFFSET = 1500;
static const int NETWORK_ERROR_OFFSET = 1000;

static const QSet<int> ALLOWED_HTTP_STATUSES = {200, 201, 202, 203, 204, 205, 206};

std::atomic<qulonglong> Proof::AbstractRestApiPrivate::lastUsedOperationId {0};

using namespace Proof;

AbstractRestApi::AbstractRestApi(const RestClientSP &restClient, AbstractRestApiPrivate &dd, QObject *parent)
    : ProofObject(dd, parent)
{
    setRestClient(restClient);
}

RestClientSP AbstractRestApi::restClient() const
{
    Q_D(const AbstractRestApi);
    return d->restClient;
}

void AbstractRestApi::setRestClient(const RestClientSP &client)
{
    Q_D(AbstractRestApi);
    if (d->restClient == client)
        return;
    onRestClientChanging(client);
    d->restClient = client;
}

void AbstractRestApi::abortRequest(qulonglong operationId)
{
    Q_D(AbstractRestApi);
    d->repliesMutex.lock();
    auto networkReplies = d->replies.keys();
    QNetworkReply *toAbort = nullptr;

    RestApiError error = RestApiError{RestApiError::Level::ClientError,
            NETWORK_ERROR_OFFSET + static_cast<int>(QNetworkReply::NetworkError::OperationCanceledError),
            NETWORK_MODULE_CODE, NetworkErrorCode::ServiceUnavailable,
            QStringLiteral("Request canceled"), false};
    for(auto reply : networkReplies) {
        if (operationId != d->replies[reply].first)
            continue;
        d->replies.remove(reply);
        toAbort = reply;
        break;
    }
    d->repliesMutex.unlock();

    if (toAbort && toAbort->isRunning()) {
        emit apiErrorOccurred(operationId, error);
        toAbort->abort();
        toAbort->deleteLater();
    }
}

bool AbstractRestApi::isLoggedOut() const
{
    if (!restClient())
        return true;

    switch(restClient()->authType()) {
    case Proof::RestAuthType::Basic:
        return restClient()->userName().isEmpty() || restClient()->password().isEmpty();
    case Proof::RestAuthType::Wsse:
        return restClient()->userName().isEmpty();
    case Proof::RestAuthType::BearerToken:
        return restClient()->token().isEmpty();
    default:
        return false;
    }
}

qlonglong AbstractRestApi::clientNetworkErrorOffset()
{
    return NETWORK_ERROR_OFFSET;
}

qlonglong AbstractRestApi::clientSslErrorOffset()
{
    return NETWORK_SSL_ERROR_OFFSET;
}

AbstractRestApi::ErrorCallbackType AbstractRestApi::generateErrorCallback(qulonglong &currentOperationId, RestApiError &error)
{
    return [&currentOperationId, &error]
            (qulonglong operationId, const Proof::RestApiError &_error) {
        if (currentOperationId != operationId)
            return false;
        error = _error;
        return true;
    };
}

AbstractRestApi::ErrorCallbackType AbstractRestApi::generateErrorCallback(qulonglong &currentOperationId, QString &errorMessage)
{
    return [&currentOperationId, &errorMessage]
            (qulonglong operationId, const Proof::RestApiError &_error) {
        if (currentOperationId != operationId)
            return false;
        errorMessage = QStringLiteral("%1: %2").arg(_error.code).arg(_error.message);
        return true;
    };
}

void AbstractRestApi::onRestClientChanging(const RestClientSP &client)
{
    Q_D(AbstractRestApi);
    if (d->replyFinishedConnection)
        QObject::disconnect(d->replyFinishedConnection);
    if (d->sslErrorsConnection)
        QObject::disconnect(d->sslErrorsConnection);
    if (!client)
        return;

    auto replyFinishedCaller = [d](QNetworkReply *reply) {
        QMutexLocker lock(&d->repliesMutex);
        if (!d->replies.contains(reply))
            return;
        qulonglong operationId = d->replies[reply].first;
        lock.unlock();
        d->replyFinished(operationId, reply);
    };
    auto sslErrorsOccurredCaller = [d](QNetworkReply *reply, const QList<QSslError> &errors) {
        QMutexLocker lock(&d->repliesMutex);
        if (!d->replies.contains(reply))
            return;
        qulonglong operationId = d->replies[reply].first;
        lock.unlock();
        d->sslErrorsOccurred(operationId, reply, errors);
    };

    d->replyFinishedConnection = QObject::connect(client.data(), &RestClient::finished, this, replyFinishedCaller);
    d->sslErrorsConnection = QObject::connect(client.data(), &RestClient::sslErrors, this, sslErrorsOccurredCaller);
}

QNetworkReply *AbstractRestApiPrivate::get(qulonglong &operationId, RestAnswerHandler &&handler, const QString &method, const QUrlQuery &query)
{
    QNetworkReply *reply = restClient->get(method, query, vendor)->result();
    setupReply(operationId, reply, std::move(handler));
    return reply;
}

QNetworkReply *AbstractRestApiPrivate::post(qulonglong &operationId, RestAnswerHandler &&handler, const QString &method, const QUrlQuery &query, const QByteArray &body)
{
    QNetworkReply *reply = restClient->post(method, query, body, vendor)->result();
    setupReply(operationId, reply, std::move(handler));
    return reply;
}

QNetworkReply *AbstractRestApiPrivate::post(qulonglong &operationId, RestAnswerHandler &&handler, const QString &method, const QUrlQuery &query, QHttpMultiPart *multiParts)
{
    QNetworkReply *reply = restClient->post(method, query, multiParts)->result();
    setupReply(operationId, reply, std::move(handler));
    return reply;
}

QNetworkReply *AbstractRestApiPrivate::put(qulonglong &operationId, RestAnswerHandler &&handler, const QString &method, const QUrlQuery &query, const QByteArray &body)
{
    QNetworkReply *reply = restClient->put(method, query, body, vendor)->result();
    setupReply(operationId, reply, std::move(handler));
    return reply;
}

QNetworkReply *AbstractRestApiPrivate::patch(qulonglong &operationId, RestAnswerHandler &&handler, const QString &method, const QUrlQuery &query, const QByteArray &body)
{
    QNetworkReply *reply = restClient->patch(method, query, body, vendor)->result();
    setupReply(operationId, reply, std::move(handler));
    return reply;
}

QNetworkReply *AbstractRestApiPrivate::deleteResource(qulonglong &operationId, RestAnswerHandler &&handler, const QString &method, const QUrlQuery &query)
{
    QNetworkReply *reply = restClient->deleteResource(method, query, vendor)->result();
    setupReply(operationId, reply, std::move(handler));
    return reply;
}

void AbstractRestApiPrivate::replyFinished(qulonglong operationId, QNetworkReply *reply, bool forceUserFriendly)
{
    Q_Q(AbstractRestApi);
    if (reply->error() == QNetworkReply::NetworkError::NoError
            || (reply->error() >= 100 && (reply->error() % 100) != 99)) {
        int errorCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!ALLOWED_HTTP_STATUSES.contains(errorCode)) {
            QString message;
            QStringList contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().split(";", QString::SkipEmptyParts);
            for (QString &str : contentType)
                str = str.trimmed();
            if (contentType.contains(QLatin1String("text/plain"))) {
                message = reply->readAll().trimmed();
            } else if (contentType.contains(QLatin1String("application/json"))) {
                QJsonParseError jsonError;
                QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &jsonError);
                if (jsonError.error == QJsonParseError::NoError && doc.isObject())
                    message = doc.object()[QStringLiteral("message")].toString();
            }
            if (message.isEmpty())
                message = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString().trimmed();

            qCDebug(proofNetworkMiscLog) << "Error occurred for" << operationId
                                         << reply->request().url().toDisplayString(QUrl::FormattingOptions(QUrl::FullyDecoded))
                                         << ": " << errorCode << message;
            emit q->apiErrorOccurred(operationId,
                                     RestApiError{RestApiError::Level::ServerError, errorCode,
                                                  NETWORK_MODULE_CODE, NetworkErrorCode::ServerError,
                                                  message, forceUserFriendly});
            cleanupReply(operationId, reply);
        }
    }

    runReplyHandler(operationId, reply);
}

void AbstractRestApiPrivate::runReplyHandler(qulonglong operationId, QNetworkReply *reply)
{
    QMutexLocker lock(&repliesMutex);
    if (replies.contains(reply)) {
        auto handler = std::move(replies.value(reply).second);
        lock.unlock();
        if (handler)
            handler(operationId, reply);
        cleanupReply(operationId, reply);
    }
}

void AbstractRestApiPrivate::replyErrorOccurred(qulonglong operationId, QNetworkReply *reply, bool forceUserFriendly)
{
    Q_Q(AbstractRestApi);
    if (reply->error() != QNetworkReply::NetworkError::NoError && (reply->error() < 200 || (reply->error() % 100) == 99)) {
        int errorCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!errorCode)
            errorCode = NETWORK_ERROR_OFFSET + static_cast<int>(reply->error());
        QString errorString = reply->errorString();
        long proofErrorCode = NetworkErrorCode::ServerError;
        qCDebug(proofNetworkMiscLog) << "Error occurred for" << operationId
                                     << reply->request().url().toDisplayString(QUrl::FormattingOptions(QUrl::FullyDecoded))
                                     << ": " << errorCode << errorString;
        switch (reply->error()) {
        case QNetworkReply::HostNotFoundError:
            errorString = QStringLiteral("Host %1 not found. Try again later").arg(reply->url().host());
            proofErrorCode = NetworkErrorCode::ServiceUnavailable;
            forceUserFriendly = true;
            break;
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:
        case QNetworkReply::TimeoutError:
        case QNetworkReply::OperationCanceledError:
            errorString = QStringLiteral("Host %1 is unavailable. Try again later").arg(reply->url().host());
            proofErrorCode = NetworkErrorCode::ServiceUnavailable;
            forceUserFriendly = true;
            break;
        default:
            break;
        }
        emit q->apiErrorOccurred(operationId,
                                 RestApiError{RestApiError::Level::ClientError, errorCode,
                                              NETWORK_MODULE_CODE, proofErrorCode,
                                              errorString, forceUserFriendly});
        cleanupReply(operationId, reply);
    }
}

void AbstractRestApiPrivate::sslErrorsOccurred(qulonglong operationId, QNetworkReply *reply, const QList<QSslError> &errors, bool forceUserFriendly)
{
    Q_Q(AbstractRestApi);
    bool firstError = true;
    for (const QSslError &error : errors) {
        if (error.error() != QSslError::SslError::NoError) {
            int errorCode = NETWORK_SSL_ERROR_OFFSET + static_cast<int>(error.error());
            qCWarning(proofNetworkMiscLog) << "SSL error occurred for" << operationId
                                           << reply->request().url().toDisplayString(QUrl::FormattingOptions(QUrl::FullyDecoded))
                                           << ": " << errorCode << error.errorString();
            if (!firstError)
                continue;
            firstError = false;
            emit q->apiErrorOccurred(operationId,
                                     RestApiError{RestApiError::Level::ClientError, errorCode,
                                                  NETWORK_MODULE_CODE, NetworkErrorCode::SslError,
                                                  error.errorString(), forceUserFriendly});
            cleanupReply(operationId, reply);
        }
    }
}

void AbstractRestApiPrivate::cleanupReply(qulonglong operationId, QNetworkReply *reply)
{
    Q_UNUSED(operationId)
    repliesMutex.lock();
    int numberRemoved = replies.remove(reply);
    repliesMutex.unlock();
    if (numberRemoved > 0)
        reply->deleteLater();
}

void AbstractRestApiPrivate::notifyAboutJsonParseError(qulonglong operationId, QJsonParseError error)
{
    Q_Q(AbstractRestApi);
    emit q->apiErrorOccurred(operationId,
                             RestApiError{RestApiError::Level::JsonParseError, error.error,
                                          NETWORK_MODULE_CODE, NetworkErrorCode::InvalidReply,
                                          QStringLiteral("JSON error: %1").arg(error.errorString())});
}

void AbstractRestApiPrivate::setupReply(qulonglong &operationId, QNetworkReply *reply, RestAnswerHandler &&handler)
{
    Q_Q(AbstractRestApi);
    operationId = ++lastUsedOperationId;
    repliesMutex.lock();
    replies[reply] = qMakePair(operationId, std::move(handler));
    repliesMutex.unlock();
    QObject::connect(reply, static_cast<void(QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error),
                     q, [this, reply, operationId](QNetworkReply::NetworkError) {
        QMutexLocker lock(&repliesMutex);
        if (!replies.contains(reply))
            return;
        lock.unlock();
        replyErrorOccurred(operationId, reply);
    });
}

void AbstractRestApiPrivate::clearReplies()
{
    Q_Q(AbstractRestApi);
    repliesMutex.lock();
    auto networkReplies = replies.keys();
    QList<QNetworkReply *> toAbort;

    RestApiError error = RestApiError{RestApiError::Level::ClientError,
            NETWORK_ERROR_OFFSET + static_cast<int>(QNetworkReply::NetworkError::OperationCanceledError),
            NETWORK_MODULE_CODE, NetworkErrorCode::ServiceUnavailable,
            QStringLiteral("Request canceled"), false};
    for(auto reply : networkReplies) {
        qulonglong operationId = replies[reply].first;
        replies.remove(reply);
        toAbort << reply;
        if (operationId)
            emit q->apiErrorOccurred(operationId, error);
    }
    repliesMutex.unlock();

    for(auto reply : qAsConst(toAbort)) {
        reply->abort();
        reply->deleteLater();
    }
}

QString RestApiError::toString() const
{
    if (level == Level::NoError)
        return QString();
    return QStringLiteral("%1: %2").arg(code).arg(message);
}

void RestApiError::reset()
{
    level = Level::NoError;
    code = 0;
    message = QString();
}

bool RestApiError::isNetworkError() const
{
    return level == Level::ClientError && code > NETWORK_ERROR_OFFSET;
}

QNetworkReply::NetworkError RestApiError::toNetworkError() const
{
    if (isNetworkError())
        return (QNetworkReply::NetworkError)(code - NETWORK_ERROR_OFFSET);
    else
        return QNetworkReply::UnknownNetworkError;
}

Failure RestApiError::toFailure() const
{
    if (level == Level::NoError)
        return Failure();
    else
        return Failure(message, proofModuleCode, proofErrorCode,
                       userFriendly ? Failure::UserFriendlyHint : Failure::NoHint,
                       code ? code : QVariant());
}

RestApiError RestApiError::fromFailure(const Failure &f)
{
    return RestApiError(f.exists ? Level::ServerError : Level::NoError,
                        f.data.toInt(), f.moduleCode, f.errorCode,
                        f.message, f.hints & Failure::UserFriendlyHint);
}
