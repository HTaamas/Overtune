#include "secure_store.h"

#include <QSettings>

#import <Foundation/Foundation.h>
#import <Security/Security.h>

// Keychain-backed implementation of SecureStore. Values live as generic
// passwords under the "Overtune" service, keyed by the caller's key name.

namespace {
constexpr auto kOrg = "Overtune";
constexpr auto kApp = "Overtune";
// Keychain items are scoped by the bundle identifier, matching
// MACOSX_BUNDLE_GUI_IDENTIFIER in CMakeLists.txt.
NSString *const kService = @"dev.htaamas.overtune";

NSMutableDictionary *baseQuery(const QString &key) {
    NSMutableDictionary *query = [NSMutableDictionary dictionary];
    query[(__bridge id)kSecClass] = (__bridge id)kSecClassGenericPassword;
    query[(__bridge id)kSecAttrService] = kService;
    query[(__bridge id)kSecAttrAccount] = key.toNSString();
    return query;
}
}

bool SecureStore::isEncrypted() {
    return true;
}

bool SecureStore::save(const QString &key, const QString &value) {
    const QByteArray utf8 = value.toUtf8();
    NSData *data = [NSData dataWithBytes:utf8.constData() length:NSUInteger(utf8.size())];

    NSMutableDictionary *query = baseQuery(key);
    // Simplest correct upsert: drop any existing item, then add the new one.
    SecItemDelete((__bridge CFDictionaryRef)query);

    query[(__bridge id)kSecValueData] = data;
    query[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlock;

    const OSStatus status = SecItemAdd((__bridge CFDictionaryRef)query, NULL);
    if (status != errSecSuccess) {
        return false;
    }

    // Clear any plaintext copy an older build left behind.
    QSettings settings(kOrg, kApp);
    settings.remove(key);
    return true;
}

QString SecureStore::load(const QString &key) {
    NSMutableDictionary *query = baseQuery(key);
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;

    CFTypeRef result = NULL;
    const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
    if (status == errSecSuccess && result) {
        // This file is compiled without ARC, so __bridge_transfer wouldn't
        // actually transfer anything — the +1 reference SecItemCopyMatching
        // hands back has to be released by hand or it leaks.
        NSData *data = (NSData *)result;
        const QString value = QString::fromUtf8(static_cast<const char *>(data.bytes),
                                                int(data.length));
        CFRelease(result);
        if (!value.isEmpty()) {
            return value;
        }
    }

    // Migrate a plaintext value written by an older build.
    QSettings settings(kOrg, kApp);
    const QString legacy = settings.value(key).toString();
    if (!legacy.isEmpty()) {
        save(key, legacy);
    }
    return legacy;
}

void SecureStore::remove(const QString &key) {
    NSMutableDictionary *query = baseQuery(key);
    SecItemDelete((__bridge CFDictionaryRef)query);

    QSettings settings(kOrg, kApp);
    settings.remove(key);
}
