#include "stdafx.h"

#include "cover_finder.h"
#include "cover_chooser.h"

#include <SDK/album_art_helpers.h>

#include <cJSON.h>
#include <winhttp.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

// ------------------------------------------------------------------ popups --

// This SDK's popup_message has no error()/g_yesno(); v3 gives a modal query
// with dark mode support, v2 a plain message box as a fallback.
void show_error(const char * message) {
    popup_message_v3::ptr api;
    if (popup_message_v3::tryGet(api)) {
        popup_message_v3::query_t query;
        query.title = "Find cover art";
        query.msg = message;
        query.buttons = popup_message_v3::buttonOK;
        query.icon = popup_message_v3::iconError;
        query.wndParent = core_api::get_main_window();
        api->show_query_modal(query);
        return;
    }
    popup_message_v2::g_show(core_api::get_main_window(), message, "Find cover art");
}

bool ask_yes_no(const char * message) {
    popup_message_v3::ptr api;
    if (popup_message_v3::tryGet(api)) {
        popup_message_v3::query_t query;
        query.title = "Find cover art";
        query.msg = message;
        query.buttons = popup_message_v3::buttonYes | popup_message_v3::buttonNo;
        query.defButton = popup_message_v3::buttonNo;
        query.icon = popup_message_v3::iconQuestion;
        query.wndParent = core_api::get_main_window();
        return api->show_query_modal(query) == popup_message_v3::buttonYes;
    }
    return ::MessageBoxA(core_api::get_main_window(), message, "Find cover art", MB_YESNO | MB_ICONQUESTION) == IDYES;
}

// ------------------------------------------------------------------ HTTP ----

pfc::string8 g_user_agent; // set once, reused for every request

void ensure_user_agent() {
    if (g_user_agent.is_empty()) g_user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) foo_cover_finder/0.1";
}

// Minimal GET. Returns false and fills `error` on anything that is not a 200
// with a body; the caller decides whether that is fatal.
bool http_get(const char * url, const char * referer, pfc::array_t<t_uint8> & out, abort_callback & abort, pfc::string8 & error) {
    out.set_size(0);
    error.reset();
    ensure_user_agent();

    pfc::stringcvt::string_wide_from_utf8 wide_url(url);

    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    wchar_t extra[512] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = _countof(host) - 1;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = _countof(path) - 1;
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = _countof(extra) - 1;
    if (!WinHttpCrackUrl(wide_url, 0, 0, &parts)) {
        error = "Could not parse the URL.";
        return false;
    }

    // path/extra are already UTF-16, straight out of WinHttpCrackUrl.
    std::wstring target = std::wstring(path) + std::wstring(extra);

    HINTERNET session = WinHttpOpen(L"foo_cover_finder/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) {
        error = "Could not start WinHTTP.";
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 20000);
    // Cover Art Archive (and occasionally iTunes) answer with a redirect to the
    // actual image, and WinHTTP does not follow those unless told to.
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));

    bool ok = false;
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    if (connection != NULL) {
        const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", target.c_str(), NULL, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (request != NULL) {
            pfc::stringcvt::string_wide_from_utf8 wide_agent(g_user_agent);
            std::wstring headers = L"User-Agent: " + std::wstring(wide_agent.get_ptr()) + L"\r\n";
            if (referer != NULL) {
                pfc::stringcvt::string_wide_from_utf8 wide_referer(referer);
                headers += L"Referer: " + std::wstring(wide_referer.get_ptr()) + L"\r\n";
            }
            WinHttpAddRequestHeaders(request, headers.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, NULL)) {
                DWORD status = 0, status_size = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &status_size, WINHTTP_NO_HEADER_INDEX);
                if (status != 200) {
                    error << "HTTP " << (unsigned)status;
                } else {
                    ok = true;
                    for (;;) {
                        if (abort.is_aborting()) {
                            error = "Cancelled.";
                            ok = false;
                            break;
                        }
                        DWORD available = 0;
                        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                        const t_size old_size = out.get_size();
                        out.set_size(old_size + available);
                        DWORD read = 0;
                        if (!WinHttpReadData(request, out.get_ptr() + old_size, available, &read)) {
                            error = "Reading the reply failed.";
                            ok = false;
                            break;
                        }
                        out.set_size(old_size + read);
                        if (read == 0) break;
                    }
                }
            } else {
                error = "The connection failed.";
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);

    if (ok && out.get_size() == 0) {
        error = "The server returned an empty reply.";
        ok = false;
    }
    return ok;
}

