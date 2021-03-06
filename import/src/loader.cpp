/*
 * Copyright (C) 2015  Malte Veerman <malte.veerman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */


#include "loader.h"

#include "guibase.h"
#include "hwmon.h"
#include "pwmfan.h"
#include "fan.h"
#include "fancontrolaction.h"

#include <QtCore/QFile>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QDir>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>

#include <KAuth/KAuthExecuteJob>
#include <KI18n/KLocalizedString>


#define HWMON_PATH "/sys/class/hwmon"

#ifndef STANDARD_CONFIG_FILE
#define STANDARD_CONFIG_FILE "/etc/fancontrol"
#endif


namespace Fancontrol
{

Loader::Loader(GUIBase *parent) : QObject(parent),
    m_reactivateAfterTesting(true),
    m_interval(10),
    m_configUrl(QUrl::fromLocalFile(QStringLiteral(STANDARD_CONFIG_FILE))),
    m_timer(new QTimer(this)),
    m_watcher(new QFileSystemWatcher(this)),
    m_sensorsDetected(false)
{
    if (parent)
    {
        connect(this, &Loader::error, parent, &GUIBase::handleError);
        connect(this, &Loader::info, parent, &GUIBase::handleInfo);
    }

    m_timer->setSingleShot(false);
    m_timer->start(1000);

    connect(m_timer, &QTimer::timeout, this, &Loader::sensorsUpdateNeeded);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &Loader::configFileChanged);
    connect(this, &Loader::configFileChanged, this, &Loader::needsSaveChanged);
}

void Loader::parseHwmons(QString path)
{
    if (path.isEmpty())
        path = QStringLiteral(HWMON_PATH);

    const auto hwmonDir = QDir(path);
    QStringList list;
    if (hwmonDir.isReadable())
        list = hwmonDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);

    else if (hwmonDir.exists())
    {
        emit error(i18n("Hwmon path is not readable: \'%1\'", path), true);
        return;
    }
    else
    {
        emit error(i18n("Hwmon path does not exist: \'%1\'", path), true);
        return;
    }

    QStringList dereferencedList;
    while (!list.isEmpty())
        dereferencedList << QFile::symLinkTarget(hwmonDir.absoluteFilePath(list.takeFirst()));

    for (auto &hwmon : m_hwmons)
    {
        if (!dereferencedList.contains(hwmon->path()))
        {
            hwmon->deleteLater();
            m_hwmons.removeOne(hwmon);
            emit hwmonsChanged();
        }
        else
            hwmon->initialize();
    }

    for (const auto &hwmonPath : qAsConst(dereferencedList))
    {
        auto hwmonExists = false;

        for (const auto &hwmon : qAsConst(m_hwmons))
        {
            if (hwmon->path() == hwmonPath)
            {
                hwmonExists = true;
                break;
            }
        }

        if (!hwmonExists)
        {
            auto newHwmon = new Hwmon(hwmonPath, this);
            if (newHwmon->isValid())
            {
                connect(this, &Loader::sensorsUpdateNeeded, newHwmon, &Hwmon::sensorsUpdateNeeded);
                m_hwmons << newHwmon;
                emit hwmonsChanged();
            }
            else
                delete newHwmon;
        }
    }

    m_watcher->addPath(path);
}

PwmFan * Loader::pwmFan(uint hwmonIndex, uint pwmFanIndex) const
{
    const auto hwmon = m_hwmons.value(hwmonIndex, Q_NULLPTR);

    if (!hwmon)
        return Q_NULLPTR;

    return hwmon->pwmFans().value(pwmFanIndex);
}

Temp * Loader::temp(uint hwmonIndex, uint tempIndex) const
{
    const auto hwmon = m_hwmons.value(hwmonIndex, Q_NULLPTR);

    if (!hwmon)
        return Q_NULLPTR;

    return hwmon->temps().value(tempIndex);
}

Fan * Loader::fan(uint hwmonIndex, uint fanIndex) const
{
    const auto hwmon = m_hwmons.value(hwmonIndex, Q_NULLPTR);

    if (!hwmon)
        return Q_NULLPTR;

    return hwmon->fans().value(fanIndex);
}

