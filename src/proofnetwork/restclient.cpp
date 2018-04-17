#include "restclient.h"

#include "proofcore/proofglobal.h"
#include "proofcore/proofobject_p.h"
#include "proofcore/taskchain.h"
#include "proofcore/coreapplication.h"

#include <QAuthenticator>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QCryptographicHash>
#include <QDateTime>
#include <QTimer>
#include <QUuid>
#include <QJsonParseError>
#include <QJsonObject>
#include <QThread>
#include <QBuffer>
#include <QHttpMultiPart>
#include <QNetworkInterface>

static const int DEFAULT_REPLY_TIMEOUT = 5 * 60 * 1000; //5 minutes
static const int OAUTH_TOKEN_REFRESH_TIMEOUT = 1000 * 60 * 60;//1 hour

static const int OAUTH_TOKEN_RETRY_TIMEOUT = 1000 * 2;//2 seconds
static const QSet<QNetworkReply::NetworkError> RETRIABLE_NETWORK_ERRORS = {QNetworkReply::ConnectionRefusedError,
                                                                           QNetworkReply::RemoteHostClosedError,
                                                                           QNetworkReply::HostNotFoundError,
                                                                           QNetworkReply::SslHandshakeFailedError,
                                                                           QNetworkReply::TemporaryNetworkFailureError,
                                                                           QNetworkReply::NetworkSessionFailedError,
                                                                           QNetworkReply::ProxyConnectionRefusedError,
                                                                           QNetworkReply::ProxyConnectionClosedError,
                                                                           QNetworkReply::UnknownNetworkError,
                                                                           QNetworkReply::UnknownProxyError,
                                                                           QNetworkReply::ProxyNotFoundError};

namespace Proof {
class RestClientPrivate : public ProofObjectPrivate
{
    Q_DECLARE_PUBLIC(RestClient)
public:
    QNetworkRequest createNetworkRequest(QString method, const QUrlQuery &query,
                                         const QByteArray &body, const QString &vendor);
    QByteArray generateWsseToken() const;
    void requestQuasiOAuth2token(int retries = 4, const QString &method = QStringLiteral("/oauth2/token"));

    void handleReply(QNetworkReply *reply);
    void cleanupReplyHandler(QNetworkReply *reply);
    QPair<QString, QString> parseHost(const QString &host);

    QNetworkAccessManager *qnam = nullptr;
    QString userName;
    QString password;
    QString clientName;
    QString host;
    QString postfix;
    QString quasiOAuth2Token;
    QString token;
    int port = 443;
    bool explicitPort = false;
    QString scheme = QStringLiteral("https");
    RestAuthType authType = RestAuthType::NoAuth;
    QHash<QNetworkReply *, QTimer *> replyTimeouts;
    int msecsForTimeout = DEFAULT_REPLY_TIMEOUT;
    QHash<QByteArray, QByteArray> customHeaders;
    QHash<QString, QNetworkCookie> cookies;
    QTimer *quasiOAuth2TokenCheckTimer = nullptr;
    QDateTime quasiOAuth2TokenExpiredDateTime;
    bool ignoreSslErrors = false;
    bool followRedirects = true;
};
}

using namespace Proof;

RestClient::RestClient(bool ignoreSslErrors)
    : ProofObject(*new RestClientPrivate)
{
    Q_D(RestClient);
    d->ignoreSslErrors = ignoreSslErrors;
    d->qnam = new QNetworkAccessManager(this);
    connect(d->qnam, &QNetworkAccessManager::authenticationRequired, this, &RestClient::authenticationRequired);
    connect(d->qnam, &QNetworkAccessManager::encrypted, this, &RestClient::encrypted);
    connect(d->qnam, &QNetworkAccessManager::finished, this, &RestClient::finished);
    connect(d->qnam, &QNetworkAccessManager::networkAccessibleChanged, this, &RestClient::networkAccessibleChanged);
    connect(d->qnam, &QNetworkAccessManager::proxyAuthenticationRequired, this, &RestClient::proxyAuthenticationRequired);
    if (!ignoreSslErrors) {
        connect(d->qnam, &QNetworkAccessManager::sslErrors, this,
                [this, d](QNetworkReply *reply, const QList<QSslError> &errors) {
            d->cleanupReplyHandler(reply);
            emit sslErrors(reply, errors);
        });
    }
}

