/*
 * Unplayer
 * Copyright (C) 2015-2018 Alexey Rochev <equeim@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "directorytracksmodel.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QItemSelectionModel>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QtConcurrentRun>

#include "libraryutils.h"
#include "playlistutils.h"
#include "settings.h"
#include "stdutils.h"

namespace unplayer
{
    void DirectoryTracksModel::classBegin()
    {
    }

    void DirectoryTracksModel::componentComplete()
    {
        mShowVideoFiles = Settings::instance()->showVideoFiles();
        setDirectory(Settings::instance()->defaultDirectory());
    }

    QVariant DirectoryTracksModel::data(const QModelIndex& index, int role) const
    {
        if (!index.isValid()) {
            return QVariant();
        }

        const DirectoryTrackFile& file = mFiles[index.row()];

        switch (role) {
        case FilePathRole:
            return file.filePath;
        case FileNameRole:
            return file.fileName;
        case IsDirectoryRole:
            return file.isDirectory;
        case IsPlaylistRole:
            return file.isPlaylist;
        default:
            return QVariant();
        }
    }

    int DirectoryTracksModel::rowCount(const QModelIndex&) const
    {
        return mFiles.size();
    }

    const std::vector<DirectoryTrackFile>& DirectoryTracksModel::files() const
    {
        return mFiles;
    }

    QString DirectoryTracksModel::directory() const
    {
        return mDirectory;
    }

    void DirectoryTracksModel::setDirectory(QString newDirectory)
    {
        QDir dir(newDirectory);
        if (!dir.isReadable()) {
            qWarning() << "directory is not readable:" << newDirectory;
            newDirectory = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
        } else {
            newDirectory = dir.absolutePath();
        }
        if (newDirectory != mDirectory) {
            mDirectory = newDirectory;
            emit directoryChanged();
            loadDirectory();
        }
    }

    QString DirectoryTracksModel::parentDirectory() const
    {
        return QFileInfo(mDirectory).path();
    }

    bool DirectoryTracksModel::isLoaded() const
    {
        return mLoaded;
    }

    QString DirectoryTracksModel::getTrack(int index) const
    {
        return mFiles[index].filePath;
    }

    QStringList DirectoryTracksModel::getTracks(const std::vector<int>& indexes, bool includePlaylists) const
    {
        QStringList list;
        for (int index : indexes) {
            const DirectoryTrackFile& file = mFiles[index];
            if (!file.isDirectory && (!file.isPlaylist || includePlaylists)) {
                list.append(getTrack(index));
            }
        }
        return list;
    }

    void DirectoryTracksModel::removeTrack(int index)
    {
        removeTracks({index});
    }

    void DirectoryTracksModel::removeTracks(std::vector<int> indexes)
    {
        if (mRemovingFiles || !mLoaded) {
            return;
        }

        mRemovingFiles = true;
        emit removingFilesChanged();

        auto future = QtConcurrent::run(std::bind([this](std::vector<int>& indexes) {
            std::vector<int> removed;
            auto db = QSqlDatabase::addDatabase(LibraryUtils::databaseType, staticMetaObject.className());
            db.setDatabaseName(LibraryUtils::instance()->databaseFilePath());
            if (!db.open()) {
                QSqlDatabase::removeDatabase(staticMetaObject.className());
                qWarning() << "failed to open database" << db.lastError();
                return removed;
            }
            db.transaction();
            std::sort(indexes.begin(), indexes.end(), std::greater<int>());
            for (int index : indexes) {
                QString filePath(mFiles[index].filePath);
                const bool isDir = QFileInfo(filePath).isDir();
                if (isDir) {
                    if (!QDir(filePath).removeRecursively()) {
                        qWarning() << "failed to remove directory:" << filePath;
                        continue;
                    }
                } else {
                    if (!QFile::remove(filePath)) {
                        qWarning() << "failed to remove file:" << filePath;
                        continue;
                    }
                }
                removed.push_back(index);

                QSqlQuery query(db);
                if (isDir) {
                    query.prepare(QStringLiteral("DELETE FROM tracks WHERE instr(filePath, ?) = 1"));
                    filePath.append('/');
                } else {
                    query.prepare(QStringLiteral("DELETE FROM tracks WHERE filePath = ?"));
                }
                query.addBindValue(filePath);
                if (!query.exec()) {
                    qWarning() << "failed to remove file from database" << query.lastQuery();
                }
            }
            db.commit();
            QSqlDatabase::removeDatabase(db.connectionName());

            return removed;
        }, std::move(indexes)));

        using Watcher = QFutureWatcher<std::vector<int>>;
        auto watcher = new Watcher(this);
        QObject::connect(watcher, &Watcher::finished, this, [this, watcher]() {
            mRemovingFiles = false;
            emit removingFilesChanged();

            for (int index : watcher->result()) {
                beginRemoveRows(QModelIndex(), index, index);
                mFiles.erase(mFiles.begin() + index);
                endRemoveRows();
            }
            emit LibraryUtils::instance()->databaseChanged();
            watcher->deleteLater();
        });
        watcher->setFuture(future);
    }

    QHash<int, QByteArray> DirectoryTracksModel::roleNames() const
    {
        return {{FilePathRole, "filePath"},
                {FileNameRole, "fileName"},
                {IsDirectoryRole, "isDirectory"},
                {IsPlaylistRole, "isPlaylist"}};
    }

    void DirectoryTracksModel::loadDirectory()
    {
        if (mRemovingFiles) {
            return;
        }

        mLoaded = false;
        emit loadedChanged();

        beginRemoveRows(QModelIndex(), 0, mFiles.size() - 1);
        mFiles.clear();
        endRemoveRows();

        const QString directory(mDirectory);
        const bool showVideoFiles = mShowVideoFiles;
        auto future = QtConcurrent::run([directory, showVideoFiles]() {
            std::vector<DirectoryTrackFile> files;
            const QMimeDatabase mimeDb;
            const QList<QFileInfo> fileInfos(QDir(directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Files | QDir::Readable));
            for (const QFileInfo& info : fileInfos) {
                if (info.isDir()) {
                    files.push_back({info.filePath(),
                                     info.fileName(),
                                     true,
                                     false});
                } else {
                    const QString mimeType(mimeDb.mimeTypeForFile(info, QMimeDatabase::MatchExtension).name());
                    const bool isPlaylist = contains(PlaylistUtils::playlistsExtensions, info.suffix());
                    if (isPlaylist
                            || contains(LibraryUtils::mimeTypesByExtension, mimeType)
                            || (showVideoFiles && contains(LibraryUtils::videoMimeTypesByExtension, mimeType))) {
                        files.push_back({info.filePath(),
                                         info.fileName(),
                                         false,
                                         isPlaylist});
                    }
                }
            }
            return files;
        });

        using FutureWatcher = QFutureWatcher<std::vector<DirectoryTrackFile>>;
        auto watcher = new FutureWatcher(this);
        QObject::connect(watcher, &FutureWatcher::finished, this, [=]() {
            auto files(watcher->result());
            beginInsertRows(QModelIndex(), 0, files.size() - 1);
            mFiles = std::move(files);
            endInsertRows();

            mLoaded = true;
            emit loadedChanged();
        });
        watcher->setFuture(future);
    }

    bool DirectoryTracksModel::isRemovingFiles() const
    {
        return mRemovingFiles;
    }

    DirectoryTracksProxyModel::DirectoryTracksProxyModel()
        : mDirectoriesCount(0),
          mTracksCount(0)
    {
        setFilterRole(DirectoryTracksModel::FileNameRole);
        setSortEnabled(true);
        setSortRole(DirectoryTracksModel::FileNameRole);
        setIsDirectoryRole(DirectoryTracksModel::IsDirectoryRole);
    }

    void DirectoryTracksProxyModel::componentComplete()
    {
        auto updateCount = [=]() {
            mDirectoriesCount = 0;
            mTracksCount = 0;
            const std::vector<DirectoryTrackFile>& files = static_cast<const DirectoryTracksModel*>(sourceModel())->files();
            for (int i = 0, max = rowCount(); i < max; ++i) {
                if (files.at(sourceIndex(i)).isDirectory) {
                    mDirectoriesCount++;
                } else {
                    mTracksCount++;
                }
            }
            emit countChanged();
        };

        QObject::connect(this, &QAbstractItemModel::modelReset, this, updateCount);
        QObject::connect(this, &QAbstractItemModel::rowsInserted, this, updateCount);
        QObject::connect(this, &QAbstractItemModel::rowsRemoved, this, updateCount);

        updateCount();

        DirectoryContentProxyModel::componentComplete();
    }

    int DirectoryTracksProxyModel::directoriesCount() const
    {
        return mDirectoriesCount;
    }

    int DirectoryTracksProxyModel::tracksCount() const
    {
        return mTracksCount;
    }

    QStringList DirectoryTracksProxyModel::getSelectedTracks() const
    {
        const std::vector<int> indexes(selectedSourceIndexes());
        QStringList tracks;
        tracks.reserve(indexes.size());
        auto model = static_cast<const DirectoryTracksModel*>(sourceModel());
        for (int index : indexes) {
            tracks.push_back(model->getTrack(index));
        }
        return tracks;
    }

    void DirectoryTracksProxyModel::selectAll()
    {
        const std::vector<DirectoryTrackFile>& files = static_cast<const DirectoryTracksModel*>(sourceModel())->files();
        for (int i = 0, max = rowCount(); i < max; ++i) {
            if (!files[sourceIndex(i)].isDirectory) {
                selectionModel()->select(index(i, 0), QItemSelectionModel::Select);
            }
        }
    }
}