QPair<uint, uint> Loader::getEntryNumbers(const QString &entry)
{
    if (entry.isEmpty())
        return QPair<uint, uint>(0, 0);

    auto list = entry.split('/', QString::SkipEmptyParts);
    if (list.size() != 2)
    {
        emit error(i18n("Invalid entry: \'%1\'", entry));
        return QPair<uint, uint>(0, 0);
    }
    auto &hwmon = list[0];
    auto &sensor = list[1];

    if (!hwmon.startsWith(QStringLiteral("hwmon")))
    {
        emit error(i18n("Invalid entry: \'%1\'", entry));
        return QPair<uint, uint>(0, 0);
    }
    if (!sensor.contains(QRegExp("^(pwm|fan|temp)\\d+")))
    {
        emit error(i18n("Invalid entry: \'%1\'", entry));
        return QPair<uint, uint>(0, 0);
    }

    auto success = false;

    hwmon.remove(QStringLiteral("hwmon"));
    sensor.remove(QRegExp("^(pwm|fan|temp)"));
    sensor.remove(QStringLiteral("_input"));

    const auto hwmonResult = hwmon.toUInt(&success);
    if (!success)
    {
        emit error(i18n("Invalid entry: \'%1\'", entry));
        return QPair<uint, uint>(0, 0);
    }
    const auto sensorResult = sensor.toUInt(&success);
    if (!success)
    {
        emit error(i18n("Invalid entry: \'%1\'", entry));
        return QPair<uint, uint>(0, 0);
    }

    return QPair<uint, uint>(hwmonResult, sensorResult);
}