// ------------------------------------------------------------- helpers ----

void url_encode(const char * text, pfc::string8 & out) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char * p = (const unsigned char *)text; *p != 0; p++) {
        const unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out.add_char((char)c);
        } else if (c == ' ') {
            out.add_char('+');
        } else {
            out.add_char('%');
            out.add_char(hex[c >> 4]);
            out.add_char(hex[c & 0x0F]);
        }
    }
}

const char * json_string(cJSON * object, const char * key) {
    if (object == NULL) return NULL;
    cJSON * item = cJSON_GetObjectItem(object, key);
    if (item == NULL || item->type != cJSON_String) return NULL;
    return item->valuestring;
}

// "..../100x100bb.jpg" -> 300px preview and 1200px full size variants.
void itunes_art_urls(const char * art100, pfc::string8 & thumb, pfc::string8 & full) {
    pfc::string8 base(art100);
    thumb = base;
    full = base;
    const t_size position = base.find_first("100x100bb");
    if (position == pfc::infinite_size) return;
    const char * tail = base.get_ptr() + position + 9;
    pfc::string8 head(base.get_ptr(), position);
    thumb = head;
    thumb << "300x300bb" << tail;
    full = head;
    full << "1200x1200bb" << tail;
}

// -------------------------------------------------------------- matching ----

std::string lowered(const char * text) {
    std::string result(text != NULL ? text : "");
    for (size_t i = 0; i < result.size(); i++) {
        const unsigned char c = (unsigned char)result[i];
        if (c >= 'A' && c <= 'Z') result[i] = (char)(c + 32);
    }
    return result;
}

// Folds away the noise sources add to album names: case, quotes, and the
// " - Single" / " (Cover)" tails that would otherwise stop an exact match.
std::string normalized(const char * text) {
    std::string result = lowered(text);
    static const char * const suffixes[] = { " - single", " - ep", " - single version", " (cover)", " - cover" };
    bool trimmed = true;
    while (trimmed) {
        trimmed = false;
        while (!result.empty() && (result[result.size() - 1] == ' ' || result[result.size() - 1] == '"' ||
                                   result[result.size() - 1] == '\'' || result[result.size() - 1] == '.'))
            result.erase(result.size() - 1);
        for (size_t i = 0; i < _countof(suffixes); i++) {
            const std::string suffix(suffixes[i]);
            if (result.size() > suffix.size() && result.compare(result.size() - suffix.size(), suffix.size(), suffix) == 0) {
                result.erase(result.size() - suffix.size());
                trimmed = true;
            }
        }
    }
    return result;
}

int score_candidate(const char * wanted_album, const char * wanted_artist, const CoverCandidate & candidate) {
    const std::string album = normalized(wanted_album);
    const std::string artist = normalized(wanted_artist);
    const std::string got_album = normalized(candidate.album);
    const std::string got_artist = normalized(candidate.artist);

    int score = 0;
    // The substring rules need a length floor: a one-letter album name is a
    // substring of half the catalogue ("R" matching "avant-garde history").
    if (!album.empty() && !got_album.empty()) {
        if (got_album == album)
            score += 100;
        else if (album.size() >= 4 && got_album.find(album) != std::string::npos)
            score += 60;
        else if (got_album.size() >= 4 && album.find(got_album) != std::string::npos)
            score += 40;
    }
    if (!artist.empty() && !got_artist.empty()) {
        if (got_artist == artist)
            score += 40;
        else if (artist.size() >= 3 && got_artist.find(artist) != std::string::npos)
            score += 25;
        else if (got_artist.size() >= 3 && artist.find(got_artist) != std::string::npos)
            score += 15;
    }
    return score;
}

