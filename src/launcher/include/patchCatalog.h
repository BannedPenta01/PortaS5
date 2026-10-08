#ifndef COMMUNITY_PATCH_CATALOG_H
#define COMMUNITY_PATCH_CATALOG_H

#include <QString>
#include <QStringList>

class QWidget;

namespace CommunityPatches {

// Raw file endpoints of the community patch repository.
QString IndexUrl();
QString FileUrl(const QString& repo_path);

// Repo-relative patch files listed for a title (e.g. "PPSA01325").
// Empty on any failure (offline, unknown title, bad index). Results are
// cached on disk for 24h so repeated launches stay fast and offline-safe.
QStringList FilesForTitle(const QString& title_id);

// Downloads the title's files and merges their mods into the local
// _Patches/<TITLE>.json consumed by the emulator. Prefers files matching
// game_version. Returns an empty string on success, else an error message.
// Records the title as downloaded so the offer is never shown again.
QString DownloadToLocal(const QString& title_id, const QString& game_version);

// Offer bookkeeping (persisted): never ask again once downloaded or dismissed.
bool Downloaded(const QString& title_id);
bool Dismissed(const QString& title_id);
void SetDismissed(const QString& title_id, bool dismissed);

// True when a local patch file with at least one enabled mod exists.
bool LocalHasEnabledMods(const QString& title_id);

// Full Run-time flow for MainDialog: checks the catalog and, when community
// patches exist that the user doesn't have yet, offers to download them.
// Always returns; the game boots regardless of the choice.
void OfferOnRun(QWidget* parent, const QString& title_id, const QString& game_version);

} // namespace CommunityPatches

#endif // COMMUNITY_PATCH_CATALOG_H
