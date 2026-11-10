// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * KWin 效果：按环境光色温调整屏幕白点。
 *
 * 白点适配在 CIE XYZ 空间用 Bradford 方法计算：把显示器的 D65 白点
 * 适配到环境光对应的黑体色温，再转换回线性 sRGB。强度按百分比与单位
 * 变换混合，环境色温限制在 MinimumCct..MaximumCct 之间。
 */

#include "ambientcolor.h"

#include "effect/effecthandler.h"
#include "opengl/glshader.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QLoggingCategory>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(KWIN_AMBIENT_COLOR, "kwin_effect_ambientcolor", QtInfoMsg)

static void ensureResources()
{
    // AUTORCC 以 qrc 文件名生成初始化函数，文件为 resources.qrc。
    Q_INIT_RESOURCE(resources);
}

namespace KWin
{

namespace
{

constexpr double kD65X = 0.95047;
constexpr double kD65Y = 1.0;
constexpr double kD65Z = 1.08883;

struct Mat3 {
    double m[3][3];

    static Mat3 identity()
    {
        return Mat3{{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}};
    }

    double &operator()(int row, int col) { return m[row][col]; }
    double operator()(int row, int col) const { return m[row][col]; }
};

Mat3 multiply(const Mat3 &a, const Mat3 &b)
{
    Mat3 result{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            result(row, col) = a(row, 0) * b(0, col) + a(row, 1) * b(1, col) + a(row, 2) * b(2, col);
        }
    }
    return result;
}

Mat3 blendWithIdentity(const Mat3 &matrix, double strength)
{
    Mat3 result = Mat3::identity();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            result(row, col) = (1.0 - strength) * result(row, col) + strength * matrix(row, col);
        }
    }
    return result;
}

QMatrix3x3 toQMatrix3x3(const Mat3 &matrix)
{
    QMatrix3x3 result;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            result(row, col) = static_cast<float>(matrix(row, col));
        }
    }
    return result;
}

// CIE 1931 黑体轨迹近似，1667 至 25000 K 有效。
void cctToChromaticity(double cct, double &x, double &y)
{
    if (cct <= 4000.0) {
        x = -0.2661239e9 / (cct * cct * cct) - 0.2343589e6 / (cct * cct) + 0.8776956e3 / cct + 0.179910;
    } else {
        x = -3.0258469e9 / (cct * cct * cct) + 2.1070379e6 / (cct * cct) + 0.2226347e3 / cct + 0.240390;
    }

    if (cct <= 2222.0) {
        y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
    } else if (cct <= 4000.0) {
        y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    }
}

Mat3 bradfordAdaptation(double cct)
{
    const Mat3 bradford{{
        {0.8951, 0.2664, -0.1614},
        {-0.7502, 1.7135, 0.0367},
        {0.0389, -0.0685, 1.0296},
    }};
    const Mat3 bradfordInverse{{
        {0.9869929, -0.1470543, 0.1599627},
        {0.4323053, 0.5183603, 0.0492912},
        {-0.0085287, 0.0400428, 0.9684867},
    }};
    const Mat3 xyzToRgb{{
        {3.2404542, -1.5371385, -0.4985314},
        {-0.9692660, 1.8760108, 0.0415560},
        {0.0556434, -0.2040259, 1.0572252},
    }};
    const Mat3 rgbToXyz{{
        {0.4124564, 0.3575761, 0.1804375},
        {0.2126729, 0.7151522, 0.0721750},
        {0.0193339, 0.1191920, 0.9503041},
    }};

    double x = 0.0;
    double y = 0.0;
    cctToChromaticity(cct, x, y);
    if (y <= 0.0) {
        return Mat3::identity();
    }

    const double ambientXyz[3] = {x / y, 1.0, (1.0 - x - y) / y};
    const double d65Xyz[3] = {kD65X, kD65Y, kD65Z};

    double ambientLms[3];
    double d65Lms[3];
    for (int row = 0; row < 3; ++row) {
        ambientLms[row] = bradford(row, 0) * ambientXyz[0]
            + bradford(row, 1) * ambientXyz[1]
            + bradford(row, 2) * ambientXyz[2];
        d65Lms[row] = bradford(row, 0) * d65Xyz[0]
            + bradford(row, 1) * d65Xyz[1]
            + bradford(row, 2) * d65Xyz[2];
    }

    Mat3 scale = Mat3::identity();
    for (int i = 0; i < 3; ++i) {
        scale(i, i) = ambientLms[i] / d65Lms[i];
    }

    const Mat3 adaptationXyz = multiply(multiply(bradfordInverse, scale), bradford);
    return multiply(multiply(xyzToRgb, adaptationXyz), rgbToXyz);
}