QString RestClient::userName() const
{
    Q_D(const RestClient);
    return d->userName;
}

void RestClient::setUserName(const QString &arg)
{
    Q_D(RestClient);
    if (d->userName != arg) {
        d->userName = arg;
        emit userNameChanged(arg);
    }
}

QString RestClient::password() const
{
    Q_D(const RestClient);
    return d->password;
}

void RestClient::setPassword(const QString &arg)
{
    Q_D(RestClient);
    if (d->password != arg) {
        d->password = arg;
        emit passwordChanged(arg);
    }
}

QString RestClient::clientName() const
{
    Q_D(const RestClient);
    return d->clientName;
}

void RestClient::setClientName(const QString &arg)
{
    Q_D(RestClient);
    if (d->clientName != arg) {
        d->clientName = arg;
        emit clientNameChanged(arg);
    }
}

QString RestClient::host() const
{
    Q_D(const RestClient);
    return d->host;
}

void RestClient::setHost(const QString &arg)
{
    Q_D(RestClient);
    QPair<QString, QString> result = d->parseHost(arg);
    if (d->host != result.first) {
        d->host = result.first;
        emit hostChanged(result.first);
    }
    setPostfix(result.second);
}

QString RestClient::postfix() const
{
    Q_D(const RestClient);
    return d->postfix;
}

void RestClient::setPostfix(const QString &arg)
{
    Q_D(RestClient);
    if (d->postfix != arg) {
        d->postfix = arg;
        emit postfixChanged(arg);
    }
}

int RestClient::port() const
{
    Q_D(const RestClient);
    return d->port;
}

void RestClient::setPort(int arg)
{
    Q_D(RestClient);
    d->explicitPort = true;
    if (d->port != arg) {
        d->port = arg;
        emit portChanged(arg);
    }
}

QString RestClient::scheme() const
{
    Q_D(const RestClient);
    return d->scheme;
}

void RestClient::setScheme(const QString &arg)
{
    Q_D(RestClient);
    if (d->scheme != arg) {
        d->scheme = arg;
        emit schemeChanged(arg);
    }
}

QString RestClient::token() const
{
    Q_D(const RestClient);
    return d->token;
}

void RestClient::setToken(const QString &arg)
{
    Q_D(RestClient);
    if (d->token != arg) {
        d->token = arg;
        emit tokenChanged(arg);
    }
}

RestAuthType RestClient::authType() const
{
    Q_D(const RestClient);
    return d->authType;
}

void RestClient::setAuthType(RestAuthType arg)
{
    Q_D(RestClient);
    if (d->authType != arg) {
        d->authType = arg;
        if (arg != RestAuthType::QuasiOAuth2 && d->quasiOAuth2TokenCheckTimer)
            d->quasiOAuth2TokenCheckTimer->stop();
        emit authTypeChanged(arg);
    }
}

qlonglong RestClient::msecsForTimeout() const
{
    Q_D(const RestClient);
    return d->msecsForTimeout;
}

void RestClient::setMsecsForTimeout(qlonglong arg)
{
    Q_D(RestClient);
    if (d->msecsForTimeout != arg) {
        d->msecsForTimeout = arg;
        emit msecsForTimeoutChanged(arg);
    }
}

bool RestClient::followRedirects() const
{
    Q_D(const RestClient);
    return d->followRedirects;
}

void RestClient::setFollowRedirects(bool arg)
{
    Q_D(RestClient);
    if (d->followRedirects != arg) {
        d->followRedirects = arg;
        emit followRedirectsChanged(arg);
    }
}

