// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QVariant>

// Cross-source project references recorded by prismlauncher-mcp's
// cross_check_mods tool in <dataRoot>/prismlauncher-mcp/crossrefs.json:
// project slug -> the project's ids on Modrinth and on CurseForge.
//
// The mod search pages use this to recognize a pack as installed when the
// copy inside the instance came from the OTHER provider (for example
// browsing CurseForge while the Modrinth build of the same mod is
// installed). The file is a cache produced by an external tool; when it is
// absent or has no entry for a slug, behavior is unchanged.
namespace CrossSourceRefs {

// The slug's project id on one provider, in that provider's native shape
// (string for Modrinth, integer for CurseForge). Invalid QVariant when the
// slug is not recorded.
QVariant projectId(const QString& slug, bool modrinth);

}  // namespace CrossSourceRefs