bool Loader::parseConfig(QString config)
{
    //Disconnect hwmons for performance reasons
    //They get reconnected later
    for (const auto &hwmon : qAsConst(m_hwmons))
    {
        disconnect(hwmon, &Hwmon::configUpdateNeeded, this, &Loader::updateConfig);
    }

    toDefault();

    bool success = true;
    QTextStream stream;
    stream.setString(&config, QIODevice::ReadOnly);
    QStringList lines;
    do
    {
        auto line(stream.readLine());

        if (line.startsWith('#') || line.trimmed().isEmpty())
            continue;

        const auto offset = line.indexOf('#');

        if (offset != -1)
            line.truncate(offset);

        line = line.simplified();
        lines << line;
    }
    while (!stream.atEnd());

    for (auto line : qAsConst(lines))
    {
        if (line.startsWith(QStringLiteral("INTERVAL=")))
        {
            line.remove(QStringLiteral("INTERVAL="));
            line = line.simplified();
            auto intSuccess = false;
            const auto interval = line.toUInt(&intSuccess);

            if (intSuccess)
                setInterval(interval, false);
            else
            {
                emit error(i18n("Unable to parse interval line: \'%1\'", line), true);
                success = false;
            }
        }
        else if (line.startsWith(QStringLiteral("FCTEMPS=")))
        {
            line.remove(QStringLiteral("FCTEMPS="));
            line = line.simplified();
            const auto fctemps = line.split(' ');
            for (const auto &fctemp : fctemps)
            {
                const auto nameValuePair = fctemp.split('=');
                if (nameValuePair.size() == 2)
                {
                    const auto pwmFanString = nameValuePair.at(0);
                    const auto tempString = nameValuePair.at(1);
                    const auto pwmPointer = pwmFan(getEntryNumbers(pwmFanString));
                    const auto tempPointer = temp(getEntryNumbers(tempString));

                    if (pwmPointer && tempPointer)
                    {
                        pwmPointer->setTemp(tempPointer);
                        pwmPointer->setHasTemp(true);
                        pwmPointer->setMinPwm(0);
                    }
                    else
                    {
                        if (!pwmPointer)
                            emit error(i18n("Invalid fan entry: \'%1\'", pwmFanString), true);

                        if (!tempPointer)
                            emit error(i18n("Invalid temp entry: \'%1\'", tempString), true);
                    }
                }
                else
                    emit error(i18n("Invalid entry: \'%1\'", fctemp), true);
            }
        }
        else if (line.startsWith(QStringLiteral("DEVNAME=")))
        {
            line.remove(QStringLiteral("DEVNAME="));
            line = line.simplified();
            const auto devnames = line.split(' ');
            for (const auto &devname : devnames)
            {
                const auto indexNamePair = devname.split('=');

                if (indexNamePair.size() == 2)
                {
                    auto index = indexNamePair.at(0);
                    const auto &name = indexNamePair[1];
                    auto intSuccess = false;
                    index.remove(QStringLiteral("hwmon"));
                    const auto hwmonPointer = m_hwmons.value(index.toUInt(&intSuccess), Q_NULLPTR);

                    if (!intSuccess)
                    {
                        emit error(i18n("Invalid DEVNAME: \'%1\'!", devname), true);
                        success = false;
                    }

                    if (!hwmonPointer)
                    {
                        emit error(i18n("Invalid DEVNAME: \'%1\'! No hwmon with index %2", devname, index), true);
                        success = false;
                    }
                    else if (hwmonPointer->name().split('.').first() != name)
                    {
                        emit error(i18n("Wrong name for hwmon %1! Should be \'%2\'", index, hwmonPointer->name().split('.').first()), true);
                        success = false;
                    }
                }
                else
                    emit error(i18n("Invalid DEVNAME: \'%1\'!", devname), true);
            }
        }
        else if (line.startsWith(QStringLiteral("MINTEMP=")))
        {
            line.remove(QStringLiteral("MINTEMP="));
            parseConfigLine(line.simplified(), &PwmFan::setMinTemp);
        }
        else if (line.startsWith(QStringLiteral("MAXTEMP=")))
        {
            line.remove(QStringLiteral("MAXTEMP="));
            parseConfigLine(line.simplified(), &PwmFan::setMaxTemp);
        }
        else if (line.startsWith(QStringLiteral("MINSTART=")))
        {
            line.remove(QStringLiteral("MINSTART="));
            parseConfigLine(line.simplified(), &PwmFan::setMinStart);
        }
        else if (line.startsWith(QStringLiteral("MINSTOP=")))
        {
            line.remove(QStringLiteral("MINSTOP="));
            parseConfigLine(line.simplified(), &PwmFan::setMinStop);
        }
        else if (line.startsWith(QStringLiteral("MINPWM=")))
        {
            line.remove(QStringLiteral("MINPWM="));
            parseConfigLine(line.simplified(), &PwmFan::setMinPwm);
        }
        else if (line.startsWith(QStringLiteral("MAXPWM=")))
        {
            line.remove(QStringLiteral("MAXPWM="));
            parseConfigLine(line.simplified(), &PwmFan::setMaxPwm);
        }
        else if (line.startsWith(QStringLiteral("AVERAGE=")))
        {
            line.remove(QStringLiteral("AVERAGE="));
            parseConfigLine(line.simplified(), &PwmFan::setAverage);
        }
        else if (!line.startsWith(QStringLiteral("DEVPATH=")) &&
            !line.startsWith(QStringLiteral("FCFANS=")))
        {
            emit error(i18n("Unrecognized line in config: \'%1\'", line), true);
            success = false;
        }
    }

    updateConfig();

    //Connect hwmons again
    for (const auto &hwmon : qAsConst(m_hwmons))
        connect(hwmon, &Hwmon::configUpdateNeeded, this, &Loader::updateConfig);

    return success;
}

void Loader::parseConfigLine(const QString &line, void (PwmFan::*memberSetFunction)(int))
{
    if (!memberSetFunction)
        return;

    const auto entries = line.split(' ');

    for (const auto &entry : entries)
    {
        const auto fanValuePair = entry.split('=');
        if (fanValuePair.size() == 2)
        {
            const auto pwmFanString = fanValuePair.at(0);
            const auto valueString = fanValuePair.at(1);
            auto success = false;
            const auto value = valueString.toUInt(&success);

            if (success)
            {
                auto pwmFanPointer = pwmFan(getEntryNumbers(pwmFanString));
                if (pwmFanPointer)
                    (pwmFanPointer->*memberSetFunction)(value);
                else
                    emit error(i18n("Invalid fan entry: \'%1\'", pwmFanString), true);
            }
            else
                emit error(i18n("%1 is not an integer!", valueString));
        }
        else
            emit error(i18n("Invalid entry to parse: \'%1\'", entry));
    }
}

