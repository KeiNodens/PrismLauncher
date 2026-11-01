// SPDX-FileCopyrightText: 2023 flowln <flowlnlnln@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-only

#include "ModModel.h"

#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/mod/ModFolderModel.h"
#include "modplatform/CrossSourceRefs.h"
#include "modplatform/CrossSourceResolveTask.h"

#include <QSet>
#include "modplatform/ModIndex.h"

#include <QMessageBox>
#include <algorithm>

namespace ResourceDownload {

ModModel::ModModel(BaseInstance& base_inst, ResourceAPI* api, QString debugName, QString metaEntryBase)
    : ResourceModel(api), m_base_instance(base_inst), m_debugName(debugName + " (Model)"), m_metaEntryBase(metaEntryBase)
{
    // Agent bridge addition: refresh the cross-source reference table for
    // this instance in the background (once per instance per session),
    // then repaint so cross-source installs show as installed.
    static QSet<QString> crossRefreshed;
    const QString key = m_base_instance.id();
    if (!crossRefreshed.contains(key)) {
        crossRefreshed.insert(key);
        auto task = new CrossSourceResolveTask(m_base_instance, this);
        connect(task, &Task::finished, this, [this] {
            if (rowCount(QModelIndex()) > 0)
                emit dataChanged(index(0), index(rowCount(QModelIndex()) - 1), {});
        });
        task->start();
    }
}

/******** Make data requests ********/

ResourceAPI::SearchArgs ModModel::createSearchArguments()
{
    auto profile = static_cast<MinecraftInstance const&>(m_base_instance).getPackProfile();

    Q_ASSERT(profile);
    Q_ASSERT(m_filter);

    std::optional<std::vector<Version>> versions{};
    std::optional<QStringList> categories{};
    auto loaders = profile->getSupportedModLoaders();

    // Version filter
    if (!m_filter->versions.empty())
        versions = m_filter->versions;
    if (m_filter->loaders)
        loaders = m_filter->loaders;
    if (!m_filter->categoryIds.empty())
        categories = m_filter->categoryIds;
    auto side = m_filter->side;

    auto sort = getCurrentSortingMethodByIndex();

    return {
        ModPlatform::ResourceType::Mod, m_next_search_offset, m_search_term, sort, loaders, versions, side, categories, m_filter->openSource
    };
}

ResourceAPI::VersionSearchArgs ModModel::createVersionsArguments(const QModelIndex& entry)
{
    auto pack = m_packs[entry.row()];
    auto profile = static_cast<MinecraftInstance const&>(m_base_instance).getPackProfile();

    Q_ASSERT(profile);
    Q_ASSERT(m_filter);

    std::optional<std::vector<Version>> versions{};
    auto loaders = profile->getSupportedModLoaders();
    if (!m_filter->versions.empty())
        versions = m_filter->versions;
    if (m_filter->loaders)
        loaders = m_filter->loaders;

    return { pack, versions, loaders, ModPlatform::ResourceType::Mod };
}

ResourceAPI::ProjectInfoArgs ModModel::createInfoArguments(const QModelIndex& entry)
{
    auto pack = m_packs[entry.row()];
    return { pack };
}

void ModModel::searchWithTerm(const QString& term, unsigned int sort, bool filter_changed)
{
    if (m_search_term == term && m_search_term.isNull() == term.isNull() && m_current_sort_index == sort && !filter_changed) {
        return;
    }

    setSearchTerm(term);
    m_current_sort_index = sort;

    refresh();
}

bool ModModel::isPackInstalled(ModPlatform::IndexedPack::Ptr pack) const
{
    auto allMods = static_cast<MinecraftInstance&>(m_base_instance).loaderModList()->allMods();
    auto matches = [](Mod* mod, ModPlatform::ResourceProvider provider, const QVariant& id) {
        if (auto meta = mod->metadata(); meta)
            return meta->provider == provider && meta->project_id == id;
        return false;
    };
    if (std::any_of(allMods.cbegin(), allMods.cend(),
                    [pack, &matches](Mod* mod) { return matches(mod, pack->provider, pack->addonId); }))
        return true;

    // The same project may be installed from the OTHER provider (browsing
    // CurseForge while the Modrinth copy is installed, or vice versa).
    // CrossSourceResolveTask and prismlauncher-mcp record slug <-> id pairs
    // in crossrefs.json; consult it before answering "not installed".
    const bool otherIsModrinth = pack->provider == ModPlatform::ResourceProvider::FLAME;
    const auto otherId = CrossSourceRefs::projectId(pack->slug, otherIsModrinth);
    if (!otherId.isValid())
        return false;
    const auto otherProvider = otherIsModrinth ? ModPlatform::ResourceProvider::MODRINTH : ModPlatform::ResourceProvider::FLAME;
    return std::any_of(allMods.cbegin(), allMods.cend(),
                       [&matches, otherProvider, &otherId](Mod* mod) { return matches(mod, otherProvider, otherId); });
}

QString ModModel::getCrossInstalledVersion(ModPlatform::IndexedPack::Ptr pack) const
{
    const bool otherIsModrinth = pack->provider == ModPlatform::ResourceProvider::FLAME;
    const auto otherId = CrossSourceRefs::projectId(pack->slug, otherIsModrinth);
    if (!otherId.isValid())
        return {};
    const auto otherProvider = otherIsModrinth ? ModPlatform::ResourceProvider::MODRINTH : ModPlatform::ResourceProvider::FLAME;
    auto allMods = static_cast<MinecraftInstance&>(m_base_instance).loaderModList()->allMods();
    for (auto mod : allMods) {
        if (auto meta = mod->metadata(); meta && meta->provider == otherProvider && meta->project_id == otherId)
            return meta->version_number;
    }
    return {};
}

QVariant ModModel::getInstalledPackVersion(ModPlatform::IndexedPack::Ptr pack) const
{
    auto allMods = static_cast<MinecraftInstance&>(m_base_instance).loaderModList()->allMods();
    for (auto mod : allMods) {
        if (auto meta = mod->metadata(); meta && meta->provider == pack->provider && meta->project_id == pack->addonId) {
            return meta->version();
        }
    }
    return {};
}

bool checkSide(ModPlatform::Side filter, ModPlatform::Side value)
{
    return (filter != ModPlatform::Side::ClientSide && filter != ModPlatform::Side::ServerSide) ||
           (value != ModPlatform::Side::ClientSide && value != ModPlatform::Side::ServerSide) || filter == value;
}

bool ModModel::checkFilters(ModPlatform::IndexedPack::Ptr pack)
{
    if (!m_filter)
        return true;
    return !(m_filter->hideInstalled && isPackInstalled(pack)) && checkSide(m_filter->side, pack->side);
}

bool ModModel::checkVersionFilters(const ModPlatform::IndexedVersion& v)
{
    if (!m_filter)
        return true;
    auto loaders = static_cast<MinecraftInstance&>(m_base_instance).getPackProfile()->getSupportedModLoaders();
    if (m_filter->loaders)
        loaders = m_filter->loaders;
    return (!optedOut(v) &&                                                         // is opted out(aka curseforge download link)
            (!loaders.has_value() || !v.loaders || loaders.value() & v.loaders) &&  // loaders
            checkSide(m_filter->side, v.side) &&                                    // side
            (m_filter->releases.empty() ||                                          // releases
             std::find(m_filter->releases.cbegin(), m_filter->releases.cend(), v.version_type) != m_filter->releases.cend()) &&
            m_filter->checkMcVersions(v.mcVersion));  // mcVersions
}

}  // namespace ResourceDownload
