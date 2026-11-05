#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <vector>
#include <cmath>
#include <map>
#include <fstream>
#include "filter.h"
#include <blend2d/blend2d.h>

#ifndef FILTER_FLAG_EX_DATA
#define FILTER_FLAG_EX_DATA 2048
#endif

#define EXDATA_MAGIC 0x48544150
#define EXDATA_SIZE 65536

enum PathType {
    PATH_CURVE,
    PATH_LINE
};

struct PathPoint {
    double x;
    double y;
};

struct PathData {
    PathType type;
    std::vector<PathPoint> pts;
    double size;
    uint8_t r, g, b;
    bool is_round;
    bool is_fill;
};

static std::map<int, std::vector<PathData>> g_db;
static std::map<int, int> g_sel_map;
static int g_dragging_idx = -1;
static HWND g_hwndDlg = NULL;
static bool g_syncing_ui = false;
static int g_img_w = 1920;
static int g_img_h = 1080;

// ExEdit UI Polling variables
static int g_last_show_check = -1;
static FILTER* g_last_fp = NULL;

static inline uint8_t clamp8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static uint32_t ycp_to_prgb32(const PIXEL_YC& p) {
    int Y  = p.y  * 255 / 4096;
    int Cb = p.cb * 255 / 4096;
    int Cr = p.cr * 255 / 4096;
    int R = Y + (1402 * Cr) / 1000;
    int G = Y - (344 * Cb) / 1000 - ( 714 * Cr) / 1000;
    int B = Y + (1772 * Cb) / 1000;
    return 0xFF000000u | ((uint32_t)clamp8(R) << 16) | ((uint32_t)clamp8(G) << 8) | (uint32_t)clamp8(B);
}

static PIXEL_YC prgb32_to_ycp(uint32_t pixel) {
    const uint8_t B = (uint8_t)(pixel & 0xFF);
    const uint8_t G = (uint8_t)((pixel >> 8) & 0xFF);
    const uint8_t R = (uint8_t)((pixel >> 16) & 0xFF);
    const int luma = 299 * R + 587 * G + 114 * B;
    const int chroma_b = -169 * R - 331 * G + 500 * B;
    const int chroma_r = 500 * R - 419 * G - 81 * B;
    PIXEL_YC result;
    result.y = (short)(luma * 4096 / 255000);
    result.cb = (short)(chroma_b * 4096 / 255000);
    result.cr = (short)(chroma_r * 4096 / 255000);
    return result;
}

static void catmull_to_bezier(PathPoint p0, PathPoint p1, PathPoint p2, PathPoint p3, PathPoint& c1, PathPoint& c2) {
    c1.x = p1.x + (p2.x - p0.x) / 6.0;
    c1.y = p1.y + (p2.y - p0.y) / 6.0;
    c2.x = p2.x - (p3.x - p1.x) / 6.0;
    c2.y = p2.y - (p3.y - p1.y) / 6.0;
}

void SaveDatabase() {
    std::ofstream ofs("path_draw_save.dat", std::ios::binary);
    if (!ofs) return;
    int count = (int)g_db.size();
    ofs.write((char*)&count, sizeof(int));
    for (auto& pair : g_db) {
        int id = pair.first;
        ofs.write((char*)&id, sizeof(int));
        int pcount = (int)pair.second.size();
        ofs.write((char*)&pcount, sizeof(int));
        for (auto& pd : pair.second) {
            int type = pd.type;
            ofs.write((char*)&type, sizeof(int));
            ofs.write((char*)&pd.size, sizeof(double));
            ofs.write((char*)&pd.r, 1);
            ofs.write((char*)&pd.g, 1);
            ofs.write((char*)&pd.b, 1);
            uint8_t rnd = pd.is_round ? 1 : 0;
            uint8_t fll = pd.is_fill ? 1 : 0;
            ofs.write((char*)&rnd, 1);
            ofs.write((char*)&fll, 1);
            int ptcnt = (int)pd.pts.size();
            ofs.write((char*)&ptcnt, sizeof(int));
            for (auto& pt : pd.pts) {
                ofs.write((char*)&pt.x, sizeof(double));
                ofs.write((char*)&pt.y, sizeof(double));
            }
        }
    }
}

