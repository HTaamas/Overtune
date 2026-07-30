#ifndef UI_THEME_H
#define UI_THEME_H

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QString>
#include <QStringList>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Nocturne — the design system every visible surface is built on. A dark,
// low-chroma blue-grey ground with a single blurple accent used only as a line,
// a small mark, a glow, or a ≤14%-alpha tint. Never pure black or pure white.
//
// Every literal here comes from the design system's token sheet
// (_ds/nocturne-…/styles.css). The (user-themeable) OverlaySettings defaults
// point at these so the "custom overlay colours" feature keeps working while
// shipping the Nocturne look out of the box.
namespace theme {

// --- Ground / surface ---
constexpr auto kBg = "#161826";        // overlay + dialog background (--color-bg)
constexpr auto kSurface = "#232532";   // sidebar, menu ground, chips (--color-surface)
constexpr auto kText = "#e9e9ed";      // titles (--color-text)

// --- Accent ramp (blurple) ---
constexpr auto kAccent = "#9184d9";    // volume fill, liked heart, sparkle, focus ring
constexpr auto kAccent200 = "#e7e5fe"; // Indigo preset accent
constexpr auto kAccent300 = "#d2cefd"; // accent text at body size (contrast)
constexpr auto kAccent400 = "#b5abfc"; // hover accent text, play glyph, Lifted accent
constexpr auto kAccent700 = "#5d5294"; // art-placeholder glyph on a neutral block
constexpr auto kAccent800 = "#423a6a"; // art-placeholder glyph on queue rows
constexpr auto kAccent900 = "#2b2741"; // "Locked" chip ground

// --- Neutral ramp ---
constexpr auto kNeutral300 = "#cfd3e5"; // song-progress fill
constexpr auto kNeutral400 = "#b2b6ca"; // volume percentage text, Ink accent
constexpr auto kNeutral500 = "#9397ab"; // artist / secondary text
constexpr auto kNeutral600 = "#75798c"; // time, index numbers, muted labels
constexpr auto kNeutral700 = "#595d6c"; // disabled numbers, overlay edge hairline
constexpr auto kNeutral800 = "#3f424d"; // rail troughs, art-placeholder ground
constexpr auto kNeutral900 = "#292b31"; // empty-state art ground

// --- Lines / tints (as Qt rgba() stylesheet strings) ---
constexpr auto kDivider = "rgba(233,233,237,0.16)";   // 1px rules, chip borders
constexpr auto kHoverTint = "rgba(233,233,237,0.06)";  // queue-row / menu-item hover
constexpr auto kNavHoverTint = "rgba(233,233,237,0.07)"; // sidebar / button hover

// --- Radius (px) ---
constexpr int kRadiusSm = 4;   // swatches
constexpr int kRadiusMd = 8;   // art, rows, buttons, inputs
constexpr int kRadiusLg = 14;  // overlay + dialog windows

// --- Spacing scale (density 0.7x), rounded to whole px ---
constexpr int kSpace1 = 3;
constexpr int kSpace2 = 6;
constexpr int kSpace3 = 8;
constexpr int kSpace4 = 11;
constexpr int kSpace6 = 17;
constexpr int kSpace8 = 22;

// Honors the OS "reduce motion" / "show animations" accessibility setting.
// Cached once — the setting doesn't change within a session in practice.
inline bool reducedMotion() {
    static const bool reduced = []() {
#ifdef _WIN32
        BOOL animEnabled = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animEnabled, 0)) {
            return !animEnabled;
        }
#endif
        return false;
    }();
    return reduced;
}

// Collapses an animation duration to 0 (instant) when reduced motion is on.
inline int animMs(int ms) { return reducedMotion() ? 0 : ms; }

// Loads the bundled Inter family (400 + 500) from the Qt resource system and
// returns the resolved family name, or an empty string if it couldn't be
// loaded (callers then fall back to the platform UI font). Weight is chosen
// per-widget via QFont::setWeight; Inter ships 400 and 500 only — bold is not
// used, hierarchy is size and space.
inline QString loadInterFamily() {
    static const QString family = []() -> QString {
        const QStringList files = {
            QStringLiteral(":/fonts/Inter-Regular.ttf"),
            QStringLiteral(":/fonts/Inter-Medium.ttf"),
        };
        QString resolved;
        for (const QString &file : files) {
            const int id = QFontDatabase::addApplicationFont(file);
            if (id < 0) {
                continue;
            }
            const QStringList families = QFontDatabase::applicationFontFamilies(id);
            if (!families.isEmpty()) {
                resolved = families.first();
            }
        }
        return resolved;
    }();
    return family;
}

// The UI font family: bundled Inter when available, else the platform default.
inline QString fontFamily() {
    const QString inter = loadInterFamily();
    return inter.isEmpty() ? QString() : inter;
}

// A QFont at the given pixel size and weight, using Inter when bundled. Numeric
// labels (time, volume) pass tabular=true to keep figures from jittering as
// they tick (Qt >= 6.7 exposes the OpenType "tnum" feature).
inline QFont uiFont(int pixelSize, QFont::Weight weight = QFont::Normal, bool tabular = false) {
    QFont f;
    const QString family = fontFamily();
    if (!family.isEmpty()) {
        f.setFamily(family);
    }
    f.setPixelSize(pixelSize);
    f.setWeight(weight);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    if (tabular) {
        f.setFeature("tnum", 1);
    }
#else
    Q_UNUSED(tabular);
#endif
    return f;
}

// A named theme preset: a background + accent pairing over the shared Nocturne
// neutrals. The full list lives here so the settings grid and the persisted
// preset name agree on one source of truth.
struct Preset {
    const char *name;
    const char *background;
    const char *accent;
};

inline const QList<Preset> &presets() {
    static const QList<Preset> list = {
        {"Nocturne", kBg, kAccent},
        {"Ink", "#0d0f16", kNeutral400}, // no accent — neutral stands in
        {"Lifted", kSurface, kAccent400},
        {"Indigo", "#262a60", kAccent200},
    };
    return list;
}

} // namespace theme

#endif // UI_THEME_H
