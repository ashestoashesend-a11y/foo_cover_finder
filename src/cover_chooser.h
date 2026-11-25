#pragma once

#include "cover_finder.h"

#include <vector>

// Shows the candidates as large thumbnails. Returns true and fills `chosen`
// with the index the user picked, false if the dialog was cancelled.
bool cover_chooser_show(HWND parent, std::vector<CoverCandidate> & candidates, size_t & chosen);
