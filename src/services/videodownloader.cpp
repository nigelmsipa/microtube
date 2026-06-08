#include "videodownloader.h"

#include <QNetworkRequest>
#include <QSettings>
#include <QList>


VideoDownloader::VideoDownloader(QObject *parent) : QObject(parent), _downloadFile(nullptr), _reply(nullptr), _status(Null), _progress(0), _audioOnly(false)
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

    setDownloadStatus(DownloadStatus::Progress);
    emit downloadStarted(path.split('/').last());

    if (QFile::exists(path)) {
        QFile::remove(path);
    }

    _downloadFile = new QFile(path);

    Search search;
    search.country = QSettings().value("country", "US").toString();
    search.safeSearch = QSettings().value("safeSearch", false).toBool();
    search.type = Search::VideoInfo;
    search.query = url;
    _jsProcessHelper.asyncGetVideoInfo(search);
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
    if (_audioOnly) {
        // Audio-only itags, most compatible first: 140 = m4a/AAC ~128k,
        // 251 = opus ~160k, then lower-bitrate fallbacks.
        const QList<int> audioItags = {140, 141, 251, 250, 249, 139};
        for (int itag : audioItags) {
            if (formats.contains(itag)) {
                qDebug() << "Selecting audio format: " << itag;
                url = formats[itag];
                break;
            }
        }
    } else {
        for (QHash<int, QString>::iterator it = formats.begin(); it != formats.end(); it++) {
            if (it.key() == 22 || it.key() == 18) {
                qDebug() << "Selecting download format: " << it.key();
                url = it.value();
                break;
            }
        }
    }
    if (url == "") {
        setDownloadStatus(DownloadStatus::Failed);
        delete _downloadFile;
        _downloadFile = nullptr;
        return;
    }

    // The stream URL is already percent-encoded (sparams/sig/pot contain %2C,
    // %3D, ...). QUrl(QString) would re-encode them (%2C -> %252C) and corrupt
    // the signature/token, causing a 403. fromEncoded keeps it byte-for-byte.
    QUrl qurl = QUrl::fromEncoded(url.toUtf8());
    QNetworkRequest networkRequest(qurl);
    networkRequest.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    _reply = _nam.get(networkRequest);

    if (_downloadFile->open(QIODevice::WriteOnly | QIODevice::Append)) {
        connect(_reply, &QNetworkReply::readyRead, [this]() {
            while (_reply->bytesAvailable()) {
                _downloadFile->write(_reply->read(1048576));
            }
        });
        connect (_reply, &QNetworkReply::downloadProgress, [this](quint64 bytesReceived, quint64 bytesTotal) {
            setDownloadProgress(((double)bytesReceived)/bytesTotal);
        });
        connect(_reply, &QNetworkReply::finished, [this]() {
            _downloadFile->close();
            delete _downloadFile;
            _downloadFile = nullptr;

            if (_reply->error() == QNetworkReply::NoError) {
                setDownloadStatus(DownloadStatus::Finished);
            } else {
                qWarning() << _reply->errorString();
                setDownloadStatus(DownloadStatus::Failed);
            }
        });
    } else {
        setDownloadStatus(DownloadStatus::Failed);
        delete _downloadFile;
        _downloadFile = nullptr;
        return;
    }
}
