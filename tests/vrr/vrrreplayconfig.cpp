#include "vrrreplayconfig.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>

#include <cmath>
#include <limits>

namespace {

template<typename T>
bool readUnsigned(const QJsonValue& json, const QString& name, T& value,
                  QString& error)
{
    if (!json.isDouble() || !std::isfinite(json.toDouble()) ||
            json.toDouble() < 0 || std::floor(json.toDouble()) != json.toDouble() ||
            json.toDouble() > std::min(
                9007199254740991.0,
                static_cast<double>(std::numeric_limits<T>::max()))) {
        error = name + " must be a non-negative integer in range";
        return false;
    }
    value = static_cast<T>(json.toDouble());
    return true;
}

bool applyControllerObject(const QJsonObject& object,
                           VrrTimingParameters& value, QString& error)
{
    QSet<QString> known;
#define APPLY_FIELD(type, jsonName, memberName, defaultValue) \
    known.insert(QStringLiteral(#jsonName)); \
    if (object.contains(QStringLiteral(#jsonName)) && \
            !readUnsigned(object.value(QStringLiteral(#jsonName)), \
                          QStringLiteral("controller.") + \
                          QStringLiteral(#jsonName), \
                          value.memberName, error)) return false;
    VRR_TIMING_PARAMETER_FIELDS(APPLY_FIELD)
#undef APPLY_FIELD
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!known.contains(it.key())) {
            error = "unknown controller parameter: " + it.key();
            return false;
        }
    }
    return validateVrrTimingParameters(value, error);
}

bool applyWorkerObject(const QJsonObject& object,
                       VrrReplayWorkerParameters& value, QString& error)
{
    const QSet<QString> known { "queue_capacity" };
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!known.contains(it.key())) {
            error = "unknown worker parameter: " + it.key();
            return false;
        }
    }
    return (!object.contains("queue_capacity") ||
            readUnsigned(object.value("queue_capacity"),
                         "worker.queue_capacity", value.queueCapacity,
                         error)) && validateVrrWorkerParameters(value, error);
}

bool applyParametersObject(const QJsonObject& object,
                           VrrTimingParameters& controller,
                           VrrReplayWorkerParameters& worker, QString& error)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.key() != "controller" && it.key() != "worker") {
            error = "unknown parameter section: " + it.key();
            return false;
        }
        if (!it.value().isObject()) {
            error = it.key() + " parameter section must be an object";
            return false;
        }
    }
    return (!object.contains("controller") ||
            applyControllerObject(object.value("controller").toObject(),
                                  controller, error)) &&
        (!object.contains("worker") ||
         applyWorkerObject(object.value("worker").toObject(), worker, error));
}

} // namespace