// -------------------------------------------------------------- sources ----

void itunes_query(const char * term, std::vector<CoverCandidate> & out, abort_callback & abort, pfc::string8 & error) {
    pfc::string8 url("https://itunes.apple.com/search?term=");
    url_encode(term, url);
    url << "&entity=album&limit=50&country=JP";

    pfc::array_t<t_uint8> body;
    pfc::string8 request_error;
    if (!http_get(url, NULL, body, abort, request_error)) {
        if (error.is_empty()) error << "iTunes: " << request_error;
        return;
    }
    body.append_single(0);
    cJSON * root = cJSON_Parse((const char *)body.get_ptr());
    if (root == NULL) {
        if (error.is_empty()) error = "iTunes: the reply could not be parsed.";
        return;
    }

    cJSON * results = cJSON_GetObjectItem(root, "results");
    const int count = cJSON_GetArraySize(results);
    for (int i = 0; i < count; i++) {
        cJSON * item = cJSON_GetArrayItem(results, i);
        const char * art = json_string(item, "artworkUrl100");
        const char * album = json_string(item, "collectionName");
        if (art == NULL || album == NULL) continue;

        CoverCandidate candidate;
        candidate.source = "iTunes";
        candidate.album = album;
        const char * artist = json_string(item, "artistName");
        candidate.artist = artist != NULL ? artist : "";
        candidate.width = 1200;
        itunes_art_urls(art, candidate.thumb_url, candidate.image_url);
        out.push_back(candidate);
    }
    cJSON_Delete(root);
}

void netease_query(const char * term, std::vector<CoverCandidate> & out, abort_callback & abort, pfc::string8 & error) {
    pfc::string8 url("https://music.163.com/api/cloudsearch/pc?s=");
    url_encode(term, url);
    url << "&type=10&limit=20";

    pfc::array_t<t_uint8> body;
    pfc::string8 request_error;
    if (!http_get(url, "https://music.163.com/", body, abort, request_error)) {
        if (error.is_empty()) error << "NetEase: " << request_error;
        return;
    }
    body.append_single(0);
    cJSON * root = cJSON_Parse((const char *)body.get_ptr());
    if (root == NULL) {
        if (error.is_empty()) error = "NetEase: the reply could not be parsed.";
        return;
    }

    cJSON * result = cJSON_GetObjectItem(root, "result");
    cJSON * albums = cJSON_GetObjectItem(result, "albums");
    const int count = cJSON_GetArraySize(albums);
    for (int i = 0; i < count; i++) {
        cJSON * album_object = cJSON_GetArrayItem(albums, i);
        const char * picture = json_string(album_object, "picUrl");
        const char * album = json_string(album_object, "name");
        if (picture == NULL || album == NULL) continue;

        CoverCandidate candidate;
        candidate.source = "NetEase";
        candidate.album = album;
        const char * artist = json_string(cJSON_GetObjectItem(album_object, "artist"), "name");
        candidate.artist = artist != NULL ? artist : "";
        candidate.width = 1000;
        candidate.thumb_url = picture;
        candidate.thumb_url << "?param=300y300";
        candidate.image_url = picture;
        candidate.image_url << "?param=1000y1000";
        out.push_back(candidate);
    }
    cJSON_Delete(root);
}

