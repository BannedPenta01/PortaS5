#include "patchCatalog.h"


#include "patchesDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QTimer>

namespace CommunityPatches {

namespace {

constexpr char kRawBase[] = "https://raw.githubusercontent.com/BannedPenta01/ps5-game-patches/"
                            "main/";
constexpr char kIndexPath[]      = "index.json";
constexpr int  kRequestTimeoutMs = 8000;
constexpr int  kCacheTtlSeconds  = 24 * 60 * 60;

QString CachePath() {
	const auto plan = PatchesDialog::PatchPlanPath(QStringLiteral("PPSA00000"));
	if (plan.isEmpty()) {
		return {};
	}
	return QFileInfo(plan).absoluteDir().absoluteFilePath(
	    QStringLiteral(".community-index.cache.json"));
}

QSettings FlagSettings() {
	return QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("Kyty"),
	                 QStringLiteral("Kyty"));
}

QByteArray FetchBlocking(const QUrl& url, QString* error) {
	QNetworkAccessManager network;
	QNetworkRequest       request(url);
	request.setTransferTimeout(kRequestTimeoutMs);
	QEventLoop loop;
	QTimer     timer;
	timer.setSingleShot(true);
	QByteArray data;
	QString    failure;
	QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
		failure = QObject::tr("Request timed out.");
		loop.quit();
	});
	QObject::connect(&network, &QNetworkAccessManager::finished, &loop,
	                 [&](QNetworkReply* reply) {
		                 reply->deleteLater();
		                 if (reply->error() != QNetworkReply::NoError) {
			                 failure = reply->errorString();
		                 } else {
			                 data = reply->readAll();
		                 }
		                 loop.quit();
	                 });
	timer.start(kRequestTimeoutMs + 2000);
	QNetworkReply* reply = network.get(request);
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit, Qt::QueuedConnection);
	// NOTE: finished() is emitted by the manager too; the extra quit is harmless.
	loop.exec();
	if (timer.isActive()) {
		timer.stop();
	} else if (failure.isEmpty()) {
		failure = QObject::tr("Request timed out.");
	}
	if (error != nullptr) {
		*error = failure;
	}
	return failure.isEmpty() ? data : QByteArray();
}

QJsonObject ReadCache() {
	const auto path = CachePath();
	if (path.isEmpty()) {
		return {};
	}
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const auto age = QFileInfo(file).lastModified().secsTo(QDateTime::currentDateTimeUtc());
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (document.isNull() || !document.isObject() || age < 0 || age > kCacheTtlSeconds) {
		return {};
	}
	return document.object();
}

void WriteCache(const QJsonObject& index) {
	const auto path = CachePath();
	if (path.isEmpty() || index.isEmpty()) {
		return;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(index).toJson(QJsonDocument::Compact));
	}
}

QJsonObject LoadIndex() {
	if (const auto cached = ReadCache(); !cached.isEmpty()) {
		return cached;
	}
	QString    error;
	const auto data = FetchBlocking(QUrl(QString::fromLatin1(kRawBase) + QLatin1String(kIndexPath)),
	                                  &error);
	if (data.isEmpty()) {
		return {};
	}
	const auto document = QJsonDocument::fromJson(data);
	if (document.isNull() || !document.isObject()) {
		return {};
	}
	WriteCache(document.object());
	return document.object();
}

} // namespace

QString IndexUrl() {
	return QString::fromLatin1(kRawBase) + QLatin1String(kIndexPath);
}

QString FileUrl(const QString& repo_path) {
	return QString::fromLatin1(kRawBase) + repo_path;
}

QStringList FilesForTitle(const QString& title_id) {
	const auto key   = title_id.trimmed().toUpper();
	const auto index = LoadIndex();
	const auto files = index.value(key).toArray();
	QStringList paths;
	for (const auto& value: files) {
		if (value.isString() && !value.toString().isEmpty()) {
			paths.append(value.toString());
		}
	}
	return paths;
}

