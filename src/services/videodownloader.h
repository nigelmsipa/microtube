#ifndef VIDEODOWNLOADER_H
#define VIDEODOWNLOADER_H

#include <QObject>
#include <QFile>
#include <QNetworkAccessManager>
#include <QUrl>
#include <QNetworkReply>
#include "managers/jsprocessmanager.h"

class VideoDownloader : public QObject
{
    Q_OBJECT
    Q_PROPERTY(DownloadStatus downloadStatus READ getDownloadStatus NOTIFY downloadStatusChanged)
    Q_PROPERTY(double downloadProgress READ getDownloadProgress NOTIFY downloadProgressChanged)
public:
    enum DownloadStatus {
        Null,
        Progress,
        Finished,
        Failed
    };
    Q_ENUM(DownloadStatus)

    explicit VideoDownloader(QObject *parent = nullptr);

    Q_INVOKABLE void download(QString url, QString path);
    Q_INVOKABLE void downloadAudio(QString url, QString path);
    DownloadStatus getDownloadStatus() const;
    void setDownloadStatus(DownloadStatus status);
    double getDownloadProgress() const;
    void setDownloadProgress(double progress);

private:
    void startDownload(QString url, QString path);
    void requestNextChunk();
    void cleanupReply();
    void cleanupFile(bool removePartial);

private slots:
    void doDownload(QHash<int, QString> formats);

signals:
    void downloadStatusChanged();
    void downloadProgressChanged();
    void downloadStarted(QString filename);

private:
    JSProcessManager _jsProcessHelper;
    QFile *_downloadFile;
    QNetworkAccessManager _nam;
    QNetworkReply *_reply;
    DownloadStatus _status;
    double _progress;
    bool _audioOnly;
    QString _url;
    qint64 _downloadedBytes;
    qint64 _totalBytes;
    qint64 _currentRangeStart;
    qint64 _currentRangeEnd;
};

#endif // VIDEODOWNLOADER_H