QJsonObject vrrTimingParametersToJson(const VrrTimingParameters& value)
{
    QJsonObject object;
#define WRITE_FIELD(type, jsonName, memberName, defaultValue) \
    object[QStringLiteral(#jsonName)] = static_cast<double>(value.memberName);
    VRR_TIMING_PARAMETER_FIELDS(WRITE_FIELD)
#undef WRITE_FIELD
    return object;
}

QJsonObject vrrWorkerParametersToJson(const VrrReplayWorkerParameters& value)
{
    QJsonObject object;
    object["queue_capacity"] = static_cast<double>(value.queueCapacity);
    return object;
}

QJsonObject vrrDefaultReplayConfigurationJson()
{
    QJsonObject parameters;
    parameters["controller"] = vrrTimingParametersToJson({});
    parameters["worker"] = vrrWorkerParametersToJson({});
    QJsonObject scenario;
    scenario["name"] = "candidate";
    scenario["mode"] = "fixed";
    QJsonObject root;
    root["config_schema"] = 1;
    root["parameters"] = parameters;
    root["scenarios"] = QJsonArray { scenario };
    return root;
}

QStringList vrrReplayParameterNames()
{
    QStringList names;
#define ADD_NAME(type, jsonName, memberName, defaultValue) \
    names.append("controller." #jsonName);
    VRR_TIMING_PARAMETER_FIELDS(ADD_NAME)
#undef ADD_NAME
    names << "worker.queue_capacity";
    return names;
}

bool validateVrrTimingParameters(const VrrTimingParameters& value,
                                 QString& error)
{
    const auto fail = [&error](const char* text) { error = text; return false; };
    if (value.baseGuardDivisor == 0 ||
            value.majorCadenceRatioDenominator == 0 ||
            value.candidateCadenceRatioDenominator == 0 ||
            value.readinessAttackDenominator == 0 ||
            value.readinessReleaseDenominator == 0 ||
            value.usableHeadroomDenominator == 0) {
        return fail("parameter denominators must be non-zero");
    }
    if (value.renderLeadFloorUs > value.renderLeadCeilingUs ||
            value.minimumGuardUs > value.maximumBaseGuardUs ||
            value.maximumBaseGuardUs > value.maximumAdaptiveGuardUs ||
            value.minimumReadinessReserveUs > value.readinessCeilingUs ||
            value.latchedPresentationHeadroomUs >
                value.latchedPresentationExitHeadroomUs ||
            value.looseCadenceWindowUs > value.tightCadenceWindowUs) {
        return fail("parameter floors, ceilings, or hysteresis are inverted");
    }
    if (value.guardDecayFrames == 0 || value.schedulerLearningSamples == 0 ||
            value.readinessLearningSamples == 0 ||
            value.preparationLearningSamples == 0 ||
            value.minimumReadinessSamples == 0 ||
            value.minimumCadenceSamples < 2 ||
            value.maximumCadenceSamples < value.minimumCadenceSamples ||
            value.rateCandidateSamples < 2 || value.phaseErrorFrames == 0) {
        return fail("sample counts and frame thresholds must be consistent and non-zero");
    }
    const unsigned int percents[] = {
        value.materialRateChangePercent, value.preparationPercentile,
        value.schedulerPercentile, value.readinessLowPercentile,
        value.readinessTightPercentile, value.readinessLoosePercentile,
    };
    for (unsigned int percent : percents) {
        if (percent > 100) return fail("percentiles and percentages must be in 0..100");
    }
    if (value.readinessLowPercentile > value.readinessLoosePercentile ||
            value.readinessLoosePercentile > value.readinessTightPercentile) {
        return fail("readiness percentiles must be low <= loose <= tight");
    }
    return true;
}

bool validateVrrWorkerParameters(const VrrReplayWorkerParameters& value,
                                 QString& error)
{
    if (value.queueCapacity == 0) {
        error = "worker parameters must be non-zero";
        return false;
    }
    return true;
}

bool loadVrrReplayConfiguration(const QByteArray& json,
                                VrrReplayConfiguration& configuration,
                                QString& error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (!document.isObject()) {
        error = "invalid replay configuration: " + parseError.errorString();
        return false;
    }
    const QJsonObject root = document.object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (it.key() != "config_schema" && it.key() != "parameters" &&
                it.key() != "scenarios") {
            error = "unknown replay configuration key: " + it.key();
            return false;
        }
    }
    if (root.value("config_schema").toInt(-1) != 1) {
        error = "unsupported replay config_schema (expected 1)";
        return false;
    }
    configuration = VrrReplayConfiguration {};
    if (root.contains("parameters")) {
        if (!root.value("parameters").isObject() ||
                !applyParametersObject(root.value("parameters").toObject(),
                                       configuration.commonController,
                                       configuration.commonWorker, error)) {
            return false;
        }
    }
    const QJsonValue scenariosValue = root.value("scenarios");
    if (!scenariosValue.isArray() || scenariosValue.toArray().isEmpty()) {
        error = "scenarios must be a non-empty array";
        return false;
    }
    QSet<QString> names;
    for (const QJsonValue& item : scenariosValue.toArray()) {
        if (!item.isObject()) { error = "each scenario must be an object"; return false; }
        const QJsonObject object = item.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.key() != "name" && it.key() != "mode" &&
                    it.key() != "parameters" && it.key() != "assertions") {
                error = "unknown scenario key: " + it.key(); return false;
            }
        }
        VrrReplayScenario scenario;
        scenario.controller = configuration.commonController;
        scenario.worker = configuration.commonWorker;
        scenario.name = object.value("name").toString();
        scenario.mode = object.value("mode").toString("fixed");
        if (scenario.name.isEmpty() || names.contains(scenario.name)) {
            error = "scenario names must be non-empty and unique"; return false;
        }
        names.insert(scenario.name);
        if (scenario.mode != "fixed" && scenario.mode != "worker") {
            error = "scenario mode must be fixed or worker"; return false;
        }
        if (object.contains("parameters") &&
                (!object.value("parameters").isObject() ||
                 !applyParametersObject(object.value("parameters").toObject(),
                                        scenario.controller, scenario.worker,
                                        error))) return false;
        if (object.contains("assertions")) {
            if (!object.value("assertions").isArray()) {
                error = "scenario assertions must be an array"; return false;
            }
            for (const QJsonValue& assertionValue :
                 object.value("assertions").toArray()) {
                const QJsonObject assertionObject = assertionValue.toObject();
                VrrReplayAssertion assertion;
                assertion.metric = assertionObject.value("metric").toString();
                assertion.operation = assertionObject.value("operator").toString();
                assertion.value = assertionObject.value("value").toDouble(
                    std::numeric_limits<double>::quiet_NaN());
                if (assertion.metric.isEmpty() ||
                        !QStringList { "<", "<=", "==", ">=", ">" }
                            .contains(assertion.operation) ||
                        !std::isfinite(assertion.value)) {
                    error = "invalid scenario assertion"; return false;
                }
                scenario.assertions.append(assertion);
            }
        }
        configuration.scenarios.append(scenario);
    }
    return true;
}

bool applyVrrReplayOverride(const QString& expression,
                            VrrReplayScenario& scenario, QString& error)
{
    const int equals = expression.indexOf('=');
    if (equals <= 0 || equals == expression.size() - 1) {
        error = "override must be section.name=value: " + expression;
        return false;
    }
    const QString path = expression.left(equals);
    bool ok = false;
    const qulonglong number = expression.mid(equals + 1).toULongLong(&ok);
    if (!ok || number > 9007199254740991ULL) {
        error = "override value must be an exact unsigned JSON integer: " + expression;
        return false;
    }
    QJsonObject section;
    section[path.section('.', 1, 1)] = static_cast<double>(number);
    if (path.startsWith("controller.")) {
        return applyControllerObject(section, scenario.controller, error);
    }
    if (path.startsWith("worker.")) {
        return applyWorkerObject(section, scenario.worker, error);
    }
    error = "override must start with controller. or worker.: " + expression;
    return false;
}
