// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * KWin 效果：按环境光色温调整屏幕白点。
 *
 * 数据来自会话 D-Bus 服务 org.loa.AmbientLight（loa-alsd），效果本身不
 * 直接访问传感器。色温变化按 120 ms 步进平滑过渡，配置项位于 kwinrc
 * 的 [Effect-ambientcolor] 组。
 */

#pragma once

#include <QDBusServiceWatcher>
#include <QMatrix3x3>
#include <QTimer>
#include <memory>
#include <unordered_set>

#include "effect/offscreeneffect.h"
#include "opengl/glshadermanager.h"

namespace KWin
{

class AmbientColorEffect : public OffscreenEffect
{
    Q_OBJECT

public:
    explicit AmbientColorEffect();
    ~AmbientColorEffect() override;

    bool isActive() const override;
    void reconfigure(ReconfigureFlags flags) override;
    int requestedEffectChainPosition() const override;
    bool drawWindow(const RenderTarget &renderTarget,
                    const RenderViewport &viewport,
                    EffectWindow *window,
                    int mask,
                    const Region &deviceRegion,
                    WindowPaintData &data) override;

    static bool supported();

private Q_SLOTS:
    void slotWindowAdded(KWin::EffectWindow *window);
    void slotWindowDeleted(KWin::EffectWindow *window);
    void slotServiceRegistered(const QString &serviceName);
    void slotServiceUnregistered(const QString &serviceName);
    void slotPropertiesChanged(const QString &interfaceName,
                               const QVariantMap &changedProperties,
                               const QStringList &invalidatedProperties);
    void stepTowardsTarget();

private:
    void loadShader();
    void querySensor();
    void applyTargetCct(double cct);
    void disableAmbient();
    void updateMatrix();
    void redirectAll();
    void unredirectAll();

    std::unique_ptr<GLShader> m_shader;
    std::unordered_set<EffectWindow *> m_windows;
    QDBusServiceWatcher *m_serviceWatcher = nullptr;

    QMatrix3x3 m_matrix;
    QTimer m_animation;
    double m_targetCct = 0.0;
    double m_currentCct = 0.0;
    bool m_valid = false;

    // 配置：强度百分比，以及参与校正的环境色温范围（K）。
    int m_strength = 100;
    int m_minCct = 3000;
    int m_maxCct = 6500;
};

} // namespace KWin
