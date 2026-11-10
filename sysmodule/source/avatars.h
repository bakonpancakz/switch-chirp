// People's pictures, fetched from Discord's CDN onto the card as small PNGs
// (CHIRP_AVATARS/<file>.png) for the overlay to decode and draw. The
// sysmodule never holds the pixels: a picture goes from the TLS connection
// to the card in 2 KB pieces. Its own thread, its own TLS connection, one
// picture at a time.
#pragma once
#include <stdint.h>

namespace avatars {

void start();  // once the sockets and ssl are up
void stop();

// Someone's picture is wanted (file: ChirpMember.avatar). Already on the
// card, fetched or tried before: nothing happens.
void want(uint64_t user, uint64_t guild, const char* file);

}  // namespace avatars