void RestClient::setCustomHeader(const QByteArray &header, const QByteArray &value)
{
    Q_D(RestClient);
    d->customHeaders[header] = value;
}

QByteArray RestClient::customHeader(const QByteArray &header) const
{
    Q_D(const RestClient);
    return d->customHeaders.value(header);
}

bool RestClient::containsCustomHeader(const QByteArray &header) const
{
    Q_D(const RestClient);
    return d->customHeaders.contains(header);
}

void RestClient::unsetCustomHeader(const QByteArray &header)
{
    Q_D(RestClient);
    d->customHeaders.remove(header);
}

void RestClient::setCookie(const QNetworkCookie &cookie)
{
    Q_D(RestClient);
    d->cookies[cookie.name()] = cookie;
}

QNetworkCookie RestClient::cookie(const QString &name) const
{
    Q_D(const RestClient);
    return d->cookies.value(name);
}

bool RestClient::containsCookie(const QString &name) const
{
    Q_D(const RestClient);
    return d->cookies.contains(name);
}

void RestClient::unsetCookie(const QString &name)
{
    Q_D(RestClient);
    d->cookies.remove(name);
}

QNetworkReply *RestClient::get(const QString &method, const QUrlQuery &query, const QString &vendor)
{
    Q_D(RestClient);
    qCDebug(proofNetworkMiscLog) << method << query.toString(QUrl::EncodeSpaces);
    QNetworkReply *reply = d->qnam->get(d->createNetworkRequest(method, query, "", vendor));
    d->handleReply(reply);
    return reply;
}

QNetworkReply *RestClient::post(const QString &method, const QUrlQuery &query, const QByteArray &body, const QString &vendor)
{
    Q_D(RestClient);
    qCDebug(proofNetworkMiscLog) << method << query.toString(QUrl::EncodeSpaces);
    QNetworkReply *reply = d->qnam->post(d->createNetworkRequest(method, query, body, vendor), body);
    d->handleReply(reply);
    return reply;
}

QNetworkReply *RestClient::post(const QString &method, const QUrlQuery &query, QHttpMultiPart *multiParts)
{
    Q_D(RestClient);
    qCDebug(proofNetworkMiscLog) << method << query.toString(QUrl::EncodeSpaces);
    QNetworkRequest request = d->createNetworkRequest(method, query, "", QLatin1String(""));
    request.setHeader(QNetworkRequest::KnownHeaders::ContentTypeHeader,
                      QStringLiteral("multipart/form-data; boundary=%1").arg(QString(multiParts->boundary())));
    QNetworkReply *reply = d->qnam->post(request, multiParts);
    qCDebug(proofNetworkMiscLog) << request.header(QNetworkRequest::KnownHeaders::ContentTypeHeader).toString();
    multiParts->setParent(reply);
    d->handleReply(reply);
    return reply;
}

QNetworkReply *RestClient::put(const QString &method, const QUrlQuery &query, const QByteArray &body, const QString &vendor)
{
    Q_D(RestClient);
    qCDebug(proofNetworkMiscLog) << method << query.toString(QUrl::EncodeSpaces);
    QNetworkReply *reply = d->qnam->put(d->createNetworkRequest(method, query, body, vendor), body);
    d->handleReply(reply);
    return reply;
}

QNetworkReply *RestClient::patch(const QString &method, const QUrlQuery &query, const QByteArray &body, const QString &vendor)
{
    Q_D(RestClient);
    QBuffer *bodyBuffer = new QBuffer;
    bodyBuffer->setData(body);
    qCDebug(proofNetworkMiscLog) << method << query.toString(QUrl::EncodeSpaces);
    QNetworkReply *reply = d->qnam->sendCustomRequest(d->createNetworkRequest(method, query, body, vendor), "PATCH", bodyBuffer);
    d->handleReply(reply);
    bodyBuffer->setParent(reply);
    return reply;
}