// QQ Music's proper search API needs a signed request; the "smart box"
// suggestion endpoint answers without one and does return albums, a couple at a
// time, which is enough when it is queried once per term.
void qq_query(const char * term, std::vector<CoverCandidate> & out, abort_callback & abort, pfc::string8 & error) {
    pfc::string8 url("https://c.y.qq.com/splcloud/fcgi-bin/smartbox_new.fcg?key=");
    url_encode(term, url);
    url << "&format=json";

    pfc::array_t<t_uint8> body;
    pfc::string8 request_error;
    if (!http_get(url, "https://y.qq.com/", body, abort, request_error)) {
        if (error.is_empty()) error << "QQ Music: " << request_error;
        return;
    }
    body.append_single(0);
    cJSON * root = cJSON_Parse((const char *)body.get_ptr());
    if (root == NULL) {
        if (error.is_empty()) error = "QQ Music: the reply could not be parsed.";
        return;
    }

    cJSON * albums = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"), "album"), "itemlist");
    const int count = cJSON_GetArraySize(albums);
    for (int i = 0; i < count; i++) {
        cJSON * album_object = cJSON_GetArrayItem(albums, i);
        const char * mid = json_string(album_object, "mid");
        const char * album = json_string(album_object, "name");
        if (mid == NULL || album == NULL || *mid == 0) continue;

        CoverCandidate candidate;
        candidate.source = "QQ Music";
        candidate.album = album;
        const char * artist = json_string(album_object, "singer");
        candidate.artist = artist != NULL ? artist : "";
        candidate.width = 800;
        // Built from the album mid instead of patching the 180x180 URL the API
        // hands out, which ends with a crop suffix that must not survive.
        candidate.thumb_url = "https://y.gtimg.cn/music/photo_new/T002R300x300M000";
        candidate.thumb_url << mid << ".jpg";
        candidate.image_url = "https://y.gtimg.cn/music/photo_new/T002R800x800M000";
        candidate.image_url << mid << ".jpg";
        out.push_back(candidate);
    }
    cJSON_Delete(root);
}

pfc::string8 first_artist_credit(cJSON * release) {
    cJSON * credits = cJSON_GetObjectItem(release, "artist-credit");
    if (credits == NULL || cJSON_GetArraySize(credits) < 1) return pfc::string8();
    const char * name = json_string(cJSON_GetArrayItem(credits, 0), "name");
    return pfc::string8(name != NULL ? name : "");
}

// MusicBrainz only indexes the metadata; the images live at Cover Art Archive,
// one extra request per release, and MusicBrainz asks for at most one request a
// second - hence the sleeps and the small probe budget.
void musicbrainz_query(const char * query, std::vector<CoverCandidate> & out, abort_callback & abort, pfc::string8 & error) {
    pfc::string8 url("https://musicbrainz.org/ws/2/release/?query=");
    url_encode(query, url);
    url << "&fmt=json&limit=6";

    pfc::array_t<t_uint8> body;
    pfc::string8 request_error;
    if (!http_get(url, NULL, body, abort, request_error)) {
        if (error.is_empty()) error << "MusicBrainz: " << request_error;
        return;
    }
    body.append_single(0);
    cJSON * root = cJSON_Parse((const char *)body.get_ptr());
    if (root == NULL) {
        if (error.is_empty()) error = "MusicBrainz: the reply could not be parsed.";
        return;
    }

    cJSON * releases = cJSON_GetObjectItem(root, "releases");
    const int count = cJSON_GetArraySize(releases);
    int probed = 0;
    for (int i = 0; i < count && probed < 3; i++) {
        if (abort.is_aborting()) break;
        cJSON * release = cJSON_GetArrayItem(releases, i);
        const char * id = json_string(release, "id");
        const char * album = json_string(release, "title");
        if (id == NULL || album == NULL) continue;

        pfc::string8 front("https://coverartarchive.org/release/");
        front << id << "/front-500";

        ::Sleep(1100);
        pfc::array_t<t_uint8> image;
        pfc::string8 image_error;
        if (!http_get(front, NULL, image, abort, image_error)) continue; // 404 = this release has no front cover
        probed++;

        CoverCandidate candidate;
        candidate.source = "MusicBrainz";
        candidate.album = album;
        candidate.artist = first_artist_credit(release);
        candidate.width = 500;
        candidate.thumb = image; // already downloaded, no second fetch
        candidate.thumb_url = front;
        candidate.image_url = "https://coverartarchive.org/release/";
        candidate.image_url << id << "/front-1200";
        out.push_back(candidate);
    }
    cJSON_Delete(root);
}

