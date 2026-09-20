#include "services/economics/MacroCalendarService.h"

#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/TopicPolicy.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>

namespace fincept::services {
namespace {
constexpr const char* kTopic = "econ:stockquant:upcoming_events";
constexpr const char* kCalendarUrl = "https://api.tradingeconomics.com/calendar?c=guest%3Aguest&f=json";

QString country_code(QString country) {
    country = country.trimmed().toLower();
    if (country == "united states") return "US";
    if (country == "china") return "CN";
    if (country == "euro area") return "EU";
    if (country == "united kingdom") return "GB";
    if (country == "japan") return "JP";
    if (country == "canada") return "CA";
    if (country == "australia") return "AU";
    if (country == "germany") return "DE";
    if (country == "france") return "FR";
    return country.left(3).toUpper();
}

QJsonArray normalize_events(const QJsonDocument& doc) {
    if (!doc.isArray()) return {};
    struct Row { QDateTime dt; QJsonObject obj; };
    QVector<Row> rows;
    const auto now = QDateTime::currentDateTimeUtc().addSecs(-6 * 60 * 60);
    const auto horizon = now.addDays(14);
    for (const auto& value : doc.array()) {
        const auto src = value.toObject();
        QDateTime dt = QDateTime::fromString(src.value("Date").toString(), Qt::ISODate);
        if (!dt.isValid()) continue;
        if (dt.timeSpec() == Qt::LocalTime) dt = dt.toUTC();
        if (dt < now || dt > horizon) continue;
        QJsonObject e;
        e["event"] = src.value("Event").toString();
        e["category"] = src.value("Category").toString();
        e["country"] = country_code(src.value("Country").toString());
        e["date"] = dt.date().toString("yyyy-MM-dd");
        e["time"] = dt.time().toString("HH:mm");
        e["reference_period"] = src.value("Reference").toString();
        e["actual"] = src.value("Actual").toString();
        e["forecast"] = src.value("Forecast").toString();
        e["consensus"] = src.value("TEForecast").toString();
        e["previous"] = src.value("Previous").toString();
        e["revised_from"] = src.value("Revised").toString();
        e["symbol"] = src.value("Ticker").toString();
        e["importance"] = src.value("Importance").toInt();
        e["url"] = src.value("URL").toString();
        if (!e.value("event").toString().isEmpty()) rows.push_back({dt, e});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.dt < b.dt; });
    QJsonArray out;
    for (int i = 0; i < rows.size() && i < 25; ++i) out.append(rows[i].obj);
    return out;
}
}

MacroCalendarService& MacroCalendarService::instance() {
    static MacroCalendarService s;
    return s;
}

MacroCalendarService::MacroCalendarService(QObject* parent) : QObject(parent) {}

void MacroCalendarService::ensure_registered_with_hub() {
    if (hub_registered_) return;
    auto& hub = fincept::datahub::DataHub::instance();
    hub.register_producer(this);
    fincept::datahub::TopicPolicy policy;
    policy.ttl_ms = 5 * 60 * 1000;
    policy.min_interval_ms = 60 * 1000;
    policy.refresh_timeout_ms = 20 * 1000;
    hub.set_policy(QString::fromLatin1(kTopic), policy);
    hub_registered_ = true;
    LOG_INFO("MacroCalendarService", "Registered StockQuant independent economic calendar");
}

QStringList MacroCalendarService::topic_patterns() const { return {QString::fromLatin1(kTopic)}; }

void MacroCalendarService::refresh(const QStringList& topics) {
    if (!topics.contains(QString::fromLatin1(kTopic))) return;
    auto* nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(QString::fromLatin1(kCalendarUrl)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("StockQuantTerminal/0.3.2"));
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(15000);
    auto* reply = nam->get(req);
    QPointer<MacroCalendarService> self = this;
    connect(reply, &QNetworkReply::finished, this, [self, reply, nam]() {
        reply->deleteLater();
        nam->deleteLater();
        if (!self) return;
        auto& hub = fincept::datahub::DataHub::instance();
        if (reply->error() != QNetworkReply::NoError) {
            const QString msg = reply->errorString();
            LOG_WARN("MacroCalendarService", msg);
            hub.publish_error(QString::fromLatin1(kTopic), msg);
            return;
        }
        QJsonParseError err{};
        const auto doc = QJsonDocument::fromJson(reply->readAll(), &err);
        if (err.error != QJsonParseError::NoError) {
            hub.publish_error(QString::fromLatin1(kTopic), err.errorString());
            return;
        }
        const auto events = normalize_events(doc);
        if (events.isEmpty()) {
            hub.publish_error(QString::fromLatin1(kTopic), QStringLiteral("Calendar returned no upcoming events"));
            return;
        }
        hub.publish(QString::fromLatin1(kTopic), QVariant::fromValue(events));
    });
}
} // namespace fincept::services
