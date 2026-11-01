// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

#include "net/NetJob.h"
#include "tasks/Task.h"

class BaseInstance;

// Refreshes the cross-source reference table (crossrefs.json) for one
// instance's installed mods: for every mod recorded in the packwiz index
// whose slug has no entry for the OTHER provider yet, look that provider's
// project id up by slug (Modrinth project endpoint / CurseForge slug
// search) and merge the pair into the table. Started by ModModel when a
// mod search page is created, so the search's installed markers are fresh
// for the instance being browsed. Best-effort: failures just leave the
// table as it was.
class CrossSourceResolveTask : public Task {
    Q_OBJECT
   public:
    explicit CrossSourceResolveTask(BaseInstance& instance, QObject* parent = nullptr);

   protected:
    void executeTask() override;

   private:
    struct Lookup {
        QString slug;
        bool wantModrinth;
        QString ownId;
    };

    void startNext();
    void finishAll();

    QList<Lookup> m_pending;
    QJsonObject m_found;
    NetJob::Ptr m_currentJob;
    QByteArray* m_sink = nullptr;
    QString m_indexDirPath;
    int m_attempted = 0;
};