QNetworkReply *RestClient::deleteResource(const QString &method, const QUrlQuery &query, const QString &vendor)
{
    Q_D(RestClient);
    qCDebug(proofNetworkMiscLog) << method << query.toString(QUrl::EncodeSpaces);
    QNetworkReply *reply = d->qnam->deleteResource(d->createNetworkRequest(method, query, QByteArray(), vendor));
    d->handleReply(reply);
    return reply;
}

void RestClient::authenticate()
{
    Q_D(RestClient);
    if (!call(this, &RestClient::authenticate)) {
        if (authType() == RestAuthType::QuasiOAuth2) {
            if (!d->quasiOAuth2TokenCheckTimer) {
                d->quasiOAuth2TokenCheckTimer = new QTimer(this);
                d->quasiOAuth2TokenCheckTimer->setInterval(OAUTH_TOKEN_REFRESH_TIMEOUT);

                connect(d->quasiOAuth2TokenCheckTimer, &QTimer::timeout, this, [d]() {
                    d->requestQuasiOAuth2token();
                });
            }
            d->quasiOAuth2TokenCheckTimer->start();
            d->requestQuasiOAuth2token();
        }
    }
}

QNetworkRequest RestClientPrivate::createNetworkRequest(QString method, const QUrlQuery &query,
                                                        const QByteArray &body, const QString &vendor)
{
    Q_Q(RestClient);

    QNetworkRequest result;

    QUrl url;
    url.setScheme(scheme);
    url.setHost(host);
    if (explicitPort)
        url.setPort(port);
    if (!method.startsWith('/'))
        method.prepend('/');
    url.setPath(postfix + method);
    url.setQuery(query);

    result.setUrl(url);
    result.setAttribute(QNetworkRequest::FollowRedirectsAttribute, followRedirects);

    if (!body.isEmpty()) {
        QJsonParseError error;
        QJsonDocument::fromJson(body, &error);

        QString contentTypePattern = vendor.isEmpty() ? QStringLiteral("application/%1") : QStringLiteral("application/vnd.%1+%2").arg(vendor);

        //We assume that if it is not json and not xml it's url encoded data
        if (error.error == QJsonParseError::NoError)
            result.setHeader(QNetworkRequest::ContentTypeHeader, contentTypePattern.arg(QStringLiteral("json")));
        else if (body.startsWith("<?xml"))
            result.setHeader(QNetworkRequest::ContentTypeHeader, vendor.isEmpty() ? QStringLiteral("text/xml") : contentTypePattern.arg(QStringLiteral("xml")));
        else
            result.setHeader(QNetworkRequest::ContentTypeHeader, contentTypePattern.arg(QStringLiteral("x-www-form-urlencoded")));

    } else {
        if (vendor.isEmpty())
            result.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain");
        else
            result.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/vnd.%1").arg(vendor));
    }

    for (const QNetworkCookie &cookie : qAsConst(cookies))
        result.setHeader(QNetworkRequest::CookieHeader, QVariant::fromValue(cookie));

    for (auto it = customHeaders.cbegin(); it != customHeaders.cend(); ++it)
        result.setRawHeader(it.key(), it.value());

    result.setRawHeader("Proof-Application", proofApp->prettifiedApplicationName().toLatin1());
    result.setRawHeader(QStringLiteral("Proof-%1-Version").arg(proofApp->prettifiedApplicationName()).toLatin1(), qApp->applicationVersion().toLatin1());
    result.setRawHeader(QStringLiteral("Proof-%1-Framework-Version").arg(proofApp->prettifiedApplicationName()).toLatin1(), Proof::proofVersion().toLatin1());

    QStringList ipAdresses;
    const auto allAddresses = QNetworkInterface::allAddresses();
    for (const auto &address : allAddresses) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && address != QHostAddress::LocalHost)
            ipAdresses << address.toString();
    }
    result.setRawHeader(QStringLiteral("Proof-IP-Addresses").toLatin1(), ipAdresses.join(QStringLiteral("; ")).toLatin1());

    switch (authType) {
    case RestAuthType::Wsse:
        result.setRawHeader("X-WSSE", generateWsseToken());
        result.setRawHeader("X-Client-Name", clientName.toLocal8Bit());
        result.setRawHeader("Authorization", "WSSE profile=\"UsernameToken\"");
        break;
    case RestAuthType::Basic:
        if (!clientName.isEmpty())
            result.setRawHeader("X-Client-Name", clientName.toLocal8Bit());
        result.setRawHeader("Authorization",
                            QStringLiteral("Basic %1")
                            .arg(QString(QStringLiteral("%1:%2").arg(userName, password).toLatin1().toBase64()))
                            .toLatin1());
        break;
    case RestAuthType::QuasiOAuth2:
        if (QDateTime::currentDateTime() >= quasiOAuth2TokenExpiredDateTime) {
            Proof::TaskChainSP taskChain = Proof::TaskChain::createChain();
            auto task = [taskChain, q]() {
                std::function<bool()> callback = [](){return true;};
                taskChain->addSignalWaiter(q, &RestClient::authenticationErrorOccurred, callback);
                taskChain->addSignalWaiter(q, &RestClient::authenticationSucceed, callback);
                q->authenticate();

                taskChain->fireSignalWaiters();
            };
            qlonglong taskId = taskChain->addTask(task);

            while(!taskChain->touchTask(taskId))
                qApp->processEvents();
        }
        result.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(quasiOAuth2Token).toLatin1());
        break;
    case RestAuthType::BearerToken:
        if (!clientName.isEmpty())
            result.setRawHeader("X-Client-Name", clientName.toLocal8Bit());
        result.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token).toLatin1());
        break;
    case RestAuthType::NoAuth:
        if (!clientName.isEmpty())
            result.setRawHeader("X-Client-Name", clientName.toLocal8Bit());
        break;
    }

    return result;
}