QString DownloadToLocal(const QString& title_id, const QString& game_version) {
	const auto title = title_id.trimmed().toUpper();
	const auto paths = FilesForTitle(title);
	if (paths.isEmpty()) {
		return QObject::tr("No community patches are listed for %1.").arg(title);
	}
	struct Entry {
		QJsonObject document;
	};
	QList<Entry> exact;
	QList<Entry> other;
	for (const auto& path: paths) {
		QString    error;
		const auto data = FetchBlocking(QUrl(FileUrl(path)), &error);
		if (data.isEmpty()) {
			return QObject::tr("Could not download %1: %2").arg(path, error);
		}
		QJsonDocument document = QJsonDocument::fromJson(data);
		if (!document.isObject() || !document.object().value(QStringLiteral("mods")).isArray()) {
			return QObject::tr("Invalid patch file %1.").arg(path);
		}
		const auto root = document.object();
		if (root.value(QStringLiteral("id")).toString().compare(title, Qt::CaseInsensitive) != 0) {
			return QObject::tr("Patch file %1 is for a different game.").arg(path);
		}
		if (!game_version.isEmpty() &&
		    root.value(QStringLiteral("version")).toString() == game_version) {
			exact.append({document.object()});
		} else {
			other.append({document.object()});
		}
	}
	const auto& chosen = !exact.isEmpty() ? exact : other;
	if (chosen.isEmpty()) {
		return QObject::tr("No community patches are listed for %1.").arg(title);
	}
	QJsonArray mods;
	for (const auto& entry: chosen) {
		for (const auto& mod: entry.document.value(QStringLiteral("mods")).toArray()) {
			mods.append(mod);
		}
	}
	if (mods.isEmpty()) {
		return QObject::tr("No community patches are listed for %1.").arg(title);
	}
	QJsonObject merged;
	merged.insert(QStringLiteral("id"), title);
	merged.insert(QStringLiteral("version"),
	              !game_version.isEmpty() ? game_version
	                                      : chosen.first().document.value(QStringLiteral("version")));
	merged.insert(QStringLiteral("process"),
	              chosen.first().document.value(QStringLiteral("process")));
	merged.insert(QStringLiteral("mods"), mods);
	const auto plan_path = PatchesDialog::PatchPlanPath(title);
	if (plan_path.isEmpty()) {
		return QObject::tr("Unsupported title ID.");
	}
	QDir().mkpath(QFileInfo(plan_path).absolutePath());
	QFile file(plan_path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return QObject::tr("Could not save patch file: %1").arg(file.errorString());
	}
	const auto bytes = QJsonDocument(merged).toJson();
	if (file.write(bytes) != bytes.size()) {
		return QObject::tr("Could not save patch file: %1").arg(file.errorString());
	}
	QSettings settings = FlagSettings();
	settings.beginGroup(QStringLiteral("CommunityPatches"));
	settings.setValue(title + QStringLiteral("/downloaded"), true);
	settings.endGroup();
	return {};
}

bool Downloaded(const QString& title_id) {
	QSettings settings = FlagSettings();
	settings.beginGroup(QStringLiteral("CommunityPatches"));
	return settings.value(title_id.trimmed().toUpper() + QStringLiteral("/downloaded"), false)
	    .toBool();
}

bool Dismissed(const QString& title_id) {
	QSettings settings = FlagSettings();
	settings.beginGroup(QStringLiteral("CommunityPatches"));
	return settings.value(title_id.trimmed().toUpper() + QStringLiteral("/dismissed"), false)
	    .toBool();
}

void SetDismissed(const QString& title_id, bool dismissed) {
	QSettings settings = FlagSettings();
	settings.beginGroup(QStringLiteral("CommunityPatches"));
	settings.setValue(title_id.trimmed().toUpper() + QStringLiteral("/dismissed"), dismissed);
	settings.endGroup();
}

bool LocalHasEnabledMods(const QString& title_id) {
	const auto title = title_id.trimmed().toUpper();
	const auto path  = PatchesDialog::PatchPlanPath(title);
	if (path.isEmpty()) {
		return false;
	}
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject() || !document.object().value(QStringLiteral("mods")).isArray()) {
		return false;
	}
	for (const auto& value: document.object().value(QStringLiteral("mods")).toArray()) {
		if (value.toObject().value(QStringLiteral("enabled")).toBool(true)) {
			return true;
		}
	}
	return false;
}

void OfferOnRun(QWidget* parent, const QString& title_id, const QString& game_version) {
	const auto title = title_id.trimmed().toUpper();
	if (!PatchesDialog::IsSupportedTitleId(title) || Downloaded(title) || Dismissed(title) ||
	    LocalHasEnabledMods(title)) {
		return;
	}
	if (FilesForTitle(title).isEmpty()) {
		return;
	}
	QMessageBox box(parent);
	box.setWindowTitle(QObject::tr("Performance patches available"));
	box.setIcon(QMessageBox::Information);
	box.setText(QObject::tr(
	    "This game has community performance patches.\n\n"
	    "They are strongly recommended on handhelds and lower-end PCs "
	    "(for example, lowering the internal resolution), and the game may "
	    "run poorly without them.\n\n"
	    "Download them now from the community patch repository?"));
	auto*      download = box.addButton(QObject::tr("Download patches"), QMessageBox::AcceptRole);
	box.addButton(QObject::tr("Play without patches"), QMessageBox::RejectRole);
	auto* hide = new QCheckBox(QObject::tr("Don't ask again for this game"), &box);
	box.setCheckBox(hide);
	box.setDefaultButton(download);
	box.exec();
	if (box.clickedButton() == download) {
		QApplication::setOverrideCursor(Qt::WaitCursor);
		const auto error = DownloadToLocal(title, game_version);
		QApplication::restoreOverrideCursor();
		if (error.isEmpty()) {
			QMessageBox::information(parent, QObject::tr("Patches"),
			                         QObject::tr("Patches successfully downloaded."));
		} else {
			QMessageBox::warning(parent, QObject::tr("Patches"),
			                     QObject::tr("Could not download patches:\n%1").arg(error));
		}
	} else if (hide->isChecked()) {
		SetDismissed(title, true);
	}
}

} // namespace CommunityPatches