bool Loader::load(const QUrl &url)
{
    QString filePath;
    if (url.isEmpty())
        filePath = m_configUrl.toLocalFile();
    else if (url.isValid())
    {
        if (url.isLocalFile())
            filePath = url.toLocalFile();

        else
        {
            emit error(i18n("\'%1\' is not a local file!", url.toDisplayString()));
            return false;
        }
    }
    else
    {
        emit error(i18n("\'%1\' is not a valid url!", url.toDisplayString()));
        return false;
    }
    emit info(i18n("Loading config file: \'%1\'", filePath));

    watchPath(filePath);

    QTextStream stream;
    QFile file(filePath);
    QString fileContent;

    if (file.exists())
    {
        if (file.open(QFile::ReadOnly | QFile::Text))
        {
            stream.setDevice(&file);
            fileContent = stream.readAll();
        }
        else
        {
            auto action = newFancontrolAction();

            if (action.isValid())
            {
                auto map = QVariantMap();
                map[QStringLiteral("action")] = QVariant("read");
                map[QStringLiteral("filename")] = filePath;
                action.setArguments(map);
                auto job = action.execute();
                if (!job->exec())
                {
                    if (job->error() == 4)
                    {
                        emit info(i18n("Loading of file aborted by user"));
                        return false;
                    }

                    emit error(i18n("KAuth::ExecuteJob error! Code: %1\nAdditional Info: %2", job->error(), job->errorString()), true);
                    return false;
                }
                else
                    fileContent = job->data().value(QStringLiteral("content")).toString();
            }
            else
                emit error(i18n("Action not supported! Try running the application as root."), true);
        }

        bool success = load(fileContent);

        if (!url.isEmpty())
        {
            m_configUrl = url;
            emit configUrlChanged();
        }

        return success;
    }
    else
    {
        emit error(i18n("File does not yet exist: \'%1\'" ,filePath));

        m_loadedConfig = QString();
        emit needsSaveChanged();

        if (!url.isEmpty())
        {
            m_configUrl = url;
            emit configUrlChanged();
        }

        return false;
    }

    return false;
}

bool Loader::load(const QString& config)
{
    if (m_config == config)
        return true;

    if (config.isEmpty())
    {
        emit error(i18n("Cannot load empty config."), true);
        return false;
    }

    bool success = parseConfig(config);

    m_loadedConfig = config;
    emit needsSaveChanged();

    return success;
}

bool Loader::save(const QUrl &url)
{
    QString filePath;
    if (url.isEmpty())
    {
//        qDebug() << "Given empty url. Fallback to " << m_configUrl;
        filePath = m_configUrl.toLocalFile();
    }
    else if (url.isLocalFile())
    {
        filePath = url.toLocalFile();
        m_configUrl = url;
        emit configUrlChanged();
    }
    else
    {
        emit error(i18n("\'%1\' is not a local file!", url.toDisplayString()), true);
        return false;
    }
    QFile file(filePath);

    if (file.open(QFile::ReadOnly | QFile::Text))
    {
        QTextStream stream(&file);
        QString fileContent = stream.readAll();

        if (m_config == fileContent)
        {
            emit info(i18n("No changes made to config"));
            return false;
        }
        else
            file.close();
    }

    emit info(i18n("Saving config to \'%1\'", filePath));
    if (file.open(QFile::WriteOnly | QFile::Text))
    {
        QTextStream stream(&file);
        stream << m_config;
    }
    else
    {
        auto action = newFancontrolAction();

        if (action.isValid())
        {
            QVariantMap map;
            map[QStringLiteral("action")] = QVariant("write");
            map[QStringLiteral("filename")] = filePath;
            map[QStringLiteral("content")] = m_config;

            action.setArguments(map);
            auto job = action.execute();

            if (!job->exec())
            {
                if (job->error() == 4)
                {
                    emit info(i18n("Saving of file aborted by user"));
                    return false;
                }

                emit error(i18n("Error executing action. Code %1; %2; %3", job->error(), job->errorString(), job->errorText()), true);
                return false;
            }
        }
        else
        {
            emit error(i18n("Action not supported! Try running the application as root."), true);
            return false;
        }
    }

    m_loadedConfig = m_config;
    emit configChanged();

    return true;
}

