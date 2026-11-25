#include "stdafx.h"

#include "cover_chooser.h"
#include "resource.h"

#include <wincodec.h>
#include <shlwapi.h>

#include <algorithm>
#include <vector>

namespace {

const int kIconSize = 128;

// Decodes a JPEG/PNG blob into a kIconSize square bitmap, letterboxed on a
// neutral background so different aspect ratios line up in the list view.
HBITMAP decode_thumbnail(const pfc::array_t<t_uint8> & bytes, COLORREF background) {
    HBITMAP bitmap = NULL;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kIconSize;
    bmi.bmiHeader.biHeight = -kIconSize; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void * bits = NULL;
    HBITMAP dib = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (dib == NULL) return NULL;

    // Fill with the background colour first; anything not covered by the image
    // stays neutral.
    const DWORD bgra = ((DWORD)GetBValue(background) << 16) | ((DWORD)GetGValue(background) << 8) | (DWORD)GetRValue(background);
    DWORD * pixels = (DWORD *)bits;
    for (int i = 0; i < kIconSize * kIconSize; i++) pixels[i] = bgra | 0xFF000000;

    if (bytes.get_size() > 0) {
        IWICImagingFactory * factory = NULL;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
            IStream * stream = SHCreateMemStream(bytes.get_ptr(), (UINT)bytes.get_size());
            if (stream != NULL) {
                IWICBitmapDecoder * decoder = NULL;
                if (SUCCEEDED(factory->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder))) {
                    IWICBitmapFrameDecode * frame = NULL;
                    if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
                        IWICFormatConverter * converter = NULL;
                        if (SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                            SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
                                                            WICBitmapPaletteTypeCustom))) {
                            UINT src_width = 0, src_height = 0;
                            if (SUCCEEDED(converter->GetSize(&src_width, &src_height)) && src_width > 0 && src_height > 0) {
                                // No std::min/max here: windows.h defines min/max macros
                                // and they mangle the qualified call.
                                const double scale_x = (double)kIconSize / src_width;
                                const double scale_y = (double)kIconSize / src_height;
                                const double scale = scale_x < scale_y ? scale_x : scale_y;
                                UINT width = (UINT)(src_width * scale);
                                UINT height = (UINT)(src_height * scale);
                                if (width < 1) width = 1;
                                if (height < 1) height = 1;
                                const UINT x = (kIconSize - width) / 2, y = (kIconSize - height) / 2;
                                WICRect rect = { (INT)x, (INT)y, (INT)width, (INT)height };
                                if (SUCCEEDED(converter->CopyPixels(&rect, kIconSize * 4, kIconSize * kIconSize * 4, (BYTE *)bits))) {
                                    bitmap = dib;
                                }
                            }
                        }
                        if (converter != NULL) converter->Release();
                        frame->Release();
                    }
                    decoder->Release();
                }
                stream->Release();
            }
            factory->Release();
        }
    }

    if (bitmap == NULL) {
        // Undecodable image: hand back the grey square so the list still shows
        // an entry instead of a hole.
        bitmap = dib;
    }
    return bitmap;
}

class CoverChooserDlg : public CDialogImpl<CoverChooserDlg> {
public:
    enum { IDD = IDD_COVER_CHOOSER };

    std::vector<CoverCandidate> * candidates = NULL;
    size_t chosen = (size_t)-1;

    BEGIN_MSG_MAP_EX(CoverChooserDlg)
        MSG_WM_INITDIALOG(OnInitDialog)
        COMMAND_ID_HANDLER_EX(IDOK, OnOk)
        COMMAND_ID_HANDLER_EX(IDCANCEL, OnCancel)
        NOTIFY_HANDLER_EX(IDC_COVER_LIST, LVN_ITEMCHANGED, OnSelectionChanged)
        NOTIFY_HANDLER_EX(IDC_COVER_LIST, NM_DBLCLK, OnDoubleClick)
    END_MSG_MAP()

private:
    BOOL OnInitDialog(CWindow focus, LPARAM init_param) {
        CListViewCtrl list = GetDlgItem(IDC_COVER_LIST);
        list.SetExtendedListViewStyle(LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT);

        m_images = ImageList_Create(kIconSize, kIconSize, ILC_COLOR32, 4, 4);
        list.SetImageList(m_images, LVSIL_NORMAL);

        const int count = (int)candidates->size();
        for (int i = 0; i < count; i++) {
            const CoverCandidate & candidate = (*candidates)[i];
            const COLORREF background = ::GetSysColor(COLOR_BTNFACE);
            HBITMAP bitmap = decode_thumbnail(candidate.thumb, background);
            int image_index = -1;
            if (bitmap != NULL) {
                image_index = ImageList_Add(m_images, bitmap, NULL);
                DeleteObject(bitmap);
            }

            pfc::string8 caption;
            caption << candidate.source;
            if (candidate.width > 0) caption << " " << candidate.width << "px";

            pfc::stringcvt::string_wide_from_utf8 wide_caption(caption);
            LVITEM item = {};
            item.mask = LVIF_TEXT | LVIF_IMAGE;
            item.iItem = i;
            item.iImage = image_index;
            item.pszText = const_cast<wchar_t *>(wide_caption.get_ptr());
            list.InsertItem(&item);
        }

        if (count > 0) {
            list.SetItemState(0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            update_status(0);
        } else {
            uSetDlgItemText(m_hWnd, IDC_COVER_STATUS, "Nothing found.");
            ::EnableWindow(::GetDlgItem(m_hWnd, IDOK), FALSE);
        }

        return TRUE;
    }

    void update_status(int index) {
        pfc::string8 text;
        if (index >= 0 && (size_t)index < candidates->size()) {
            const CoverCandidate & candidate = (*candidates)[index];
            text << candidate.album << " \xE2\x80\x94 " << candidate.artist << "    (" << candidate.source
                 << (candidate.approx ? ", different edition" : "");
            if (candidate.width > 0) text << ", " << candidate.width << " px";
            text << ")    [" << (index + 1) << " / " << (unsigned)candidates->size() << "]";
        }
        uSetDlgItemText(m_hWnd, IDC_COVER_STATUS, text);
    }

    void on_selection_changed() {
        CListViewCtrl list = GetDlgItem(IDC_COVER_LIST);
        const int selection = list.GetNextItem(-1, LVNI_SELECTED);
        if (selection >= 0) update_status(selection);
    }

    LRESULT OnSelectionChanged(LPNMHDR) {
        on_selection_changed();
        return 0;
    }

    LRESULT OnDoubleClick(LPNMHDR) {
        CListViewCtrl list = GetDlgItem(IDC_COVER_LIST);
        const int selection = list.GetNextItem(-1, LVNI_SELECTED);
        if (selection >= 0) {
            chosen = (size_t)selection;
            EndDialog(IDOK);
        }
        return 0;
    }

    void OnOk(UINT, int, CWindow) {
        CListViewCtrl list = GetDlgItem(IDC_COVER_LIST);
        const int selection = list.GetNextItem(-1, LVNI_SELECTED);
        if (selection >= 0) {
            chosen = (size_t)selection;
            EndDialog(IDOK);
        }
    }

    void OnCancel(UINT, int, CWindow) { EndDialog(IDCANCEL); }

    HIMAGELIST m_images = NULL;
};

} // namespace

bool cover_chooser_show(HWND parent, std::vector<CoverCandidate> & candidates, size_t & chosen) {
    CoverChooserDlg dialog;
    dialog.candidates = &candidates;
    if (dialog.DoModal(parent) != IDOK) return false;
    if (dialog.chosen >= candidates.size()) return false;
    chosen = dialog.chosen;
    return true;
}