// No single source is good on its own: iTunes chokes on "<artist> <album>" as
// one term, QQ Music's key-less endpoint returns two albums at a time, NetEase
// is strong on anime releases and MusicBrainz only sometimes has scans. So ask
// every source with the terms it handles, merge, score against the tags and
// keep the best few.
void collect_candidates(const char * artist, const char * album, std::vector<CoverCandidate> & out, abort_callback & abort,
                        pfc::string8 & error) {
    std::vector<CoverCandidate> raw;

    pfc::string8 artist_query(artist);
    pfc::string8 album_query(album);
    pfc::string8 both_query(artist);
    if (!artist_query.is_empty()) both_query << " ";
    both_query << album;

    if (!artist_query.is_empty()) {
        itunes_query(artist_query, raw, abort, error);
        if (abort.is_aborting()) return;
        qq_query(artist_query, raw, abort, error);
        if (abort.is_aborting()) return;
    }
    itunes_query(album_query, raw, abort, error);
    if (abort.is_aborting()) return;
    qq_query(album_query, raw, abort, error);
    if (abort.is_aborting()) return;
    netease_query(both_query, raw, abort, error);
    if (abort.is_aborting()) return;

    pfc::string8 mb_query("release:\"");
    mb_query << album_query << "\"";
    if (!artist_query.is_empty()) mb_query << " AND artist:\"" << artist_query << "\"";
    musicbrainz_query(mb_query, raw, abort, error);
    if (abort.is_aborting()) return;

    for (size_t i = 0; i < raw.size(); i++) raw[i].score = score_candidate(album, artist, raw[i]);

    // Drop the obvious misses, but never so many that the list is empty.
    std::vector<CoverCandidate> relevant;
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i].score >= 40) relevant.push_back(raw[i]);
    }
    if (relevant.empty()) relevant = raw;

    std::sort(relevant.begin(), relevant.end(), [](const CoverCandidate & a, const CoverCandidate & b) {
        if (a.score != b.score) return a.score > b.score;
        return a.width > b.width;
    });

    // Same album from two queries (or twice in a row) only needs to be shown once.
    for (size_t i = 0; i < relevant.size() && out.size() < 12; i++) {
        bool duplicate = false;
        for (size_t j = 0; j < out.size(); j++) {
            if (normalized(out[j].album) == normalized(relevant[i].album) &&
                normalized(out[j].artist) == normalized(relevant[i].artist) && out[j].source == relevant[i].source) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) out.push_back(relevant[i]);
    }
}

// ------------------------------------------------------------------ tasks --

class CoverSearchTask : public threaded_process_callback {
public:
    CoverSearchTask(pfc::string8 artist, pfc::string8 album) : m_artist(artist), m_album(album) {}

    void run(threaded_process_status & status, abort_callback & abort) override {
        status.set_item("iTunes and NetEase");
        pfc::string8 error;
        collect_candidates(m_artist, m_album, m_candidates, abort, error);
        if (!error.is_empty()) m_warnings.add_item(error);
        if (abort.is_aborting()) throw exception_aborted();

        // Pull the previews down so the chooser can show them; a missing preview
        // is not fatal, that entry just gets a blank tile.
        for (size_t i = 0; i < m_candidates.size(); i++) {
            if (abort.is_aborting()) throw exception_aborted();
            status.set_item(m_candidates[i].album);
            if (m_candidates[i].thumb.get_size() > 0) continue; // fetched while searching
            pfc::string8 ignored;
            http_get(m_candidates[i].thumb_url, NULL, m_candidates[i].thumb, abort, ignored);
        }
        status.set_item("");
    }

    std::vector<CoverCandidate> m_candidates;
    pfc::list_t<pfc::string8> m_warnings;

private:
    pfc::string8 m_artist;
    pfc::string8 m_album;
};