QByteArray RestClientPrivate::generateWsseToken() const
{
    QByteArray hashedPassword;
    if (!password.isEmpty()) {
        QCryptographicHash passwordHasher(QCryptographicHash::Md5);
        passwordHasher.addData(password.toLatin1());
        hashedPassword = passwordHasher.result().toHex();
    }

    QString createdAt = QDateTime::currentDateTime().toString(Qt::ISODate);
    QString nonce = QUuid::createUuid().toString().replace(QLatin1String("-"), QLatin1String(""));

    QCryptographicHash hasher(QCryptographicHash::Sha1);
    hasher.addData(nonce.toLatin1());
    hasher.addData(createdAt.toLatin1());
    hasher.addData(hashedPassword);

    return QStringLiteral("UsernameToken Username=\"%1\", PasswordDigest=\"%2\", Nonce=\"%3\", Created=\"%4\"")
            .arg(userName,
                 QString(hasher.result().toBase64()),
                 QString(nonce.toLatin1().toBase64()),
                 createdAt)
            .toLatin1();
}

void RestClientPrivate::requestQuasiOAuth2token(int retries, const QString &method)
{
    Q_Q(RestClient);
    QUrl url;
    url.setScheme(scheme);
    url.setHost(host);
    if (explicitPort)
        url.setPort(port);
    url.setPath(method);
    QString quasiOAuth2TokenRequestData = QStringLiteral("grant_type=password&username=%1&password=%2").arg(userName, password);
    QDateTime expiredTime = QDateTime::currentDateTime();
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    auto reply = qnam->post(request, QUrlQuery(quasiOAuth2TokenRequestData).toString().toLatin1());
    handleReply(reply);
    QObject::connect(reply, static_cast<void(QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error),
                     q, [this, q, reply, retries, method](QNetworkReply::NetworkError code) {
        if (RETRIABLE_NETWORK_ERRORS.contains(code) && retries > 0) {
            qCDebug(proofNetworkMiscLog) << "Network request to" << reply->request().url().toString() << "failed." << retries << " more attempts will be done";
            QTimer::singleShot(OAUTH_TOKEN_RETRY_TIMEOUT, q, [this, retries, method](){requestQuasiOAuth2token(retries - 1, method);});
        } else {
            emit q->authenticationErrorOccurred(QStringLiteral("Can't connect to Scissorhands service.\nPlease try again."));
        }
        reply->deleteLater();
    });
    QObject::connect(reply, &QNetworkReply::finished,
                     q, [this, q, reply, expiredTime]() {
        if (reply->error() == QNetworkReply::NoError) {
            QJsonParseError error;
            QJsonObject response = QJsonDocument::fromJson(reply->readAll(), &error).object();

            if (error.error == QJsonParseError::NoError) {
                quasiOAuth2Token = response.value(QStringLiteral("access_token")).toString();
                int expiresInSeconds = response.value(QStringLiteral("expires_in")).toInt();
                quasiOAuth2TokenExpiredDateTime = expiredTime.addSecs(expiresInSeconds);
                if (quasiOAuth2Token.isEmpty())
                    emit q->authenticationErrorOccurred(QStringLiteral("Wrong Scissorhands service authentication.\nPlease check your authentication settings."));
                else
                    emit q->authenticationSucceed();

            } else {
                emit q->authenticationErrorOccurred(QStringLiteral("Wrong Scissorhands service answer.\nPlease check your host settings."));
            }
        }
        reply->deleteLater();
    });
}

