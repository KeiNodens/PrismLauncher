// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariant>

// Cross-source project references in <dataRoot>/prismlauncher-mcp/
// crossrefs.json: project slug -> the project's ids on Modrinth and on
// CurseForge. The table is written by prismlauncher-mcp's cross_check_mods
// tool and by the launcher's own CrossSourceResolveTask (which refreshes it
// for the open instance when a mod search page is created).
//
// The mod search pages use this to recognize a pack as installed when the
// copy inside the instance came from the OTHER provider (for example
// browsing CurseForge while the Modrinth build of the same mod is
// installed). When the file is absent or has no entry for a slug, behavior
// is unchanged.
namespace CrossSourceRefs {

// The slug's project id on one provider, in that provider's native shape
// (string for Modrinth, integer for CurseForge). Invalid QVariant when the
// slug is not recorded.
QVariant projectId(const QString& slug, bool modrinth);

// The full recorded pairs object (slug -> {"modrinth": ..., "curseforge": ...}).
QJsonObject allPairs();

// Merge new pairs into the table and persist it. Existing entries are
// overwritten by the new observations.
void mergePairs(const QJsonObject& newPairs);

}  // namespace CrossSourceRefs