void LoadDatabase() {
    std::ifstream ifs("path_draw_save.dat", std::ios::binary);
    if (!ifs) return;
    g_db.clear();
    int count = 0;
    if (!ifs.read((char*)&count, sizeof(int))) return;
    for (int i = 0; i < count; ++i) {
        int id = 0, pcount = 0;
        ifs.read((char*)&id, sizeof(int));
        ifs.read((char*)&pcount, sizeof(int));
        std::vector<PathData> paths;
        for (int j = 0; j < pcount; ++j) {
            PathData pd;
            int type = 0;
            ifs.read((char*)&type, sizeof(int)); pd.type = (PathType)type;
            ifs.read((char*)&pd.size, sizeof(double));
            ifs.read((char*)&pd.r, 1);
            ifs.read((char*)&pd.g, 1);
            ifs.read((char*)&pd.b, 1);
            uint8_t rnd = 0, fll = 0;
            ifs.read((char*)&rnd, 1); pd.is_round = (rnd != 0);
            ifs.read((char*)&fll, 1); pd.is_fill = (fll != 0);
            int ptcnt = 0;
            ifs.read((char*)&ptcnt, sizeof(int));
            for (int k = 0; k < ptcnt; ++k) {
                PathPoint pt;
                ifs.read((char*)&pt.x, sizeof(double));
                ifs.read((char*)&pt.y, sizeof(double));
                pd.pts.push_back(pt);
            }
            paths.push_back(pd);
        }
        g_db[id] = paths;
        if (g_sel_map.find(id) == g_sel_map.end()) g_sel_map[id] = 0;
    }
}

int GenerateNewID() {
    int new_id = 1;
    while (g_db.count(new_id) > 0) new_id++;
    return new_id;
}

BOOL func_proc(FILTER *fp, FILTER_PROC_INFO *fpip) {
    const int img_w = fpip->w;
    const int img_h = fpip->h;
    const int buf_w = fpip->max_w;
    PIXEL_YC * const ycp = fpip->ycp_edit;

    g_img_w = img_w;
    g_img_h = img_h;

    if (fp->track[4] == 0) {
        int new_id = GenerateNewID();
        fp->track[4] = new_id;
        
        PathData pd;
        pd.type = PATH_CURVE;
        pd.size = fp->track[0];
        pd.r = fp->track[1]; pd.g = fp->track[2]; pd.b = fp->track[3];
        pd.is_round = (fp->check[0] != 0);
        pd.is_fill = (fp->check[1] != 0);
        pd.pts.push_back({-0.1, 0.0});
        pd.pts.push_back({0.1, 0.0});
        g_db[new_id].push_back(pd);
        g_sel_map[new_id] = 0;
        
        SaveDatabase();
        if (fp->exfunc) fp->exfunc->filter_window_update(fp);
    }

    int current_id = fp->track[4];
    auto& current_paths = g_db[current_id];

    std::vector<uint32_t> pix_buf((size_t)img_w * img_h);
    for (int row = 0; row < img_h; ++row) {
        const PIXEL_YC* src_row = &ycp[row * buf_w];
        uint32_t* dst_row = &pix_buf[row * img_w];
        for (int col = 0; col < img_w; ++col) {
            dst_row[col] = ycp_to_prgb32(src_row[col]);
        }
    }

    BLImage img;
    img.create_from_data(img_w, img_h, BL_FORMAT_PRGB32, pix_buf.data(), (intptr_t)(img_w * 4));
    BLContext ctx(img);

    for (size_t pidx = 0; pidx < current_paths.size(); ++pidx) {
        auto& pd = current_paths[pidx];
        if (pd.pts.empty()) continue;

        uint32_t line_color = 0xFF000000u | (pd.r << 16) | (pd.g << 8) | pd.b;
        
        if (pd.is_round) {
            ctx.set_stroke_caps(BL_STROKE_CAP_ROUND);
            ctx.set_stroke_join(BL_STROKE_JOIN_ROUND);
        } else {
            ctx.set_stroke_caps(BL_STROKE_CAP_BUTT);
            ctx.set_stroke_join(BL_STROKE_JOIN_MITER_CLIP);
        }
        
        ctx.set_stroke_style(BLRgba32(line_color));
        ctx.set_stroke_width(pd.size);

        if (pd.pts.size() == 1) {
            ctx.set_fill_style(BLRgba32(line_color));
            double px = (pd.pts[0].x * img_w) + (img_w / 2.0);
            double py = (pd.pts[0].y * img_w) + (img_h / 2.0);
            ctx.fill_circle(px, py, pd.size / 2.0);
            continue;
        }

        BLPath path;
        double start_x = (pd.pts[0].x * img_w) + (img_w / 2.0);
        double start_y = (pd.pts[0].y * img_w) + (img_h / 2.0);
        path.move_to(start_x, start_y);

        if (pd.type == PATH_LINE) {
            for (size_t i = 1; i < pd.pts.size(); ++i) {
                double px = (pd.pts[i].x * img_w) + (img_w / 2.0);
                double py = (pd.pts[i].y * img_w) + (img_h / 2.0);
                path.line_to(px, py);
            }
        } else {
            std::vector<PathPoint> p;
            p.reserve(pd.pts.size() + 2);
            p.push_back(pd.pts.front());
            p.insert(p.end(), pd.pts.begin(), pd.pts.end());
            p.push_back(pd.pts.back());

            double mv_x = (p[1].x * img_w) + (img_w / 2.0);
            double mv_y = (p[1].y * img_w) + (img_h / 2.0);
            path.move_to(mv_x, mv_y);

            for (size_t i = 1; i + 2 < p.size(); ++i) {
                PathPoint c1, c2;
                catmull_to_bezier(p[i-1], p[i], p[i+1], p[i+2], c1, c2);
                
                double cp1_x = (c1.x * img_w) + (img_w / 2.0);
                double cp1_y = (c1.y * img_w) + (img_h / 2.0);
                double cp2_x = (c2.x * img_w) + (img_w / 2.0);
                double cp2_y = (c2.y * img_w) + (img_h / 2.0);
                double end_x = (p[i+1].x * img_w) + (img_w / 2.0);
                double end_y = (p[i+1].y * img_w) + (img_h / 2.0);
                
                path.cubic_to(cp1_x, cp1_y, cp2_x, cp2_y, end_x, end_y);
            }
        }
        
        if (pd.is_fill && pd.pts.size() > 2) {
            ctx.set_fill_style(BLRgba32(line_color));
            ctx.fill_path(path);
        }
        ctx.stroke_path(path);
    }

    ctx.end();

    for (int row = 0; row < img_h; ++row) {
        const uint32_t* src_row = &pix_buf[row * img_w];
        PIXEL_YC* dst_row = &ycp[row * buf_w];
        for (int col = 0; col < img_w; ++col) {
            dst_row[col] = prgb32_to_ycp(src_row[col]);
        }
    }
    return TRUE;
}