bool write_native_file(const char * path, const pfc::array_t<t_uint8> & bytes, pfc::string8 & error) {
    pfc::stringcvt::string_wide_from_utf8 wide_path(path);
    HANDLE file = CreateFileW(wide_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        error << "Could not create " << path;
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.get_ptr(), (DWORD)bytes.get_size(), &written, NULL);
    CloseHandle(file);
    if (!ok || written != bytes.get_size()) {
        error << "Could not write " << path;
        return false;
    }
    return true;
}

class CoverDownloadTask : public threaded_process_callback {
public:
    CoverDownloadTask(pfc::string8 url, pfc::string8 native_target, pfc::string8 fallback_target)
        : m_url(url), m_native_target(native_target), m_fallback_target(fallback_target) {}

    void run(threaded_process_status & status, abort_callback & abort) override {
        status.set_item("Downloading the cover");
        pfc::array_t<t_uint8> bytes;
        if (!http_get(m_url, NULL, bytes, abort, m_error)) return;

        if (write_native_file(m_native_target, bytes, m_error)) return;

        // Fall back to foobar2000's own filesystem layer with the canonical path:
        // it accepts the file:/// form that CreateFileW rejects, and it is what
        // the path came from in the first place.
        pfc::string8 native_error(m_error);
        try {
            {
                file::ptr output;
                filesystem::g_open_write_new(output, m_fallback_target, abort);
                output->write(bytes.get_ptr(), bytes.get_size(), abort);
            } // the file is closed and flushed when the service pointer goes away
            m_error.reset();
        } catch (std::exception const & e) {
            m_error = native_error;
            m_error << " (" << e.what() << ")";
        }
    }

    pfc::string8 m_error;

private:
    pfc::string8 m_url;
    pfc::string8 m_native_target;
    pfc::string8 m_fallback_target;
};

// Mixed folders: cover.jpg belongs to the whole folder, so a per-album image
// would show up on every unrelated track in there. In that case the picture goes
// into each selected file's own tags instead.
class CoverEmbedTask : public threaded_process_callback {
public:
    CoverEmbedTask(pfc::string8 url, metadb_handle_list items) : m_url(url), m_items(items) {}

    void run(threaded_process_status & status, abort_callback & abort) override {
        status.set_item("Downloading the cover");
        pfc::array_t<t_uint8> bytes;
        if (!http_get(m_url, NULL, bytes, abort, m_error)) return;

        album_art_data_ptr data;
        try {
            data = album_art_data_impl::g_create(bytes.get_ptr(), bytes.get_size());
        } catch (std::exception const & e) {
            m_error << "Could not prepare the image: " << e.what();
            return;
        }

        for (t_size i = 0; i < m_items.get_count(); i++) {
            if (abort.is_aborting()) throw exception_aborted();
            status.set_item(m_items[i]->get_path());
            pfc::string8 what_failed;
            try {
                album_art_editor_instance_ptr editor = album_art_editor::g_open(NULL, m_items[i]->get_path(), abort);
                editor->set(album_art_ids::cover_front, data, abort);
                editor->commit(abort);
                m_written++;
            } catch (std::exception const & e) {
                m_failed++;
                if (m_first_failure.is_empty()) m_first_failure << m_items[i]->get_path() << ": " << e.what();
            }
        }
        status.set_item("");

        if (m_written == 0) {
            m_error = "No file could take the cover art. ";
            if (!m_first_failure.is_empty()) m_error << m_first_failure;
        }
    }

    t_size m_written = 0;
    t_size m_failed = 0;
    pfc::string8 m_first_failure;
    pfc::string8 m_error;

private:
    pfc::string8 m_url;
    metadb_handle_list m_items;
};

// ------------------------------------------------------------- plumbing ----