void Loader::reset()
{
    if (m_config == m_loadedConfig)
        return;

    load(m_loadedConfig);
}

void Loader::updateConfig()
{
    const auto config = createConfig();

    if (config != m_config)
    {
        m_config = config;
        emit configChanged();
        emit needsSaveChanged();
    }
}

QString Loader::createConfig() const
{
    QList<Hwmon *> usedHwmons;
    QList<PwmFan *> usedFans;

    for (const auto &hwmon : m_hwmons)
    {
        if (hwmon->pwmFans().size() > 0 && !usedHwmons.contains(hwmon))
            usedHwmons << hwmon;

        const auto pwmFans = hwmon->pwmFans();
        auto keys = pwmFans.keys();
        std::sort(keys.begin(), keys.end());
        for (const auto &key : keys)
        {
            const auto pwmFan = pwmFans.value(key);
            if (pwmFan->hasTemp() && pwmFan->temp() && !pwmFan->testing())
            {
                usedFans << pwmFan;
                if (!usedHwmons.contains(pwmFan->temp()->parent()))
                    usedHwmons << pwmFan->temp()->parent();
            }
        }
    }

    std::sort(usedHwmons.begin(), usedHwmons.end(), [] (Hwmon *a, Hwmon *b) { return a->index() < b->index(); });

    auto configFile = QStringLiteral("# This file was created by Fancontrol-GUI") + QChar(QChar::LineFeed);

    if (m_interval != 0)
        configFile += "INTERVAL=" + QString::number(m_interval) + QChar(QChar::LineFeed);

    if (!usedHwmons.isEmpty())
    {
        configFile += QLatin1String("DEVPATH=");

        for (const auto &hwmon : qAsConst(usedHwmons))
        {
            auto sanitizedPath = hwmon->path();
            sanitizedPath.remove(QRegExp("^/sys/"));
            sanitizedPath.remove(QRegExp("/hwmon/hwmon\\d\\s*$"));
            configFile += "hwmon" + QString::number(hwmon->index()) + "=" + sanitizedPath + QChar(QChar::Space);
        }
        configFile += QChar(QChar::LineFeed);

        configFile += QLatin1String("DEVNAME=");

        for (const auto &hwmon : qAsConst(usedHwmons))
            configFile += "hwmon" + QString::number(hwmon->index()) + "=" + hwmon->name().split('.').first() + QChar(QChar::Space);

        configFile += QChar(QChar::LineFeed);

        if (!usedFans.isEmpty())
        {
            configFile += QLatin1String("FCTEMPS=");

            for (const auto &pwmFan : qAsConst(usedFans))
            {
                configFile += "hwmon" + QString::number(pwmFan->parent()->index()) + "/";
                configFile += "pwm" + QString::number(pwmFan->index()) + "=";
                configFile += "hwmon" + QString::number(pwmFan->temp()->parent()->index()) + "/";
                configFile += "temp" + QString::number(pwmFan->temp()->index()) + "_input ";
            }
            configFile += QChar(QChar::LineFeed);

            configFile += QLatin1String("FCFANS=");

            for (const auto &pwmFan : qAsConst(usedFans))
            {
                configFile += "hwmon" + QString::number(pwmFan->parent()->index()) + "/";
                configFile += "pwm" + QString::number(pwmFan->index()) + "=";
                configFile += "hwmon" + QString::number(pwmFan->parent()->index()) + "/";
                configFile += "fan" + QString::number(pwmFan->index()) + "_input ";
            }
            configFile += QChar(QChar::LineFeed);

            configFile += QLatin1String("MINTEMP=");

            for (const auto &pwmFan : qAsConst(usedFans))
            {
                configFile += "hwmon" + QString::number(pwmFan->parent()->index()) + "/";
                configFile += "pwm" + QString::number(pwmFan->index()) + "=";
                configFile += QString::number(pwmFan->minTemp()) + QChar(QChar::Space);
            }
            configFile += QChar(QChar::LineFeed);

            configFile += QLatin1String("MAXTEMP=");

            for (const auto &pwmFan : qAsConst(usedFans))
            {
                configFile += "hwmon" + QString::number(pwmFan->parent()->index()) + "/";
                configFile += "pwm" + QString::number(pwmFan->index()) + "=";
                configFile += QString::number(pwmFan->maxTemp()) + QChar(QChar::Space);
            }
            configFile += QChar(QChar::LineFeed);

            configFile += QLatin1String("MINSTART=");

            for (const auto &pwmFan : qAsConst(usedFans))
            {
                configFile += "hwmon" + QString::number(pwmFan->parent()->index()) + "/";
                configFile += "pwm" + QString::number(pwmFan->index()) + "=";
                configFile += QString::number(pwmFan->minStart()) + QChar(QChar::Space);
            }
            configFile += QChar(QChar::LineFeed);

            configFile += QLatin1String("MINSTOP=");

            for (const auto &pwmFan : qAsConst(usedFans))
            {
                configFile += "hwmon" + QString::number(pwmFan->parent()->index()) + "/";
                configFile += "pwm" + QString::number(pwmFan->index()) + "=";
                configFile += QString::number(pwmFan->minStop()) + QChar(QChar::Space);
            }
            configFile += QChar(QChar::LineFeed);

            configFile += QLatin1String("MINPWM=");

            for (const auto &pwmFan : qAsConst(usedFans))
            {
                configFile += "hwmon" + QString::number(pwmFan->parent()->index()) + "/";
                configFile += "pwm" + QString::number(pwmFan->index()) + "=";
                configFile += QString::number(pwmFan->minPwm()) + QChar(QChar::Space);
            }
            configFile += QChar(QChar::LineFeed);

            configFile += QLatin1String("MAXPWM=");

            for (const auto &pwmFan : qAsConst(usedFans))
            {
                configFile += "hwmon" + QString::number(pwmFan->parent()->index()) + "/";
                configFile += "pwm" + QString::number(pwmFan->index()) + "=";
                configFile += QString::number(pwmFan->maxPwm()) + QChar(QChar::Space);
            }
            configFile += QChar(QChar::LineFeed);

            configFile += QLatin1String("AVERAGE=");

            for (const auto &pwmFan : qAsConst(usedFans))
            {
                configFile += "hwmon" + QString::number(pwmFan->parent()->index()) + "/";
                configFile += "pwm" + QString::number(pwmFan->index()) + "=";
                configFile += QString::number(pwmFan->average()) + QChar(QChar::Space);
            }
            configFile += QChar(QChar::LineFeed);
        }
    }

    return configFile;
}