void draw_canvas(HWND hwnd, FILTER* fp) {
    int current_id = fp ? fp->track[4] : 0;
    if (current_id == 0) return;
    
    auto& current_paths = g_db[current_id];
    int selected_path = g_sel_map[current_id];

    RECT rect;
    GetClientRect(hwnd, &rect);
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

    HBRUSH hbrBg = CreateSolidBrush(RGB(40, 40, 40));
    FillRect(hdcMem, &rect, hbrBg);
    DeleteObject(hbrBg);

    int btn_h = 50;
    int canvas_area_h = h - btn_h;
    if (canvas_area_h < 10) canvas_area_h = 10;
    
    double aspect = 16.0 / 9.0;
    double cw = w;
    double ch = cw / aspect;
    if (ch > canvas_area_h) {
        ch = canvas_area_h;
        cw = ch * aspect;
    }
    double ox = (w - cw) / 2.0;
    double oy = btn_h + (canvas_area_h - ch) / 2.0;

    RECT rcCanvas = { (LONG)ox, (LONG)oy, (LONG)(ox + cw), (LONG)(oy + ch) };
    HBRUSH hbrCanvas = CreateSolidBrush(RGB(240, 240, 240));
    FillRect(hdcMem, &rcCanvas, hbrCanvas);
    DeleteObject(hbrCanvas);

    HBRUSH hbrBtn = CreateSolidBrush(RGB(70, 70, 70));
    RECT rcCurve = {10, 10, 80, 40};
    RECT rcLine  = {90, 10, 160, 40};
    RECT rcDel   = {170, 10, 240, 40};
    FillRect(hdcMem, &rcCurve, hbrBtn);
    FillRect(hdcMem, &rcLine, hbrBtn);
    FillRect(hdcMem, &rcDel, hbrBtn);
    DeleteObject(hbrBtn);

    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(255, 255, 255));
    DrawTextA(hdcMem, "+ Curve", -1, &rcCurve, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextA(hdcMem, "+ Line", -1, &rcLine, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextA(hdcMem, "Delete", -1, &rcDel, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    double scale = 1.0;
    if (g_img_w > 0) scale = cw / (double)g_img_w;

    for (size_t pidx = 0; pidx < current_paths.size(); ++pidx) {
        auto& pd = current_paths[pidx];
        bool is_sel = ((int)pidx == selected_path);

        int pen_size = (int)(pd.size * scale);
        if (pen_size < 1) pen_size = 1;

        COLORREF color = RGB(pd.b, pd.g, pd.r);
        if (!is_sel) {
            color = RGB(pd.b / 2, pd.g / 2, pd.r / 2);
        }

        HPEN hPenLine = CreatePen(PS_SOLID, pen_size, color);
        SelectObject(hdcMem, hPenLine);

        if (pd.pts.size() > 1) {
            if (pd.type == PATH_LINE) {
                MoveToEx(hdcMem, (int)(ox + cw / 2.0 + pd.pts[0].x * cw), (int)(oy + ch / 2.0 + pd.pts[0].y * cw), NULL);
                for (size_t i = 1; i < pd.pts.size(); ++i) {
                    LineTo(hdcMem, (int)(ox + cw / 2.0 + pd.pts[i].x * cw), (int)(oy + ch / 2.0 + pd.pts[i].y * cw));
                }
            } else {
                std::vector<PathPoint> p;
                p.reserve(pd.pts.size() + 2);
                p.push_back(pd.pts.front());
                p.insert(p.end(), pd.pts.begin(), pd.pts.end());
                p.push_back(pd.pts.back());

                MoveToEx(hdcMem, (int)(ox + cw / 2.0 + p[1].x * cw), (int)(oy + ch / 2.0 + p[1].y * cw), NULL);
                for (size_t i = 1; i + 2 < p.size(); ++i) {
                    PathPoint c1, c2;
                    catmull_to_bezier(p[i-1], p[i], p[i+1], p[i+2], c1, c2);
                    POINT pts[3] = {
                        { (int)(ox + cw / 2.0 + c1.x * cw), (int)(oy + ch / 2.0 + c1.y * cw) },
                        { (int)(ox + cw / 2.0 + c2.x * cw), (int)(oy + ch / 2.0 + c2.y * cw) },
                        { (int)(ox + cw / 2.0 + p[i+1].x * cw), (int)(oy + ch / 2.0 + p[i+1].y * cw) }
                    };
                    PolyBezierTo(hdcMem, pts, 3);
                }
            }
        }
        DeleteObject(hPenLine);

        HBRUSH hbrDot = CreateSolidBrush(is_sel ? RGB(255, 50, 50) : RGB(150, 150, 150));
        HPEN hPenDot = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        SelectObject(hdcMem, hbrDot);
        SelectObject(hdcMem, hPenDot);

        int rad = is_sel ? 5 : 3;
        for (size_t i = 0; i < pd.pts.size(); ++i) {
            int px = (int)(ox + cw / 2.0 + pd.pts[i].x * cw);
            int py = (int)(oy + ch / 2.0 + pd.pts[i].y * cw);
            Ellipse(hdcMem, px - rad, py - rad, px + rad, py + rad);
        }

        DeleteObject(hbrDot);
        DeleteObject(hPenDot);
    }

    BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
    EndPaint(hwnd, &ps);
}

INT_PTR CALLBACK DialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    FILTER* fp = (FILTER*)GetWindowLongPtr(hwndDlg, DWLP_USER);

    switch (uMsg) {
        case WM_INITDIALOG:
            SetWindowLongPtr(hwndDlg, DWLP_USER, (LONG_PTR)lParam);
            return TRUE;

        case WM_PAINT:
            draw_canvas(hwndDlg, fp);
            return 0;

        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: {
            if (!fp) return FALSE;
            int current_id = fp->track[4];
            if (current_id == 0) return FALSE;
            
            auto& current_paths = g_db[current_id];
            int& selected_path = g_sel_map[current_id];

            if (uMsg == WM_LBUTTONDOWN) SetCapture(hwndDlg);
            else if (uMsg == WM_LBUTTONUP) ReleaseCapture();

            POINT pt;
            pt.x = (short)LOWORD(lParam);
            pt.y = (short)HIWORD(lParam);

            bool needs_update = false;
            bool needs_save = false;

            if (uMsg == WM_LBUTTONDOWN && pt.y >= 10 && pt.y <= 40) {
                if ((pt.x >= 10 && pt.x <= 80) || (pt.x >= 90 && pt.x <= 160)) {
                    PathData pd;
                    pd.type = (pt.x <= 80) ? PATH_CURVE : PATH_LINE;
                    pd.size = fp->track[0];
                    pd.r = fp->track[1]; pd.g = fp->track[2]; pd.b = fp->track[3];
                    pd.is_round = (fp->check[0] != 0); 
                    pd.is_fill = (fp->check[1] != 0);
                    
                    current_paths.push_back(pd);
                    selected_path = (int)current_paths.size() - 1;
                    needs_update = true;
                    needs_save = true;
                }
                else if (pt.x >= 170 && pt.x <= 240) {
                    if (current_paths.size() > 1 && selected_path >= 0 && selected_path < (int)current_paths.size()) {
                        current_paths.erase(current_paths.begin() + selected_path);
                        selected_path = 0;
                    } else if (current_paths.size() == 1) {
                        current_paths[0].pts.clear();
                    }
                    needs_update = true;
                    needs_save = true;
                }
            }

            if (!needs_update) {
                RECT rect; GetClientRect(hwndDlg, &rect);
                int w = rect.right - rect.left;
                int h = rect.bottom - rect.top;
                
                int btn_h = 50;
                int canvas_area_h = h - btn_h;
                if (canvas_area_h >= 10) {
                    double aspect = 16.0 / 9.0;
                    double cw = w;
                    double ch = cw / aspect;
                    if (ch > canvas_area_h) {
                        ch = canvas_area_h;
                        cw = ch * aspect;
                    }
                    double ox = (w - cw) / 2.0;
                    double oy = btn_h + (canvas_area_h - ch) / 2.0;

                    if (cw > 0.0 && ch > 0.0) {
                        double nx = (pt.x - ox - cw / 2.0) / cw;
                        double ny = (pt.y - oy - ch / 2.0) / cw;

                        if (uMsg == WM_LBUTTONDOWN) {
                            g_dragging_idx = -1;
                            bool hit = false;
                            for (int pidx = (int)current_paths.size() - 1; pidx >= 0; --pidx) {
                                for (int i = 0; i < (int)current_paths[pidx].pts.size(); ++i) {
                                    double px = ox + cw / 2.0 + current_paths[pidx].pts[i].x * cw;
                                    double py = oy + ch / 2.0 + current_paths[pidx].pts[i].y * cw;
                                    double dx = px - pt.x;
                                    double dy = py - pt.y;
                                    if (dx * dx + dy * dy <= 20.0 * 20.0) { 
                                        if (selected_path != pidx) {
                                            selected_path = pidx;
                                            g_syncing_ui = true;
                                            fp->track[0] = (int)current_paths[pidx].size;
                                            fp->track[1] = current_paths[pidx].r;
                                            fp->track[2] = current_paths[pidx].g;
                                            fp->track[3] = current_paths[pidx].b;
                                            fp->check[0] = current_paths[pidx].is_round ? 1 : 0;
                                            fp->check[1] = current_paths[pidx].is_fill ? 1 : 0;
                                            g_syncing_ui = false;
                                            needs_save = true;
                                        }
                                        g_dragging_idx = i;
                                        hit = true;
                                        break;
                                    }
                                }
                                if (hit) break;
                            }
                            
                            if (!hit && pt.x >= ox && pt.x <= ox + cw && pt.y >= oy && pt.y <= oy + ch) {
                                if (selected_path < 0 || selected_path >= (int)current_paths.size()) {
                                    PathData pd;
                                    pd.type = PATH_CURVE;
                                    pd.size = fp->track[0];
                                    pd.r = fp->track[1]; pd.g = fp->track[2]; pd.b = fp->track[3];
                                    pd.is_round = (fp->check[0] != 0); 
                                    pd.is_fill = (fp->check[1] != 0);
                                    current_paths.push_back(pd);
                                    selected_path = (int)current_paths.size() - 1;
                                }
                                current_paths[selected_path].pts.push_back({nx, ny});
                                g_dragging_idx = (int)current_paths[selected_path].pts.size() - 1;
                                needs_update = true;
                                needs_save = true;
                            } else if (hit) {
                                needs_update = true;
                            }
                        } 
                        else if (uMsg == WM_MOUSEMOVE) {
                            if (g_dragging_idx != -1 && selected_path >= 0 && selected_path < (int)current_paths.size()) {
                                current_paths[selected_path].pts[g_dragging_idx].x = nx;
                                current_paths[selected_path].pts[g_dragging_idx].y = ny;
                                needs_update = true;
                            }
                        } 
                        else if (uMsg == WM_LBUTTONUP) {
                            if (g_dragging_idx != -1) {
                                g_dragging_idx = -1;
                                needs_update = true;
                                needs_save = true;
                            }
                        }
                        else if (uMsg == WM_RBUTTONDOWN) {
                            for (int pidx = 0; pidx < (int)current_paths.size(); ++pidx) {
                                bool deleted = false;
                                for (auto it = current_paths[pidx].pts.begin(); it != current_paths[pidx].pts.end(); ++it) {
                                    double px = ox + cw / 2.0 + it->x * cw;
                                    double py = oy + ch / 2.0 + it->y * cw;
                                    double dx = px - pt.x;
                                    double dy = py - pt.y;
                                    if (dx * dx + dy * dy <= 20.0 * 20.0) {
                                        current_paths[pidx].pts.erase(it);
                                        deleted = true;
                                        selected_path = pidx;
                                        needs_update = true;
                                        needs_save = true;
                                        break;
                                    }
                                }
                                if (deleted) break;
                            }
                        }
                    }
                }
            }

            if (needs_update) {
                InvalidateRect(hwndDlg, NULL, FALSE);
                UpdateWindow(hwndDlg);
                
                if (fp->exfunc && fp->exfunc->filter_window_update) {
                    fp->exfunc->filter_window_update(fp);
                }
                
                if (needs_save) {
                    SaveDatabase();
                    // 強制的に未保存マーク(*)を出す魔法の合図
                    if (fp->hwnd) {
                        SendMessage(fp->hwnd, WM_COMMAND, MAKEWPARAM(0, EN_CHANGE), 0);
                    }
                }
            }
            
            if (uMsg == WM_MOUSEMOVE && g_dragging_idx == -1) {
                return FALSE;
            }
            return TRUE;
        }

        case WM_CLOSE:
            ShowWindow(hwndDlg, SW_HIDE);
            if (fp) {
                fp->check[2] = 0;
                g_last_show_check = 0;
                if (fp->exfunc && fp->exfunc->filter_window_update) {
                    fp->exfunc->filter_window_update(fp);
                }
            }
            return TRUE;
    }
    return FALSE;
}

BOOL func_init(FILTER *fp) {
    LoadDatabase();
    return TRUE;
}

BOOL func_exit(FILTER *fp) {
    if (g_hwndDlg && IsWindow(g_hwndDlg)) {
        DestroyWindow(g_hwndDlg);
    }
    g_hwndDlg = NULL;
    return TRUE;
}

BOOL func_update(FILTER *fp, int status) {
    // 拡張編集等で別のオブジェクトが選択された場合の切り替え処理
    if (g_last_fp != fp) {
        g_last_fp = fp;
        g_last_show_check = -1; // 強制的に表示状態を再評価
        if (g_hwndDlg) {
            SetWindowLongPtr(g_hwndDlg, DWLP_USER, (LONG_PTR)fp);
            InvalidateRect(g_hwndDlg, NULL, FALSE);
        }
    }

    // 拡張編集の「Show Canvas」チェックボックスの監視 (常時監視方式)
    if (fp->check[2] != g_last_show_check) {
        g_last_show_check = fp->check[2];
        if (fp->check[2]) {
            if (!g_hwndDlg || !IsWindow(g_hwndDlg)) {
                g_hwndDlg = CreateDialogParam(fp->dll_hinst, MAKEINTRESOURCE(100), NULL, DialogProc, (LPARAM)fp);
            }
            if (g_hwndDlg) {
                SetWindowLongPtr(g_hwndDlg, DWLP_USER, (LONG_PTR)fp);
                ShowWindow(g_hwndDlg, SW_SHOW);
                SetForegroundWindow(g_hwndDlg);
            }
        } else {
            if (g_hwndDlg && IsWindowVisible(g_hwndDlg)) {
                ShowWindow(g_hwndDlg, SW_HIDE);
            }
        }
    }

    // 拡張編集の「New / Clear」チェックボックスの監視 (ボタン的挙動)
    if (fp->check[3]) {
        fp->check[3] = 0; // 自動でチェックを外す
        int new_id = GenerateNewID();
        fp->track[4] = new_id;
        
        g_db[new_id].clear();
        g_sel_map[new_id] = 0;
        
        SaveDatabase();
        if (g_hwndDlg && IsWindowVisible(g_hwndDlg)) InvalidateRect(g_hwndDlg, NULL, FALSE);
        if (fp->exfunc) fp->exfunc->filter_window_update(fp);
        if (fp->hwnd) SendMessage(fp->hwnd, WM_COMMAND, MAKEWPARAM(0, EN_CHANGE), 0);
    }

    int current_id = fp->track[4];
    if (current_id == 0) return TRUE;

    auto& current_paths = g_db[current_id];
    int selected_path = g_sel_map[current_id];
    
    // UIスライダーの同期
    if (!g_syncing_ui && selected_path >= 0 && selected_path < (int)current_paths.size()) {
        bool changed = false;
        if (current_paths[selected_path].size != fp->track[0]) changed = true;
        if (current_paths[selected_path].r != fp->track[1]) changed = true;
        if (current_paths[selected_path].g != fp->track[2]) changed = true;
        if (current_paths[selected_path].b != fp->track[3]) changed = true;
        
        bool ui_round = (fp->check[0] != 0);
        bool ui_fill = (fp->check[1] != 0);
        
        if (current_paths[selected_path].is_round != ui_round) changed = true;
        if (current_paths[selected_path].is_fill != ui_fill) changed = true;

        if (changed) {
            current_paths[selected_path].size = fp->track[0];
            current_paths[selected_path].r = fp->track[1];
            current_paths[selected_path].g = fp->track[2];
            current_paths[selected_path].b = fp->track[3];
            current_paths[selected_path].is_round = ui_round;
            current_paths[selected_path].is_fill = ui_fill;
            
            SaveDatabase();
            if (g_hwndDlg && IsWindowVisible(g_hwndDlg)) InvalidateRect(g_hwndDlg, NULL, FALSE);
            if (fp->hwnd) SendMessage(fp->hwnd, WM_COMMAND, MAKEWPARAM(0, EN_CHANGE), 0);
        }
    }
    return TRUE;
}

BOOL func_WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void *editp, FILTER *fp) {
    if (message == WM_FILTER_INIT) {
        if (!g_hwndDlg || !IsWindow(g_hwndDlg)) {
            g_hwndDlg = CreateDialogParam(fp->dll_hinst, MAKEINTRESOURCE(100), hwnd, DialogProc, (LPARAM)fp);
        }
        return TRUE;
    }
    return FALSE;
}