// metadb hands out canonical paths, which for local files can still be
// "file:///C:\..." URLs; everything that touches the disk needs the native form.
// "file:///C:/Music/a%20b.flac" -> "C:\Music\a b.flac". g_get_display_path is
// supposed to do this, but it does not always (and it happily leaves the URL
// alone when the path is already in that shape), so the conversion is repeated
// by hand afterwards.
void undecorate_file_url(pfc::string8 & text) {
    if (text.find_first("file://") != 0) return;
    pfc::string8 rest(text.get_ptr() + 7);
    // file:///C:/... keeps a slash in front of the drive letter
    if (rest.get_length() > 2 && rest[0] == '/' && rest[2] == ':') rest = pfc::string8(rest.get_ptr() + 1);

    pfc::string8 decoded;
    for (t_size i = 0; i < rest.get_length(); i++) {
        const char c = rest[i];
        if (c == '%' && i + 2 < rest.get_length()) {
            int value = 0;
            bool valid = true;
            for (int k = 1; k <= 2; k++) {
                const char digit = rest[i + k];
                value <<= 4;
                if (digit >= '0' && digit <= '9')
                    value += digit - '0';
                else if (digit >= 'a' && digit <= 'f')
                    value += digit - 'a' + 10;
                else if (digit >= 'A' && digit <= 'F')
                    value += digit - 'A' + 10;
                else
                    valid = false;
            }
            if (valid) {
                decoded.add_char((char)value);
                i += 2;
                continue;
            }
        }
        decoded.add_char(c);
    }
    text = decoded;
}

bool native_path_of(const metadb_handle_ptr & item, pfc::string8 & out) {
    const char * path = item->get_path();
    if (path == NULL || *path == 0) return false;
    try {
        filesystem::g_get_display_path(path, out);
    } catch (std::exception const &) {
        return false;
    }
    if (out.is_empty()) return false;
    undecorate_file_url(out);
    // Anything still carrying a protocol is remote (upnp, http) and has no
    // folder to write into.
    if (out.find_first("://") != pfc::infinite_size) return false;
    return true;
}

bool folder_of(const metadb_handle_ptr & item, pfc::string8 & folder) {
    pfc::string8 text;
    if (!native_path_of(item, text)) return false;
    t_size cut = pfc::infinite_size;
    for (t_size i = text.get_length(); i > 0; i--) {
        const char c = text[i - 1];
        if (c == '\\' || c == '/') {
            cut = i - 1;
            break;
        }
    }
    if (cut == pfc::infinite_size || cut < 3) return false;
    folder = text;
    folder.truncate(cut);
    return true;
}

} // namespace

// Does this folder hold anything besides the album being searched for? Uses the
// media library, which is the only cheap way to look at files we were not given.
bool folder_is_mixed(const pfc::string8 & folder, const char * album, t_size & tracks_in_folder) {
    tracks_in_folder = 0;
    bool mixed = false;
    try {
        metadb_handle_list everything;
        library_manager::get()->get_all_items(everything);
        const std::string wanted_folder = lowered(folder.get_ptr());
        const std::string wanted_album = normalized(album);

        for (t_size i = 0; i < everything.get_count(); i++) {
            pfc::string8 item_folder;
            if (!folder_of(everything[i], item_folder)) continue;
            if (lowered(item_folder.get_ptr()) != wanted_folder) continue;
            tracks_in_folder++;

            const file_info & info = everything[i]->get_info_ref()->info();
            if (normalized(info.meta_get("album", 0)) != wanted_album) mixed = true;
        }
    } catch (std::exception const & e) {
        console::print(pfc::string8("Cover art: could not inspect the folder (") << e.what()
                                                                              << "), assuming it holds a single album.");
    }
    return mixed;
}