QVariant unwrapVariant(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QDBusVariant>()) {
        return value.value<QDBusVariant>().variant();
    }
    return value;
}

} // namespace

AmbientColorEffect::AmbientColorEffect()
    : OffscreenEffect()
{
    ensureResources();

    reconfigure(Effect::ReconfigureAll);

    m_animation.setInterval(120);
    connect(&m_animation, &QTimer::timeout, this, &AmbientColorEffect::stepTowardsTarget);

    connect(effects, &EffectsHandler::windowAdded, this, &AmbientColorEffect::slotWindowAdded);
    connect(effects, &EffectsHandler::windowDeleted, this, &AmbientColorEffect::slotWindowDeleted);

    m_serviceWatcher = new QDBusServiceWatcher(QStringLiteral("org.loa.AmbientLight"),
                                               QDBusConnection::sessionBus(),
                                               QDBusServiceWatcher::WatchForRegistration
                                                   | QDBusServiceWatcher::WatchForUnregistration,
                                               this);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceRegistered,
            this, &AmbientColorEffect::slotServiceRegistered);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered,
            this, &AmbientColorEffect::slotServiceUnregistered);

    QDBusConnection::sessionBus().connect(QString(),
                                          QStringLiteral("/org/loa/AmbientLight"),
                                          QStringLiteral("org.freedesktop.DBus.Properties"),
                                          QStringLiteral("PropertiesChanged"),
                                          this,
                                          SLOT(slotPropertiesChanged(QString, QVariantMap, QStringList)));

    // 服务可能先于效果启动，构造完成后再查询一次，避免阻塞 KWin 启动。
    QTimer::singleShot(0, this, &AmbientColorEffect::querySensor);
}

AmbientColorEffect::~AmbientColorEffect() = default;

bool AmbientColorEffect::supported()
{
    return effects->isOpenGLCompositing();
}

int AmbientColorEffect::requestedEffectChainPosition() const
{
    return 99;
}

bool AmbientColorEffect::isActive() const
{
    return m_valid && m_strength > 0 && m_shader != nullptr;
}

void AmbientColorEffect::loadShader()
{
    m_shader = ShaderManager::instance()->generateShaderFromFile(
        ShaderTrait::MapTexture,
        QString(),
        QStringLiteral(":/effects/ambientcolor/shaders/ambientcolor.frag"));

    if (!m_shader) {
        qCCritical(KWIN_AMBIENT_COLOR) << "Failed to load the ambient color shader";
    }
}

void AmbientColorEffect::reconfigure(ReconfigureFlags flags)
{
    if (!(flags & Effect::ReconfigureAll)) {
        return;
    }

    const QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + QStringLiteral("/kwinrc");
    QSettings settings(configPath, QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Effect-ambientcolor"));
    m_strength = std::clamp(settings.value(QStringLiteral("Strength"), 100).toInt(), 0, 100);
    m_minCct = std::clamp(settings.value(QStringLiteral("MinimumCct"), 3000).toInt(), 2000, 8000);
    m_maxCct = std::clamp(settings.value(QStringLiteral("MaximumCct"), 6500).toInt(), 2000, 8000);
    settings.endGroup();

    if (m_minCct > m_maxCct) {
        std::swap(m_minCct, m_maxCct);
    }

    if (!m_shader) {
        loadShader();
    }

    if (m_valid) {
        if (m_strength > 0) {
            redirectAll();
        } else {
            unredirectAll();
        }
    }

    updateMatrix();
    effects->addRepaintFull();
}

void AmbientColorEffect::slotServiceRegistered(const QString &serviceName)
{
    Q_UNUSED(serviceName)
    querySensor();
}

void AmbientColorEffect::slotServiceUnregistered(const QString &serviceName)
{
    Q_UNUSED(serviceName)
    disableAmbient();
}

void AmbientColorEffect::disableAmbient()
{
    if (m_valid) {
        qCInfo(KWIN_AMBIENT_COLOR) << "Ambient color disabled";
    }

    m_valid = false;
    m_targetCct = 0.0;
    m_animation.stop();
    updateMatrix();
    unredirectAll();
    effects->addRepaintFull();
}

void AmbientColorEffect::querySensor()
{
    QDBusMessage request = QDBusMessage::createMethodCall(
        QStringLiteral("org.loa.AmbientLight"),
        QStringLiteral("/org/loa/AmbientLight"),
        QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("Get"));
    request << QStringLiteral("org.loa.AmbientLight1") << QStringLiteral("Cct");

    // 异步调用：服务存在但无响应时不阻塞合成器的主循环。
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(request, 500), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *watcher) {
                watcher->deleteLater();
                const QDBusPendingReply<QVariant> reply = *watcher;
                if (reply.isError()) {
                    return;
                }
                const double cct = unwrapVariant(reply.argumentAt<0>()).toDouble();
                if (cct > 0.0) {
                    applyTargetCct(cct);
                }
            });
}

