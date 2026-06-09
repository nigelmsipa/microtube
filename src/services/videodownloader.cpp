#include "videodownloader.h"

#include <QDir>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QSettings>
#include <QList>

static const qint64 CHUNK_SIZE = 1024 * 1024;

static bool parseContentRange(const QByteArray &header, qint64 *first, qint64 *last, qint64 *total)
{
    // Expected form: "bytes 0-1048575/1234567"
    const QByteArray value = header.trimmed();
    if (!value.startsWith("bytes ")) return false;

    const int dash = value.indexOf('-', 6);
    const int slash = value.indexOf('/', dash + 1);
    if (dash < 0 || slash < 0) return false;

    bool okFirst = false;
    bool okLast = false;
    bool okTotal = false;
    const qint64 parsedFirst = value.mid(6, dash - 6).toLongLong(&okFirst);
    const qint64 parsedLast = value.mid(dash + 1, slash - dash - 1).toLongLong(&okLast);
    const QByteArray totalPart = value.mid(slash + 1);
    const qint64 parsedTotal = totalPart == "*" ? -1 : totalPart.toLongLong(&okTotal);

    if (!okFirst || !okLast || (totalPart != "*" && !okTotal)) return false;

    *first = parsedFirst;
    *last = parsedLast;
    *total = parsedTotal;
    return true;
}

VideoDownloader::VideoDownloader(QObject *parent) :
    QObject(parent),
    _downloadFile(nullptr),
    _reply(nullptr),
    _status(Null),
    _progress(0),
    _audioOnly(false),
    _downloadedBytes(0),
    _totalBytes(-1),
    _currentRangeStart(0),
    _currentRangeEnd(0)
{
    connect(&_jsProcessHelper, &JSProcessManager::gotVideoInfo, this, &VideoDownloader::doDownload);
}

void VideoDownloader::download(QString url, QString path)
{
    _audioOnly = false;
    startDownload(url, path);
}

void VideoDownloader::downloadAudio(QString url, QString path)
{
    _audioOnly = true;
    startDownload(url, path);
}

void VideoDownloader::startDownload(QString url, QString path)
{
    if (_downloadFile != nullptr) return;

    _url = "";
    _downloadedBytes = 0;
    _totalBytes = -1;
    _currentRangeStart = 0;
    _currentRangeEnd = 0;
    setDownloadProgress(0);
    setDownloadStatus(DownloadStatus::Progress);
    emit downloadStarted(path.split('/').last());

    QFileInfo fileInfo(path);
    QDir dir = fileInfo.dir();
    if (!dir.exists() && !dir.mkpath(".")) {
        qWarning() << "Download failed: could not create directory" << dir.absolutePath();
        setDownloadStatus(DownloadStatus::Failed);
        return;
    }

    if (QFile::exists(path)) {
        QFile::remove(path);
    }

    _downloadFile = new QFile(path);

    Search search;
    search.country = QSettings().value("country", "US").toString();
    search.safeSearch = QSettings().value("safeSearch", false).toBool();
    search.type = Search::VideoInfo;
    search.query = url;
    if (!_jsProcessHelper.asyncGetVideoInfo(search)) {
        qWarning() << "Download failed: video info request is already running";
        cleanupFile(true);
        setDownloadStatus(DownloadStatus::Failed);
    }
}

VideoDownloader::DownloadStatus VideoDownloader::getDownloadStatus() const
{
    return _status;
}

void VideoDownloader::setDownloadStatus(DownloadStatus status)
{
    _status = status;

    emit downloadStatusChanged();
}

double VideoDownloader::getDownloadProgress() const
{
    return _progress;
}

void VideoDownloader::setDownloadProgress(double progress)
{
    _progress = progress;

    emit downloadProgressChanged();
}

void VideoDownloader::doDownload(QHash<int, QString> formats)
{
    QString url;
    int selectedItag = 0;
    if (_audioOnly) {
        // Audio-only itags, most compatible first: 140 = m4a/AAC ~128k,
        // 251 = opus ~160k, then lower-bitrate fallbacks.
        const QList<int> audioItags = {140, 141, 251, 250, 249, 139};
        for (int itag : audioItags) {
            if (formats.contains(itag)) {
                qDebug() << "Selecting audio format: " << itag;
                url = formats[itag];
                selectedItag = itag;
                break;
            }
        }
    } else {
        for (QHash<int, QString>::iterator it = formats.begin(); it != formats.end(); it++) {
            if (it.key() == 22 || it.key() == 18) {
                qDebug() << "Selecting download format: " << it.key();
                url = it.value();
                selectedItag = it.key();
                break;
            }
        }
    }
    if (url == "") {
        qWarning() << "Download failed: no compatible" << (_audioOnly ? "audio" : "progressive video") << "format. Available itags:" << formats.keys();
        setDownloadStatus(DownloadStatus::Failed);
        cleanupFile(true);
        return;
    }

    _url = url;
    qDebug() << "Starting" << (_audioOnly ? "audio" : "video") << "download with itag" << selectedItag << "url length" << url.length();

    if (!_downloadFile->open(QIODevice::WriteOnly | QIODevice::Append)) {
        qWarning() << "Download failed: could not open target file" << _downloadFile->fileName() << _downloadFile->errorString();
        setDownloadStatus(DownloadStatus::Failed);
        cleanupFile(true);
        return;
    }

    requestNextChunk();
}

