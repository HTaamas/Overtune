#ifndef SPOTIFY_CLIENT_INTERNAL_H
#define SPOTIFY_CLIENT_INTERNAL_H

// Shared innards of the SpotifyClient implementation. The class is split across
// several translation units (auth / dealer / queue / remote / collection) purely
// to keep each file navigable — this header carries the pieces they all need.
//
// Not part of the public interface: only spotify_client*.cpp should include it.

#include "spotify_client.h" // UpcomingTrack, for queueSignatureFor below

#include <QtGlobal>

// The connect-state protos define a PlayerState field named "signals", which
// collides with Qt's `signals` keyword macro. Neutralize the Qt keyword macros
// while the generated protobuf headers are included.
QT_WARNING_PUSH
#pragma push_macro("signals")
#pragma push_macro("slots")
#pragma push_macro("emit")
#undef signals
#undef slots
#undef emit
#include "spotify/connectstate/connect.pb.h"
#include "spotify/connectstate/player.pb.h"
#include "spotify/connectstate/devices/connect_devices.pb.h"
#include "spotify/clienttoken/http/v0/clienttoken.pb.h"
#include "spotify/clienttoken/data/v0/connectivity.pb.h"
#include "spotify/extendedmetadata/extended_metadata.pb.h"
#include "spotify/extendedmetadata/extension_kind.pb.h"
#include "spotify/extendedmetadata/entity_extension_data.pb.h"
#include "spotify/metadata/metadata.pb.h"
#include "spotify/collection/collection.pb.h"
#include "spotify/collection/collection_update.pb.h"
#pragma pop_macro("emit")
#pragma pop_macro("slots")
#pragma pop_macro("signals")
QT_WARNING_POP

// `const`/`constexpr` at namespace scope has internal linkage, so each including
// translation unit gets its own copy — no ODR problem, no definitions needed.
namespace spotify_client {

constexpr int kPendingVolumeGracePeriodMs = 2500;
constexpr int kMaxConnectVolume = 65535;

// Past this point in a track, "previous" restarts it instead of skipping back.
constexpr int kPrevRestartThresholdMs = 5000;

constexpr int kMaxQueueTracks = 30;

// Well-known Spotify desktop client id (librespot), required for the device flow.
const char kClientIdHex[] = "65b708073fc0480ea92a077233ca87bd";
const char kUserAgent[] = "Spotify/125700463 Win32_x86_64/0 (PC desktop)";
const char kClientVersion[] = "1.2.52.442.g01d2b6ec";
const char kDeviceName[] = "Overtune";

const char kDeviceAuthorizeUrl[] = "https://accounts.spotify.com/oauth2/device/authorize";
const char kAuthorizeUrl[] = "https://accounts.spotify.com/authorize";
const char kTokenUrl[] = "https://accounts.spotify.com/api/token";

// Whitelisted for Spotify's desktop client id — go-librespot uses the same
// port/path, and the auth-code token response includes the canonical
// username (the device flow's does not).
constexpr quint16 kOAuthRedirectPort = 36842;
const char kOAuthRedirectUri[] = "http://127.0.0.1:36842/login";
// How long the loopback redirect listener stays open waiting for the browser.
constexpr int kAuthWindowMs = 10 * 60 * 1000;
const char kClientTokenUrl[] = "https://clienttoken.spotify.com/v1/clienttoken";
const char kApresolveUrl[] = "https://apresolve.spotify.com/?type=dealer&type=spclient";

const char kDeviceFlowScopes[] =
    "app-remote-control,playlist-modify,playlist-modify-private,playlist-modify-public,"
    "playlist-read,playlist-read-collaborative,playlist-read-private,streaming,"
    "ugc-image-upload,user-follow-modify,user-follow-read,user-library-modify,"
    "user-library-read,user-modify,user-modify-playback-state,user-modify-private,"
    "user-personalized,user-read-birthdate,user-read-currently-playing,user-read-email,"
    "user-read-play-history,user-read-playback-position,user-read-playback-state,"
    "user-read-private,user-read-recently-played,user-top-read";

// Cheap value-identity for a rendered queue: if this is unchanged, nothing the
// Up Next window draws has changed, so the repaint can be skipped. Shared
// because both the queue builder and the Liked Songs sync re-stamp it.
inline QString queueSignatureFor(const QList<UpcomingTrack> &tracks) {
    QString signature;
    for (const UpcomingTrack &t : tracks) {
        signature += t.trackId + '|' + t.title + '|' + t.artist + '|' + t.artUrl +
                     '|' + (t.liked ? '1' : '0') + (t.smartShuffle ? '1' : '0') + ';';
    }
    return signature;
}

}

#endif // SPOTIFY_CLIENT_INTERNAL_H
