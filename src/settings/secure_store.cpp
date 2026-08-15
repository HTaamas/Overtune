#include "secure_store.h"

#include <QSettings>

// macOS lives in secure_store_mac.mm (Keychain); this file covers Windows
// (DPAPI) and the plaintext fallback everywhere else.
#ifndef __APPLE__

namespace {
constexpr auto kOrg = "Overtune";
constexpr auto kApp = "Overtune";

// Encrypted values are stored under a distinct key so a legacy plaintext value
// under the bare key stays recognisable (and migratable) after an upgrade.
QString encryptedKey(const QString &key) {
    return key + "_enc";
}
}

#ifdef _WIN32

#include <windows.h>
#include <dpapi.h>

bool SecureStore::isEncrypted() {
    return true;
}

bool SecureStore::save(const QString &key, const QString &value) {
    const QByteArray plain = value.toUtf8();

    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));
    in.cbData = DWORD(plain.size());

    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Overtune credential", nullptr, nullptr, nullptr, 0, &out)) {
        return false;
    }
    const QByteArray blob(reinterpret_cast<char *>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);

    QSettings settings(kOrg, kApp);
    settings.setValue(encryptedKey(key), blob.toBase64());
    settings.remove(key); // drop any legacy plaintext copy
    return true;
}

QString SecureStore::load(const QString &key) {
    QSettings settings(kOrg, kApp);

    const QByteArray stored = settings.value(encryptedKey(key)).toByteArray();
    if (!stored.isEmpty()) {
        QByteArray blob = QByteArray::fromBase64(stored);
        DATA_BLOB in;
        in.pbData = reinterpret_cast<BYTE *>(blob.data());
        in.cbData = DWORD(blob.size());

        DATA_BLOB out{};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
            // Ciphertext belongs to a different user/machine — treat as absent so
            // the caller re-authorizes rather than looping on a bad token.
            return QString();
        }
        const QString value = QString::fromUtf8(reinterpret_cast<char *>(out.pbData), int(out.cbData));
        LocalFree(out.pbData);
        return value;
    }

    // Migrate a plaintext value written by an older build.
    const QString legacy = settings.value(key).toString();
    if (!legacy.isEmpty()) {
        save(key, legacy);
    }
    return legacy;
}

void SecureStore::remove(const QString &key) {
    QSettings settings(kOrg, kApp);
    settings.remove(encryptedKey(key));
    settings.remove(key);
}

#else // !_WIN32 && !__APPLE__ — no OS keystore wired up

bool SecureStore::isEncrypted() {
    return false;
}

bool SecureStore::save(const QString &key, const QString &value) {
    QSettings settings(kOrg, kApp);
    settings.setValue(key, value);
    return true;
}

QString SecureStore::load(const QString &key) {
    QSettings settings(kOrg, kApp);
    return settings.value(key).toString();
}

void SecureStore::remove(const QString &key) {
    QSettings settings(kOrg, kApp);
    settings.remove(key);
}

#endif // _WIN32

#endif // !__APPLE__