void cover_finder_run(metadb_handle_list_cref items) {
    if (items.get_count() == 0) return;

    pfc::string8 folder;
    if (!folder_of(items[0], folder)) {
        show_error("This track is not a local file, so there is no album folder to save cover art into.");
        return;
    }
    for (t_size i = 1; i < items.get_count(); i++) {
        pfc::string8 other;
        if (!folder_of(items[i], other) || other != folder) {
            show_error("The selection spans more than one album folder. Select the tracks of a single album.");
            return;
        }
    }

    const file_info & info = items[0]->get_info_ref()->info();
    const char * album = info.meta_get("album", 0);
    if (album == NULL || *album == 0) {
        show_error("These tracks have no album tag, so there is nothing to search for.");
        return;
    }
    const char * album_artist = info.meta_get("album artist", 0);
    if (album_artist == NULL) album_artist = info.meta_get("artist", 0);
    if (album_artist == NULL) album_artist = "";

    service_ptr_t<CoverSearchTask> search = new service_impl_t<CoverSearchTask>(pfc::string8(album_artist), pfc::string8(album));
    if (!threaded_process::get()->run_modal(search, threaded_process::flag_show_progress | threaded_process::flag_show_item |
                                                       threaded_process::flag_show_abort,
                                            core_api::get_main_window(), "Searching for cover art"))
        return; // the user aborted

    for (t_size i = 0; i < search->m_warnings.get_count(); i++) {
        console::print(search->m_warnings[i]);
    }
    if (search->m_candidates.empty()) {
        pfc::string8 message("No cover art was found for \"");
        message << album << "\" by \"" << album_artist << "\".";
        if (search->m_warnings.get_count() > 0) message << "\n\nBoth sources may have failed; see the console for details.";
        show_error(message);
        return;
    }

    size_t chosen = 0;
    if (!cover_chooser_show(core_api::get_main_window(), search->m_candidates, chosen)) return;

    // A cover.jpg is shared by everything in the folder, so it is only the right
    // answer when the folder actually holds this album and nothing else.
    t_size tracks_in_folder = 0;
    if (folder_is_mixed(folder, album, tracks_in_folder)) {
        pfc::string8 question("This folder holds tracks from more than one album (");
        question << tracks_in_folder << " tracks), so cover.jpg would show up on all of them.\n\nThe cover will be embedded "
                 << "into the tags of the " << items.get_count()
                 << " selected track(s) instead, which rewrites those files.\n\nGo ahead?";
        if (!ask_yes_no(question)) return;

        metadb_handle_list selected;
        selected.add_items(items);
        service_ptr_t<CoverEmbedTask> embed =
            new service_impl_t<CoverEmbedTask>(search->m_candidates[chosen].image_url, selected);
        if (!threaded_process::get()->run_modal(embed, threaded_process::flag_show_progress |
                                                           threaded_process::flag_show_item |
                                                           threaded_process::flag_show_abort,
                                                core_api::get_main_window(), "Embedding cover art"))
            return;
        if (!embed->m_error.is_empty()) {
            show_error(embed->m_error);
            return;
        }

        metadb_io::get()->dispatch_refresh(items);
        pfc::string8 done("Cover art embedded into ");
        done << embed->m_written << " track(s)";
        if (embed->m_failed > 0) done << ", " << embed->m_failed << " skipped (format does not support artwork)";
        console::print(done);
        fb2k::showToast(done);
        return;
    }

    pfc::string8 target = folder;
    target << "\\cover.jpg";
    if (filesystem::g_exists(target, fb2k::noAbort)) {
        pfc::string8 question("cover.jpg already exists in\n");
        question << folder << "\n\nReplace it?";
        if (!ask_yes_no(question)) return;
    }

    // Two forms of the same destination: the native path for CreateFileW, and the
    // path exactly as metadb reported it for foobar2000's filesystem layer.
    pfc::string8 canonical_target(items[0]->get_path());
    canonical_target << "\\cover.jpg";

    service_ptr_t<CoverDownloadTask> download =
        new service_impl_t<CoverDownloadTask>(search->m_candidates[chosen].image_url, target, canonical_target);
    if (!threaded_process::get()->run_modal(download, threaded_process::flag_show_progress | threaded_process::flag_show_item |
                                                          threaded_process::flag_show_abort,
                                            core_api::get_main_window(), "Saving cover art"))
        return;

    if (!download->m_error.is_empty()) {
        show_error(download->m_error);
        return;
    }

    // Nudge the metadb so viewers reload the artwork for these tracks.
    metadb_io::get()->dispatch_refresh(items);
    pfc::string8 done("Cover art saved to ");
    done << target;
    console::print(done);
    fb2k::showToast(done);
}