void VideoDownloader::requestNextChunk()
{
    if (_url == "" || _downloadFile == nullptr) {
        setDownloadStatus(DownloadStatus::Failed);
        cleanupFile(true);
        return;
    }

    cleanupReply();

    // The stream URL is already percent-encoded (sparams/sig/pot contain %2C,
    // %3D, ...). QUrl(QString) would re-encode them (%2C -> %252C) and corrupt
    // the signature/token, causing a 403. fromEncoded keeps it byte-for-byte.
    QUrl qurl = QUrl::fromEncoded(_url.toUtf8());
    QNetworkRequest networkRequest(qurl);
    networkRequest.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);

    // The URL is minted by the ANDROID_VR InnerTube client (js/videoInfo.js,
    // c=ANDROID_VR in the URL). Stamp the fetch with the matching ANDROID_VR UA
    // so the request fingerprint lines up with the URL's client. A mismatched UA
    // triggers YouTube's bandwidth throttle (~50–200 KB/s) even when the request
    // succeeds. Mobile-app clients do NOT send Origin/Referer — omitting them is
    // intentional (their presence would itself be a WEB-client fingerprint).
    // (Sailfish Qt 5.6 is HTTP/1.1-only — no HTTP/2 attribute to set.)
    networkRequest.setHeader(QNetworkRequest::UserAgentHeader,
        QByteArrayLiteral("com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip"));

    _currentRangeStart = _downloadedBytes;
    _currentRangeEnd = _currentRangeStart + CHUNK_SIZE - 1;
    networkRequest.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(_currentRangeStart) + "-" + QByteArray::number(_currentRangeEnd));

    _reply = _nam.get(networkRequest);
    QNetworkReply *reply = _reply;

    connect(reply, &QNetworkReply::metaDataChanged, [this, reply]() {
        const QByteArray contentRange = reply->rawHeader("Content-Range");
        qint64 first = 0;
        qint64 last = 0;
        qint64 total = -1;
        if (parseContentRange(contentRange, &first, &last, &total) && total > 0) {
            _totalBytes = total;
        }
    });

    connect(reply, &QNetworkReply::readyRead, [this, reply]() {
        if (_downloadFile == nullptr || reply != _reply) return;
        while (reply->bytesAvailable()) {
            const QByteArray data = reply->read(CHUNK_SIZE);
            if (!data.isEmpty()) {
                _downloadFile->write(data);
            }
        }
    });

    connect(reply, &QNetworkReply::downloadProgress, [this](quint64 bytesReceived, quint64 bytesTotal) {
        Q_UNUSED(bytesTotal);
        if (_totalBytes > 0) {
            setDownloadProgress(((double)_downloadedBytes + bytesReceived) / _totalBytes);
        }
    });

    connect(reply, &QNetworkReply::finished, [this, reply]() {
        if (_downloadFile != nullptr && reply == _reply) {
            while (reply->bytesAvailable()) {
                const QByteArray data = reply->read(CHUNK_SIZE);
                if (!data.isEmpty()) {
                    _downloadFile->write(data);
                }
            }
            _downloadFile->flush();
        }

        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray contentRange = reply->rawHeader("Content-Range");
        qint64 first = 0;
        qint64 last = 0;
        qint64 total = -1;
        const bool hasContentRange = parseContentRange(contentRange, &first, &last, &total);
        if (hasContentRange && total > 0) {
            _totalBytes = total;
        }

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "Download HTTP error" << statusCode << reply->errorString() << "range" << _currentRangeStart << _currentRangeEnd;
            cleanupReply();
            cleanupFile(true);
            setDownloadStatus(DownloadStatus::Failed);
            return;
        }

        if (statusCode == 206 && hasContentRange) {
            _downloadedBytes = last + 1;
            qDebug() << "Downloaded range" << first << last << "of" << _totalBytes;

            cleanupReply();

            if (_totalBytes > 0 && _downloadedBytes < _totalBytes) {
                requestNextChunk();
            } else {
                setDownloadProgress(1);
                cleanupFile(false);
                setDownloadStatus(DownloadStatus::Finished);
            }
            return;
        }

        if (statusCode >= 200 && statusCode < 300) {
            qDebug() << "Download completed with HTTP" << statusCode << "without Content-Range";
            setDownloadProgress(1);
            cleanupReply();
            cleanupFile(false);
            setDownloadStatus(DownloadStatus::Finished);
            return;
        }

        qWarning() << "Download failed with unexpected HTTP status" << statusCode;
        cleanupReply();
        cleanupFile(true);
        setDownloadStatus(DownloadStatus::Failed);
    });
}

void VideoDownloader::cleanupReply()
{
    if (_reply != nullptr) {
        _reply->deleteLater();
        _reply = nullptr;
    }
}

void VideoDownloader::cleanupFile(bool removePartial)
{
    if (_downloadFile != nullptr) {
        const QString filename = _downloadFile->fileName();
        if (_downloadFile->isOpen()) {
            _downloadFile->close();
        }
        delete _downloadFile;
        _downloadFile = nullptr;
        if (removePartial && QFile::exists(filename)) {
            QFile::remove(filename);
        }
    }
}
