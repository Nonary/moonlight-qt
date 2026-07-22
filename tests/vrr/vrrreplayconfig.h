#pragma once

#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

struct VrrReplayWorkerParameters {
    size_t queueCapacity = 3;
    size_t rollingCostWindow = 32;
    uint64_t staleSourcePeriods = 1;
};

struct VrrReplayAssertion {
    QString metric;
    QString operation;
    double value = 0;
};

struct VrrReplayScenario {
    QString name = "candidate";
    QString mode = "fixed";
    VrrTimingParameters controller;
    VrrReplayWorkerParameters worker;
    QList<VrrReplayAssertion> assertions;
};

struct VrrReplayConfiguration {
    VrrTimingParameters commonController;
    VrrReplayWorkerParameters commonWorker;
    QList<VrrReplayScenario> scenarios;
};

QJsonObject vrrTimingParametersToJson(const VrrTimingParameters& value);
QJsonObject vrrWorkerParametersToJson(const VrrReplayWorkerParameters& value);
QJsonObject vrrDefaultReplayConfigurationJson();
QStringList vrrReplayParameterNames();

bool loadVrrReplayConfiguration(const QByteArray& json,
                                VrrReplayConfiguration& configuration,
                                QString& error);
bool applyVrrReplayOverride(const QString& expression,
                            VrrReplayScenario& scenario,
                            QString& error);
bool validateVrrTimingParameters(const VrrTimingParameters& value,
                                 QString& error);
bool validateVrrWorkerParameters(const VrrReplayWorkerParameters& value,
                                 QString& error);
