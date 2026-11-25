#include "stdafx.h"

#include "cover_finder.h"

DECLARE_COMPONENT_VERSION("Cover art finder", "0.1.0",
                          "Finds album art online from the playlist context menu and saves the image you pick as cover.jpg "
                          "next to the tracks.\n"
                          "Sources: iTunes Search (JP store) and NetEase Cloud Music. No API keys, no accounts.");

VALIDATE_COMPONENT_FILENAME("foo_cover_finder.dll");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;

namespace {

const GUID guid_cover_finder_menu = { 0x5f4e2b71, 0x9c3d, 0x4a86, { 0xb2, 0x59, 0x71, 0x0e, 0xc4, 0x8b, 0x36, 0xd2 } };

} // namespace

DECLARE_CONTEXT_MENU_ITEM(cover_finder_menu_item, "Find cover art (online)", "", cover_finder_run, guid_cover_finder_menu,
                          "Search iTunes and NetEase for this album's artwork, then save the image you pick as cover.jpg")