bool Loader::watchPath(const QString& path)
{
    if (m_watcher->files().contains(path))
        return true;

    m_watcher->removePaths(m_watcher->files());
    return m_watcher->addPath(path);
}

void Loader::setInterval(int interval, bool writeNewConfig)
{
    if (interval < 1)
    {
        emit error(i18n("Interval must be greater or equal to one!"), true);
        return;
    }

    if (interval != m_interval)
    {
        m_interval = interval;
        emit intervalChanged();

        if (writeNewConfig)
            updateConfig();
    }
}

void Loader::testFans()
{
    for (const auto &hwmon : qAsConst(m_hwmons))
        hwmon->testFans();
}

void Loader::abortTestingFans()
{
    for (const auto &hwmon : qAsConst(m_hwmons))
        hwmon->abortTestingFans();
}

QList<QObject *> Loader::hwmonsAsObjects() const
{
    auto list = QList<QObject *>();
    for (const auto &hwmon : m_hwmons)
        list << hwmon;

    return list;
}

void Loader::handleTestStatusChanged()
{
    auto testing = false;

    for (const auto &hwmon : qAsConst(m_hwmons))
    {
        if (hwmon->testing() == true)
        {
            testing = true;
            break;
        }
    }

    if (!testing && !m_reactivateAfterTesting)
        return;

    emit requestSetServiceActive(!testing);
}

void Loader::setRestartServiceAfterTesting(bool restart)
{
    if (m_reactivateAfterTesting == restart)
        return;

    m_reactivateAfterTesting = restart;
    emit restartServiceAfterTestingChanged();
}

void Loader::toDefault()
{
    for (const auto &hwmon : qAsConst(m_hwmons))
        hwmon->toDefault();
    }
}
