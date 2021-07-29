#include "githubimagehost.h"

#include <QDebug>
#include <QFileInfo>
#include <QByteArray>

#include <utils/utils.h>
#include <utils/webutils.h>

using namespace vnotex;

const QString GitHubImageHost::c_apiUrl = "https://api.github.com";

GitHubImageHost::GitHubImageHost(QObject *p_parent)
    : ImageHost(p_parent)
{
}

bool GitHubImageHost::ready() const
{
    return !m_personalAccessToken.isEmpty() && !m_userName.isEmpty() && !m_repoName.isEmpty();
}

ImageHost::Type GitHubImageHost::getType() const
{
    return Type::GitHub;
}

QJsonObject GitHubImageHost::getConfig() const
{
    QJsonObject obj;
    obj[QStringLiteral("personal_access_token")] = m_personalAccessToken;
    obj[QStringLiteral("user_name")] = m_userName;
    obj[QStringLiteral("repository_name")] = m_repoName;
    return obj;
}

void GitHubImageHost::setConfig(const QJsonObject &p_jobj)
{
    parseConfig(p_jobj, m_personalAccessToken, m_userName, m_repoName);

    m_imageUrlPrefix = QString("https://raw.githubusercontent.com/%1/%2/master/").arg(m_userName, m_repoName);
}

bool GitHubImageHost::testConfig(const QJsonObject &p_jobj, QString &p_msg)
{
    p_msg.clear();

    QString token, userName, repoName;
    parseConfig(p_jobj, token, userName, repoName);

    if (token.isEmpty() || userName.isEmpty() || repoName.isEmpty()) {
        p_msg = tr("PersonalAccessToken/UserName/RepositoryName should not be empty.");
        return false;
    }

    auto reply = getRepoInfo(token, userName, repoName);
    p_msg = QString::fromUtf8(reply.m_data);
    return reply.m_error == QNetworkReply::NoError;
}

QPair<QByteArray, QByteArray> GitHubImageHost::authorizationHeader(const QString &p_token)
{
    auto token = "token " + p_token;
    return qMakePair(QByteArray("Authorization"), token.toUtf8());
}

QPair<QByteArray, QByteArray> GitHubImageHost::acceptHeader()
{
    return qMakePair(QByteArray("Accept"), QByteArray("application/vnd.github.v3+json"));
}

vte::NetworkAccess::RawHeaderPairs GitHubImageHost::prepareCommonHeaders(const QString &p_token)
{
    vte::NetworkAccess::RawHeaderPairs rawHeader;
    rawHeader.push_back(authorizationHeader(p_token));
    rawHeader.push_back(acceptHeader());
    return rawHeader;
}

vte::NetworkReply GitHubImageHost::getRepoInfo(const QString &p_token, const QString &p_userName, const QString &p_repoName) const
{
    auto rawHeader = prepareCommonHeaders(p_token);
    const auto urlStr = QString("%1/repos/%2/%3").arg(c_apiUrl, p_userName, p_repoName);
    auto reply = vte::NetworkAccess::request(QUrl(urlStr), rawHeader);
    return reply;
}

void GitHubImageHost::parseConfig(const QJsonObject &p_jobj,
                                  QString &p_token,
                                  QString &p_userName,
                                  QString &p_repoName)
{
    p_token = p_jobj[QStringLiteral("personal_access_token")].toString();
    p_userName = p_jobj[QStringLiteral("user_name")].toString();
    p_repoName = p_jobj[QStringLiteral("repository_name")].toString();
}

QString GitHubImageHost::create(const QByteArray &p_data, const QString &p_path, QString &p_msg)
{
    QString destUrl;

    if (p_path.isEmpty()) {
        p_msg = tr("Failed to create image with empty path.");
        return destUrl;
    }

    destUrl = createResource(p_data, p_path, p_msg);
    return destUrl;
}