void RestClientPrivate::handleReply(QNetworkReply *reply)
{
    Q_Q(RestClient);

    if (ignoreSslErrors)
        reply->ignoreSslErrors();

    QTimer *timer = new QTimer();
    timer->setSingleShot(true);
    replyTimeouts.insert(reply, timer);
    QObject::connect(timer, &QTimer::timeout, [timer, reply](){
        qCWarning(proofNetworkMiscLog) << "Timed out:" << reply->request().url().toDisplayString(QUrl::FormattingOptions(QUrl::FullyDecoded)) << reply->isRunning();
        if (reply->isRunning())
            reply->abort();
        timer->deleteLater();
    });

    timer->start(msecsForTimeout);

    QObject::connect(reply, static_cast<void(QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error),
                     q, [this, reply](QNetworkReply::NetworkError e) {
        qCWarning(proofNetworkMiscLog) << "Error occurred:" << reply->request().url().toDisplayString(QUrl::FormattingOptions(QUrl::FullyDecoded)) << e;
        cleanupReplyHandler(reply);
    });
    QObject::connect(reply, &QNetworkReply::finished,
                     q, [this, reply]() {
        qCDebug(proofNetworkMiscLog) << "Finished:" << reply->request().url().toDisplayString(QUrl::FormattingOptions(QUrl::FullyDecoded)) << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        cleanupReplyHandler(reply);
    });
}

void RestClientPrivate::cleanupReplyHandler(QNetworkReply *reply)
{
    if (replyTimeouts.contains(reply)) {
        QTimer *connectionTimer = replyTimeouts.take(reply);
        connectionTimer->stop();
        connectionTimer->deleteLater();
    }
}

QPair<QString, QString> RestClientPrivate::parseHost(const QString &host)
{
    QStringList parts = host.split('/', QString::SkipEmptyParts);
    if (parts.isEmpty())
        return {host, QString()};

    int hostIndex = parts[0].endsWith(':') ? 1 : 0;
    QString newHost = parts[hostIndex];

    parts.removeFirst();
    if (hostIndex > 0)
        parts.removeFirst();

    QString postfix;
    if (!parts.isEmpty())
        postfix = '/' + parts.join('/');
    return {newHost, postfix};
}
