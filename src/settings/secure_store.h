#ifndef SECURE_STORE_H
#define SECURE_STORE_H

#include <QString>

// At-rest storage for the few secrets this app holds (currently just the OAuth
// refresh token, which is a long-lived credential to the user's account).
//
// Windows encrypts with DPAPI, so the ciphertext is bound to the logged-in user
// account and is useless if lifted off the machine. macOS stores the value in
// the login Keychain. Nothing else has an OS-backed store wired up here, so on
// those platforms the value stays in QSettings as plaintext — isEncrypted()
// reports which of the two you're getting.
//
// Every backend transparently migrates a pre-existing plaintext QSettings value
// on first load, so upgrading users don't have to re-authorize.
namespace SecureStore {
bool save(const QString &key, const QString &value);
QString load(const QString &key);
void remove(const QString &key);

// False when the value is being kept in plaintext (no OS keystore on this
// platform, or the keystore call failed).
bool isEncrypted();
}

#endif // SECURE_STORE_H
