#pragma once

#include "stdafx.h"

// One album art candidate, as returned by an online source.
struct CoverCandidate {
    pfc::string8 source;    // "iTunes", "Deezer"
    pfc::string8 album;
    pfc::string8 artist;
    pfc::string8 thumb_url; // small image, used for the dialog preview
    pfc::string8 image_url; // full size image, what actually gets saved
    int width = 0;          // nominal width in pixels, 0 when the source does not say
    int score = 0;          // how well this matches the tags being searched for
    bool approx = false;    // matched only after ignoring "(Deluxe Edition)" style markers
    pfc::array_t<t_uint8> thumb; // downloaded thumbnail bytes (JPEG/PNG)
};

// Entry point: work out the album from the selection, ask the sources, let the
// user pick one, save it as cover.jpg next to the files.
void cover_finder_run(metadb_handle_list_cref items);
