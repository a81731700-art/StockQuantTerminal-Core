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
#include <QUrlQuery>

namespace fincept::services {
namespace {
constexpr const char* kTopic = "econ:stockquant:upcoming_events";
constexpr const char* kCalendarBase = "https://www.financecalendar.com/wp-json/fc/v1/calendar";

int importance_from(const QJsonValue& v) {
    if (v.isDouble()) return v.toInt();
    const QString s = v.toString().trimmed().toLower();
    if (s == "high") return 3;
    if (s == "medium" || s == "med") return 2;
    if (s == "low") return 1;
    return 0;
}

QString country_code(const QJsonObject& src) {
    QString c = src.value("country").toString().trimmed();
    if (c.isEmpty()) c = src.value("currency").toString().trimmed();
    if (c.isEmpty()) {
        const QString n = src.value("name").toString().toUpper();
        if (n.startsWith("US ") || n.contains("FOMC") || n.contains("FED ")) c = "US";
        else if (n.startsWith("UK ") || n.contains("BANK OF ENGLAND")) c = "GB";
        else if (n.contains("ECB") || n.startsWith("EU ")) c = "EU";
        else if (n.contains("BOJ") || n.startsWith("JAPAN ")) c = "JP";
        else if (n.contains("PBOC") || n.startsWith("CHINA ")) c = "CN";
    }
    return c.left(3).toUpper();
}

QJsonArray source_array(const QJsonDocument& doc) {
    if (doc.isArray()) return doc.array();
    if (!doc.isObject()) return {};
    const auto root = doc.object();
    if (root.value("events").isArray()) return root.value("events").toArray();
    if (root.value("data").isArray()) return root.value("data").toArray();
    return {};
}

QJsonArray normalize_events(const QJsonDocument& doc) {
    struct Row { QDateTime dt; QJsonObject obj; };
    QVector<Row> rows;
    const auto now = QDateTime::currentDateTimeUtc().addSecs(-6 * 60 * 60);
    const auto horizon = now.addDays(14);

    for (const auto& value : source_array(doc)) {
        const auto src = value.toObject();
        QString name = src.value("name").toString().trimmed();
        if (name.isEmpty()) name = src.value("title").toString().trimmed();
        if (name.isEmpty()) name = src.value("event").toString().trimmed();
        if (name.isEmpty()) continue;

        QDateTime dt = QDateTime::fromString(src.value("time_utc").toString(), Qt::ISODate);
        if (!dt.isValid()) {
            const QString date = src.value("date").toString();
            const QString time = src.value("time").toString();
            dt = QDateTime::fromString(date + (time.isEmpty() ? "T00:00:00Z" : "T" + time + ":00Z"), Qt::ISODate);
        }
        if (!dt.isValid()) continue;
        dt = dt.toUTC();
        if (dt < now || dt > horizon) continue;

        QJsonObject e;
        e["event"] = name;
        e["category"] = src.value("category").toString();
        e["country"] = country_code(src);
        e["date"] = dt.date().toString("yyyy-MM-dd");
        e["time"] = src.value("all_day").toBool(false) ? QString() : dt.time().toString("HH:mm");
        e["reference_period"] = src.value("reference_period").toString();
        e["actual"] = src.value("actual").toVariant().toString();
        e["forecast"] = src.value("forecast").toVariant().toString();
        e["consensus"] = src.value("consensus").toVariant().toString();
        e["previous"] = src.value("prior").toVariant().toString();
        e["importance"] = importance_from(src.value("impact"));
        e["url"] = src.value("url").toString();
        e["source"] = "financecalendar.com";
        rows.push_back({dt, e});
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
    LOG_INFO("MacroCalendarService", "Registered StockQuant economic calendar (financecalendar.com)");
}

QStringList MacroCalendarService::topic_patterns() const { return {QString::fromLatin1(kTopic)}; }

void MacroCalendarService::refresh(const QStringList& topics) {
    if (!topics.contains(QString::fromLatin1(kTopic))) return;

    const QDate today = QDate::currentDate();
    QUrl url(QString::fromLatin1(kCalendarBase));
    QUrlQuery query;
    query.addQueryItem("from", today.toString("yyyy-MM-dd"));
    query.addQueryItem("to", today.addDays(14).toString("yyyy-MM-dd"));
    query.addQueryItem("limit", "100");
    url.setQuery(query);

    auto* nam = new QNetworkAccessManager(this);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("StockQuantTerminal/0.3.3"));
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(15000);
    auto* reply = nam->get(req);
    QPointer<MacroCalendarService> self = this;
    connect(reply, &QNetworkReply::finished, this, [self, reply, nam]() {
        const QByteArray payload = reply->readAll();
        const auto net_error = reply->error();
        const QString net_message = reply->errorString();
        reply->deleteLater();
        nam->deleteLater();
        if (!self) return;

        auto& hub = fincept::datahub::DataHub::instance();
        if (net_error != QNetworkReply::NoError) {
            LOG_WARN("MacroCalendarService", net_message);
            hub.publish_error(QString::fromLatin1(kTopic), net_message);
            return;
        }

        QJsonParseError err{};
        const auto doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError) {
            LOG_WARN("MacroCalendarService", QString("JSON parse error: %1").arg(err.errorString()));
            hub.publish_error(QString::fromLatin1(kTopic), err.errorString());
            return;
        }

        const auto events = normalize_events(doc);
        if (events.isEmpty()) {
            LOG_WARN("MacroCalendarService", QString("Calendar returned no usable upcoming events (%1 bytes)").arg(payload.size()));
            hub.publish_error(QString::fromLatin1(kTopic), QStringLiteral("Calendar returned no upcoming events"));
            return;
        }

        LOG_INFO("MacroCalendarService", QString("Published %1 upcoming events").arg(events.size()));
        hub.publish(QString::fromLatin1(kTopic), QVariant::fromValue(events));
    });
}
} // namespace fincept::services