void AmbientColorEffect::slotPropertiesChanged(const QString &interfaceName,
                                               const QVariantMap &changedProperties,
                                               const QStringList &invalidatedProperties)
{
    if (interfaceName != QStringLiteral("org.loa.AmbientLight1")) {
        return;
    }

    if (invalidatedProperties.contains(QStringLiteral("Cct"))) {
        disableAmbient();
        return;
    }

    const auto cctIt = changedProperties.constFind(QStringLiteral("Cct"));
    if (cctIt == changedProperties.constEnd()) {
        return;
    }

    const double cct = unwrapVariant(*cctIt).toDouble();
    if (cct > 0.0) {
        applyTargetCct(cct);
    } else {
        disableAmbient();
    }
}

void AmbientColorEffect::applyTargetCct(double cct)
{
    m_targetCct = std::clamp(cct, static_cast<double>(m_minCct), static_cast<double>(m_maxCct));

    if (!m_valid) {
        m_valid = true;
        m_currentCct = m_targetCct;
        updateMatrix();
        redirectAll();
        effects->addRepaintFull();
        qCInfo(KWIN_AMBIENT_COLOR) << "Ambient color enabled at" << m_targetCct << "K";
        return;
    }

    if (std::abs(m_targetCct - m_currentCct) > 1.0 && !m_animation.isActive()) {
        m_animation.start();
    }
}

void AmbientColorEffect::stepTowardsTarget()
{
    const double difference = m_targetCct - m_currentCct;
    if (std::abs(difference) < 1.0) {
        m_currentCct = m_targetCct;
        m_animation.stop();
    } else {
        m_currentCct += difference * 0.2;
    }

    updateMatrix();
    effects->addRepaintFull();
}

void AmbientColorEffect::updateMatrix()
{
    Mat3 matrix = Mat3::identity();

    if (m_valid && m_strength > 0 && m_currentCct > 0.0) {
        matrix = blendWithIdentity(bradfordAdaptation(m_currentCct), m_strength / 100.0);
    }

    m_matrix = toQMatrix3x3(matrix);
}

void AmbientColorEffect::redirectAll()
{
    if (!m_shader || m_strength <= 0) {
        return;
    }

    const auto windows = effects->stackingOrder();
    for (EffectWindow *window : windows) {
        if (m_windows.contains(window)) {
            continue;
        }
        redirect(window);
        setShader(window, m_shader.get());
        m_windows.insert(window);
    }
}

void AmbientColorEffect::unredirectAll()
{
    for (EffectWindow *window : m_windows) {
        unredirect(window);
    }
    m_windows.clear();
}

void AmbientColorEffect::slotWindowAdded(EffectWindow *window)
{
    if (!m_valid || m_strength <= 0 || !m_shader || m_windows.contains(window)) {
        return;
    }

    redirect(window);
    setShader(window, m_shader.get());
    m_windows.insert(window);
}

void AmbientColorEffect::slotWindowDeleted(EffectWindow *window)
{
    m_windows.erase(window);
}

bool AmbientColorEffect::drawWindow(const RenderTarget &renderTarget,
                                    const RenderViewport &viewport,
                                    EffectWindow *window,
                                    int mask,
                                    const Region &deviceRegion,
                                    WindowPaintData &data)
{
    if (m_shader) {
        ShaderBinder binder{m_shader.get()};
        m_shader->setUniform("adaptation", m_matrix);
    }

    return OffscreenEffect::drawWindow(renderTarget, viewport, window, mask, deviceRegion, data);
}

KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(AmbientColorEffect,
                                      "metadata.json",
                                      return AmbientColorEffect::supported();,
                                      return false;)

} // namespace KWin
