// SPDX-License-Identifier: GPL-3.0-or-later
#include <KPluginFactory>
#include <KQuickConfigModule>

// 系统设置内嵌的平板设置页。界面来自 QRC 中的 ui/main.qml；
// 开关与按钮直接写入 KScreen，不存在待保存的编辑，因此隐藏「应用」按钮。
class TabletConfigModule : public KQuickConfigModule
{
    Q_OBJECT

public:
    TabletConfigModule(QObject *parent, const KPluginMetaData &metaData)
        : KQuickConfigModule(parent, metaData)
    {
        setButtons(NoAdditionalButton);
    }
};

K_PLUGIN_CLASS_WITH_JSON(TabletConfigModule, "kcm_loa_tablet.json")

#include "kcm_loa_tablet.moc"