QString GitHubImageHost::createResource(const QByteArray &p_content, const QString &p_path, QString &p_msg) const
{
    Q_ASSERT(!p_path.isEmpty());

    if (!ready()) {
        p_msg = tr("Invalid GitHub image host configuration.");
        return QString();
    }

    auto rawHeader = prepareCommonHeaders(m_personalAccessToken);
    const auto urlStr = QString("%1/repos/%2/%3/contents/%4").arg(c_apiUrl, m_userName, m_repoName, p_path);

    // Check if @p_path already exists.
    auto reply = vte::NetworkAccess::request(QUrl(urlStr), rawHeader);
    if (reply.m_error == QNetworkReply::NoError) {
        p_msg = tr("The resource already exists at the image host (%1).").arg(p_path);
        return QString();
    } else if (reply.m_error != QNetworkReply::ContentNotFoundError) {
        p_msg = tr("Failed to query the resource at the image host (%1) (%2) (%3).").arg(urlStr, reply.errorStr(), reply.m_data);
        return QString();
    }

    // Create the content.
    QJsonObject requestDataObj;
    requestDataObj[QStringLiteral("message")] = QString("VX_ADD: %1").arg(p_path);
    requestDataObj[QStringLiteral("content")] = QString::fromUtf8(p_content.toBase64());
    auto requestData = Utils::toJsonString(requestDataObj);
    reply = vte::NetworkAccess::put(QUrl(urlStr), rawHeader, requestData);
    if (reply.m_error != QNetworkReply::NoError) {
        p_msg = tr("Failed to create resource at the image host (%1) (%2) (%3).").arg(urlStr, reply.errorStr(), reply.m_data);
        return QString();
    } else {
        auto replyObj = Utils::fromJsonString(reply.m_data);
        Q_ASSERT(!replyObj.isEmpty());
        auto targetUrl = replyObj[QStringLiteral("content")].toObject().value(QStringLiteral("download_url")).toString();
        if (targetUrl.isEmpty()) {
            p_msg = tr("Failed to create resource at the image host (%1) (%2) (%3).").arg(urlStr, reply.errorStr(), reply.m_data);
        } else {
            qDebug() << "created resource" << targetUrl;
        }
        return targetUrl;
    }
}

bool GitHubImageHost::ownsUrl(const QString &p_url) const
{
    return p_url.startsWith(m_imageUrlPrefix);
}

bool GitHubImageHost::remove(const QString &p_url, QString &p_msg)
{
    Q_ASSERT(ownsUrl(p_url));

    if (!ready()) {
        p_msg = tr("Invalid GitHub image host configuration.");
        return false;
    }

    const QString resourcePath = WebUtils::purifyUrl(p_url.mid(m_imageUrlPrefix.size()));

    auto rawHeader = prepareCommonHeaders(m_personalAccessToken);
    const auto urlStr = QString("%1/repos/%2/%3/contents/%4").arg(c_apiUrl, m_userName, m_repoName, resourcePath);

    // Get the SHA of the resource.
    auto reply = vte::NetworkAccess::request(QUrl(urlStr), rawHeader);
    if (reply.m_error != QNetworkReply::NoError) {
        p_msg = tr("Failed to fetch information about the resource (%1).").arg(resourcePath);
        return false;
    }

    auto replyObj = Utils::fromJsonString(reply.m_data);
    Q_ASSERT(!replyObj.isEmpty());
    const auto sha = replyObj[QStringLiteral("sha")].toString();
    if (sha.isEmpty()) {
        p_msg = tr("Failed to fetch SHA about the resource (%1) (%2).").arg(resourcePath, QString::fromUtf8(reply.m_data));
        return false;
    }

    // Delete.
    QJsonObject requestDataObj;
    requestDataObj[QStringLiteral("message")] = QString("VX_DEL: %1").arg(resourcePath);
    requestDataObj[QStringLiteral("sha")] = sha;
    auto requestData = Utils::toJsonString(requestDataObj);
    reply = vte::NetworkAccess::deleteResource(QUrl(urlStr), rawHeader, requestData);
    if (reply.m_error != QNetworkReply::NoError) {
        p_msg = tr("Failed to delete resource (%1) (%2).").arg(resourcePath, QString::fromUtf8(reply.m_data));
        return false;
    }

    qDebug() << "deleted resource" << resourcePath;

    return true;
}