#define TRACK_N 5
static TCHAR *track_name[]    = { (TCHAR*)"Size", (TCHAR*)"Color_R", (TCHAR*)"Color_G", (TCHAR*)"Color_B", (TCHAR*)"[Data ID]" };
static int    track_default[] = { 10, 255, 255, 255, 0 };
static int    track_s[]       = { 1, 0, 0, 0, 0 };
static int    track_e[]       = { 500, 255, 255, 255, 999999 };

#define CHECK_N 4
static TCHAR *check_name[]    = { (TCHAR*)"Round Cap", (TCHAR*)"Fill Inside", (TCHAR*)"Show Canvas", (TCHAR*)"New / Clear" };
static int    check_default[] = { 1, 0, 0, 0 };

FILTER_DLL filter = {
    FILTER_FLAG_EX_INFORMATION | FILTER_FLAG_MAIN_MESSAGE,
    0, 0,
    (TCHAR*)"Path Draw",
    TRACK_N, track_name, track_default, track_s, track_e,
    CHECK_N, check_name, check_default,
    func_proc,
    func_init,
    func_exit,
    func_update,
    func_WndProc,
    NULL, NULL,
    NULL,
    0,
    (TCHAR*)"Path Draw Pro"
};

EXTERN_C __declspec(dllexport) FILTER_DLL* GetFilterTable(void) {
    return &filter;
}